#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "a2a/core/version.hpp"
#include "a2a/core/id.hpp"
#include "a2a/models/agent_card.hpp"
#include "a2a/models/message.hpp"
#include "a2a/models/requests.hpp"
#include "a2a/models/task.hpp"
#include "a2a/server/agent_http_server.hpp"
#include "a2a/server/agent_server.hpp"
#include "a2a/server/sqlite_task_store.hpp"
#include "a2a/server/task_store.hpp"

#include "aiagent/agent/agent_event.hpp"
#include "aiagent/agent/agent_runtime.hpp"
#include "aiagent/agent/approval_broker.hpp"
#include "aiagent/agent/history_cache.hpp"
#include "aiagent/agent/project_instructions.hpp"
#include "aiagent/agent/session_meta_store.hpp"
#include "aiagent/agent/system_prompt.hpp"
#include "aiagent/app_paths.hpp"
#include "aiagent/config.hpp"
#include "aiagent/llm/deepseek_client.hpp"
#include "aiagent/log.hpp"
#include "aiagent/mcp/mcp_client_bridge.hpp"
#include "aiagent/permissions/bash_policy.hpp"
#include "aiagent/permissions/permission_store.hpp"
#include "aiagent/permissions/project_config.hpp"
#include "aiagent/permissions/trust_store.hpp"
#include "aiagent/tools/composite_tool_provider.hpp"
#include "aiagent/tools/local_tools.hpp"
#include "aiagent/version.hpp"
#include "aiagent/web/web_gateway.hpp"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void OnSignal(int) {
    g_stop_requested = 1;
}

struct AppOptions {
    int port = 18080;
    int web_port = 18081;
    std::string config_path = "conf/conf.ini";
    std::string db_path = "build/db/aiagent_tasks.db";
    std::string mcp_binary;
    std::string mcp_plugins;
    std::string mcp_python_path;
    std::string log_file;
    std::string log_level;
    bool log_console = false;
};

AppOptions ParseOptions(int argc, char** argv) {
    AppOptions options;
    if (const char* value = std::getenv("AIAGENT_PORT")) {
        options.port = std::stoi(value);
    }
    if (const char* value = std::getenv("AIAGENT_WEB_PORT")) {
        options.web_port = std::stoi(value);
    }
    if (const char* value = std::getenv("AIAGENT_CONFIG")) {
        options.config_path = value;
    }
    if (const char* value = std::getenv("AIAGENT_DB")) {
        options.db_path = value;
    }
    if (const char* value = std::getenv("AIAGENT_MCP_BINARY")) {
        options.mcp_binary = value;
    }
    if (const char* value = std::getenv("AIAGENT_MCP_PLUGINS")) {
        options.mcp_plugins = value;
    }
    if (const char* value = std::getenv("AIAGENT_MCP_PYTHON_PATH")) {
        options.mcp_python_path = value;
    }
    if (const char* value = std::getenv("AIAGENT_LOG_FILE")) {
        options.log_file = value;
    }
    if (const char* value = std::getenv("AIAGENT_LOG_LEVEL")) {
        options.log_level = value;
    }
    if (const char* value = std::getenv("AIAGENT_LOG_CONSOLE")) {
        options.log_console = std::string(value) == "1" ||
                              std::string(value) == "true";
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            options.config_path = argv[++i];
        } else if (arg == "--db" && i + 1 < argc) {
            options.db_path = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            options.port = std::stoi(argv[++i]);
        } else if (arg == "--web-port" && i + 1 < argc) {
            options.web_port = std::stoi(argv[++i]);
        } else if (arg == "--mcp-binary" && i + 1 < argc) {
            options.mcp_binary = argv[++i];
        } else if (arg == "--mcp-plugins" && i + 1 < argc) {
            options.mcp_plugins = argv[++i];
        } else if (arg == "--mcp-python-path" && i + 1 < argc) {
            options.mcp_python_path = argv[++i];
        } else if (arg == "--log-file" && i + 1 < argc) {
            options.log_file = argv[++i];
        } else if (arg == "--log-level" && i + 1 < argc) {
            options.log_level = argv[++i];
        } else if (arg == "--log-console") {
            options.log_console = true;
        } else {
            options.port = std::stoi(arg);
        }
    }
    return options;
}

// 缓存优先地取某个会话的最新历史；缓存未命中时回退存储层。
// 选取写入时间最新的任务：手动/自动压缩会写入更短的历史，比较条数会选错。
std::vector<a2a::models::Message> LoadLatestHistory(
        aiagent::agent::HistoryCache& cache, a2a::server::ITaskStore& store,
        const std::string& context_id) {
    std::vector<a2a::models::Message> history;
    if (context_id.empty()) {
        return history;
    }
    if (cache.TryGet(context_id, history)) {
        return history;
    }
    std::optional<a2a::core::TimePoint> latest_time;
    for (const auto& task : store.ListAll()) {
        if (task.context_id != context_id || task.history.empty()) {
            continue;
        }
        if (latest_time.has_value()) {
            if (!task.status.timestamp.has_value() ||
                !(*task.status.timestamp > *latest_time)) {
                continue;
            }
        }
        history = task.history;
        latest_time = task.status.timestamp;
    }
    if (!history.empty()) {
        cache.Put(context_id, history);
    }
    return history;
}

// 从历史中取首条 user 文本作为标题，UTF-8 安全截断（最多 48 字节）。
std::string DeriveShortTitle(const std::vector<a2a::models::Message>& history) {
    for (const auto& message : history) {
        if (message.role != a2a::models::Role::kUser) {
            continue;
        }
        for (const auto& part : message.parts) {
            if (!part.text.has_value() || part.text->empty()) {
                continue;
            }
            const std::string& text = *part.text;
            constexpr std::size_t kMaxBytes = 48;
            if (text.size() <= kMaxBytes) {
                return text;
            }
            std::size_t end = kMaxBytes;
            while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
                --end;
            }
            return text.substr(0, end) + "…";
        }
    }
    return "新对话";
}

a2a::models::AgentCard BuildAgentCard(int port) {
    using a2a::models::AgentCapabilities;
    using a2a::models::AgentCard;
    using a2a::models::AgentInterface;
    using a2a::models::AgentSkill;

    AgentInterface iface;
    iface.url = "http://127.0.0.1:" + std::to_string(port) + "/";
    iface.protocol_binding = std::string(a2a::models::kBindingJsonRpc);
    iface.protocol_version = std::string(a2a::core::kProtocolVersion);

    AgentCapabilities capabilities;
    capabilities.streaming = false;
    capabilities.push_notifications = false;

    AgentSkill skill;
    skill.id = "de-agent";
    skill.name = "de agent";
    skill.description =
            "通用 coding agent：读写文件、执行命令、调用工具。";
    skill.tags = {"coding", "agent", "tools"};

    AgentCard card(aiagent::kAgentName, aiagent::kAgentDescription,
                   aiagent::kVersion);
    card.WithInterface(std::move(iface))
            .WithCapabilities(std::move(capabilities))
            .WithDefaultInputModes({"text/plain"})
            .WithDefaultOutputModes({"text/plain"})
            .WithSkill(std::move(skill));
    return card;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const AppOptions options = ParseOptions(argc, argv);

        std::string config_error;
        const auto config = aiagent::LoadDeepSeekConfigFile(options.config_path, &config_error);
        if (!config.has_value()) {
            std::cerr << "配置错误: " << config_error << '\n';
            return 1;
        }

        aiagent::LogConfig log_config;
        std::string log_error;
        const auto parsed_log = aiagent::LoadLogConfigFile(options.config_path, &log_error);
        if (parsed_log.has_value()) {
            log_config = *parsed_log;
        }
        if (!options.log_file.empty()) {
            log_config.file = options.log_file;
        }
        if (!options.log_level.empty()) {
            log_config.level = options.log_level;
        }
        if (options.log_console) {
            log_config.console = true;
        }
        aiagent::ConfigureDefaultLogger(log_config.level, log_config.file,
                                        log_config.console,
                                        log_config.max_size_bytes,
                                        log_config.max_files);
        aiagent::DefaultLogger().Log(aiagent::LogLevel::kInfo, "main",
                                     "config loaded: " + options.config_path);

        const std::filesystem::path db_path(options.db_path);
        if (db_path.has_parent_path()) {
            std::filesystem::create_directories(db_path.parent_path());
        }

        auto task_store =
                std::make_shared<a2a::server::SqliteTaskStore>(db_path.string());
        // 会话显式标题：与任务库同目录的 JSON 文件。
        std::filesystem::path meta_path = db_path;
        meta_path.replace_filename("aiagent_sessions.json");
        aiagent::agent::SessionMetaStore session_meta(meta_path.string());
        auto model = std::make_shared<aiagent::llm::DeepSeekClient>(*config);
        const std::string coding_system_prompt =
                aiagent::agent::BuildCodingSystemPrompt();
        aiagent::agent::AgentRuntime runtime(model, coding_system_prompt);
        aiagent::InstructionsConfig instructions_config;
        std::string instructions_error;
        const auto parsed_instructions = aiagent::LoadInstructionsConfigFile(
                options.config_path, &instructions_error);
        if (!parsed_instructions.has_value()) {
            std::cerr << "配置错误: " << instructions_error << '\n';
            return 1;
        }
        instructions_config = *parsed_instructions;
        const std::string fallback_working_directory =
                std::filesystem::current_path().string();
        runtime.SetFallbackWorkingDirectory(fallback_working_directory);
        auto instructions_loader =
                std::make_shared<aiagent::agent::ProjectInstructionsLoader>(
                        instructions_config);
        runtime.SetProjectInstructionsLoader(instructions_loader);
        // 最近一次压缩记录（按会话），供 REST 查询/前端展示。
        std::map<std::string, aiagent::agent::CompactionRecord> compaction_records;
        std::mutex compaction_records_mutex;
        // 上下文压缩配置：超阈值时把较早的消息摘要成一条 user 消息。
        aiagent::CompactionConfig compaction_config;
        std::string compaction_error;
        const auto parsed_compaction =
                aiagent::LoadCompactionConfigFile(options.config_path, &compaction_error);
        if (parsed_compaction.has_value()) {
            compaction_config = *parsed_compaction;
        }
        aiagent::agent::CompactionSettings compaction_settings;
        compaction_settings.enabled = compaction_config.enabled;
        compaction_settings.context_window = compaction_config.context_window;
        compaction_settings.reserve_tokens = compaction_config.reserve_tokens;
        compaction_settings.compact_at_tokens = compaction_config.compact_at_tokens;
        compaction_settings.keep_recent_tokens = compaction_config.keep_recent_tokens;
        compaction_settings.min_gain_ratio = compaction_config.min_gain_ratio;
        // 防呆：keep_recent 太贴近阈值会让压缩退化成"压完立刻又压"，启动时纠正并留痕。
        const auto compaction_adjustment =
                aiagent::agent::NormalizeCompactionSettings(&compaction_settings);
        if (compaction_adjustment.changed) {
            aiagent::DefaultLogger().Log(aiagent::LogLevel::kWarning, "main",
                                         "compaction settings adjusted: " +
                                                 compaction_adjustment.reason);
        }
        runtime.SetCompactionSettings(compaction_settings);
        runtime.SetCompactionCallback(
                [&](const std::string& context_id,
                    const aiagent::agent::CompactionRecord& record) {
                    std::lock_guard<std::mutex> lock(compaction_records_mutex);
                    compaction_records[context_id] = record;
                });
        // 会话最新历史内存缓存：避免每次请求 ListAll() 全表扫描（长会话 O(n²)）。
        aiagent::agent::HistoryCache history_cache;

        // 内置 coding 工具（read/write/edit/bash）+ 可选的 MCP 工具，合并为一个 provider。
        // 文件工具的路径范围与能力由「权限模式」逐请求决定（见 permissions 模块）。
        aiagent::ToolsConfig tools_config;
        std::string tools_error;
        const auto parsed_tools =
                aiagent::LoadToolsConfigFile(options.config_path, &tools_error);
        if (parsed_tools.has_value()) {
            tools_config = *parsed_tools;
        }

        // 用户级数据目录与权限存储。
        aiagent::StorageConfig storage_config;
        std::string storage_error;
        if (const auto parsed =
                    aiagent::LoadStorageConfigFile(options.config_path, &storage_error);
            parsed.has_value()) {
            storage_config = *parsed;
        }
        const aiagent::AppPaths app_paths =
                aiagent::ResolveAppPaths(storage_config.data_dir);
        std::string permissions_path = storage_config.permissions_file;
        if (permissions_path.empty()) {
            permissions_path = app_paths.permissions_file;
        }
        if (permissions_path.empty()) {
            std::filesystem::path fallback = db_path;
            fallback.replace_filename("permissions.json");
            permissions_path = fallback.string();
        }
        auto permission_store =
                std::make_shared<aiagent::permissions::PermissionStore>(permissions_path);

        aiagent::PermissionsConfig permissions_config;
        std::string permissions_error;
        if (const auto parsed = aiagent::LoadPermissionsConfigFile(options.config_path,
                                                                  &permissions_error);
            parsed.has_value()) {
            permissions_config = *parsed;
        }
        aiagent::agent::PermissionSettings permission_settings;
        if (const auto mode = aiagent::permissions::ParseMode(permissions_config.default_mode);
            mode.has_value()) {
            permission_settings.default_mode = *mode;
        }
        if (const auto mode = aiagent::permissions::ParseMode(permissions_config.max_mode);
            mode.has_value()) {
            permission_settings.max_mode = *mode;
        }
        if (const auto mode =
                    aiagent::permissions::ParseMode(
                            permissions_config.non_interactive_max_mode);
            mode.has_value()) {
            permission_settings.non_interactive_max_mode = *mode;
        }
        permission_settings.allow_full_access = permissions_config.allow_full_access;
        if (!tools_config.sandbox_enabled) {
            permission_settings.default_mode = aiagent::permissions::PermissionMode::kFull;
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kWarning, "main",
                    "[tools] sandbox_enabled=false is deprecated; default permission "
                    "mode forced to full");
        }
        runtime.SetPermissionStore(permission_store);
        runtime.SetPermissionSettings(permission_settings);
        // 逐次人工确认：Agent 阻塞等待，CLI 通过 /api/v1/chat/approve 唤醒。
        auto approval_broker = std::make_shared<aiagent::agent::ApprovalBroker>();
        runtime.SetApprovalHandler(
                [approval_broker, timeout = permissions_config.approval_timeout_seconds](
                        const aiagent::agent::ApprovalRequest& request) {
                    return approval_broker->Wait(request.context_id, request.tool_call_id,
                                                 std::chrono::seconds(timeout));
                });
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kInfo, "main",
                "permissions file=" + permissions_path + " default_mode=" +
                        aiagent::permissions::ModeName(permission_settings.default_mode) +
                        " allow_full_access=" +
                        (permission_settings.allow_full_access ? "true" : "false"));

        // bash 命令白/黑名单：用户级规则文件；不存在时写入带注释的默认文件。
        std::string bash_policy_path = storage_config.bash_policy_file;
        if (bash_policy_path.empty()) {
            bash_policy_path = app_paths.bash_policy_file;
        }
        if (bash_policy_path.empty()) {
            std::filesystem::path fallback = db_path;
            fallback.replace_filename("bash_policy.rules");
            bash_policy_path = fallback.string();
        }
        aiagent::permissions::BashPolicy bash_policy;
        std::string bash_policy_error;
        if (aiagent::permissions::LoadBashPolicyFile(bash_policy_path, &bash_policy,
                                                     &bash_policy_error)) {
            // 已存在：以文件为准。
        } else if (aiagent::permissions::WriteDefaultBashPolicyFile(bash_policy_path)) {
            bash_policy = aiagent::permissions::DefaultBashPolicy();
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kInfo, "main",
                    "bash policy default file written: " + bash_policy_path);
        } else {
            bash_policy = aiagent::permissions::DefaultBashPolicy();
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kWarning, "main",
                    "using built-in bash policy (cannot write " + bash_policy_path +
                            ")");
        }
        runtime.SetBashPolicy(bash_policy);
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kInfo, "main",
                "bash policy file=" + bash_policy_path + " deny=" +
                        std::to_string(bash_policy.deny.size()) + " allow=" +
                        std::to_string(bash_policy.allow.size()));

        // 项目级 `.de/` 只读分层：未信任的项目只能收紧（P0）。
        auto project_config_cache =
                std::make_shared<aiagent::permissions::ProjectConfigCache>();
        runtime.SetProjectConfigProvider(
                [project_config_cache](const std::string& cwd) {
                    return project_config_cache->Get(cwd);
                });

        // 用户级信任存储：项目 `.de/` 被信任后才允许放松权限。
        std::string trust_path = storage_config.trust_file;
        if (trust_path.empty()) {
            trust_path = app_paths.trust_file;
        }
        if (trust_path.empty()) {
            std::filesystem::path fallback = db_path;
            fallback.replace_filename("trust.json");
            trust_path = fallback.string();
        }
        auto trust_store =
                std::make_shared<aiagent::permissions::TrustStore>(trust_path);
        runtime.SetTrustStore(trust_store);

        auto tools = std::make_shared<aiagent::tools::CompositeToolProvider>();
        tools->Add(std::make_shared<aiagent::tools::LocalCodingTools>(
                std::filesystem::current_path().string(), tools_config.sandbox_roots));
        runtime.SetToolProvider(tools);

        // MCP：优先读 [mcp] 配置；没有时尝试根目录 vendor 的本地构建。
        aiagent::MCPConfig mcp_config;
        std::string mcp_error;
        const auto parsed_mcp = aiagent::LoadMCPConfigFile(options.config_path, &mcp_error);
        if (parsed_mcp.has_value() && parsed_mcp->enabled) {
            mcp_config = *parsed_mcp;
        } else {
            mcp_config.binary = options.mcp_binary.empty()
                                        ? "vendor/mcp-extension-platform/build/mcp_server/mcp_server"
                                        : options.mcp_binary;
            mcp_config.plugins_dir = options.mcp_plugins.empty()
                                             ? "vendor/mcp-extension-platform/mcp_server/plugins"
                                             : options.mcp_plugins;
            if (!options.mcp_python_path.empty()) {
                mcp_config.python_path = options.mcp_python_path;
            }
            mcp_config.enabled = std::filesystem::exists(mcp_config.binary);
        }

        auto mcp_bridge = std::make_shared<aiagent::mcp::MCPClientBridge>();
        if (mcp_config.enabled) {
            try {
                mcp_bridge->Start(mcp_config);
                tools->Add(mcp_bridge);
                std::cout << "MCP tools loaded: " << mcp_bridge->ListTools().size() << '\n';
            } catch (const std::exception& e) {
                std::cerr << "MCP disabled: " << e.what() << '\n';
                mcp_bridge.reset();
            }
        } else {
            std::cout << "MCP disabled (binary not found)\n";
        }

        a2a::server::AgentServer agent_server(BuildAgentCard(options.port), task_store);

        agent_server.task_manager().SetMessageHandler(
                [&](const a2a::models::SendMessageRequest& request) {
                    std::vector<a2a::models::Message> history =
                            LoadLatestHistory(history_cache, *task_store,
                                              request.message.context_id);
                    history.push_back(request.message);
                    auto response = runtime.HandleMessage(
                            request.message, history, request.message.context_id);
                    if (!request.message.context_id.empty() && response.IsTask()) {
                        // 响应携带的 task.history 即本次落库内容，同步更新缓存。
                        history_cache.Put(request.message.context_id,
                                          response.GetTask().history);
                    }
                    return response;
                });

        aiagent::web::WebGateway web_gateway(agent_server, *task_store);
        web_gateway.SetHistoryCache(&history_cache);
        // 手动压缩：压缩后把更短的历史作为新任务落库，并刷新缓存。
        web_gateway.SetCompactHandler(
                [&](const std::string& session_id, const std::string& cwd,
                    std::string* error) {
                    const std::string effective_cwd =
                            !cwd.empty()
                                    ? cwd
                                    : session_meta.GetCwd(session_id).value_or("");
                    if (!effective_cwd.empty()) {
                        session_meta.SetCwd(session_id, effective_cwd);
                    }
                    std::vector<a2a::models::Message> history =
                            LoadLatestHistory(history_cache, *task_store, session_id);
                    if (history.empty()) {
                        if (error != nullptr) {
                            *error = "session not found";
                        }
                        return false;
                    }
                    std::vector<a2a::models::Message> compacted;
                    aiagent::agent::CompactionRecord record;
                    std::string compact_error;
                    if (!runtime.CompactHistory(session_id, history, &compacted,
                                                &record, &compact_error,
                                                effective_cwd)) {
                        if (error != nullptr) {
                            *error = compact_error;
                        }
                        return false;
                    }
                    a2a::models::Task task;
                    task.id = std::string(a2a::core::NewUuid());
                    task.context_id = session_id;
                    task.status = a2a::models::MakeTaskStatus(
                            a2a::models::TaskState::kCompleted);
                    task.history = compacted;
                    history_cache.Put(session_id, std::move(compacted));
                    task_store->Put(std::move(task));
                    aiagent::DefaultLogger().Log(
                            aiagent::LogLevel::kInfo, "main",
                            "session=" + session_id + " event=manual_compaction tokens_before=" +
                                    std::to_string(record.tokens_before) +
                                    " tokens_after=" +
                                    std::to_string(record.tokens_after) +
                                    " messages_before=" +
                                    std::to_string(record.messages_before) +
                                    " messages_after=" +
                                    std::to_string(record.messages_after));
                    return true;
                });
        web_gateway.SetCompactionInfoHandler(
                [&](const std::string& session_id,
                    aiagent::agent::CompactionRecord* record) {
                    std::lock_guard<std::mutex> lock(compaction_records_mutex);
                    const auto it = compaction_records.find(session_id);
                    if (it == compaction_records.end()) {
                        return false;
                    }
                    *record = it->second;
                    return true;
                });
        web_gateway.SetSessionMetaStore(&session_meta);
        web_gateway.SetSessionDetailsHandler(
                [&, instructions_loader, fallback_working_directory](
                        const std::string& session_id, std::string* cwd,
                        std::vector<aiagent::web::InstructionSourceInfo>* instructions) {
                    if (cwd == nullptr || instructions == nullptr) {
                        return false;
                    }
                    const auto stored_cwd = session_meta.GetCwd(session_id);
                    if (!stored_cwd.has_value()) {
                        return false;
                    }
                    *cwd = *stored_cwd;
                    const auto loaded = instructions_loader->Load(
                            *stored_cwd, fallback_working_directory);
                    instructions->reserve(loaded.files.size());
                    for (const auto& file : loaded.files) {
                        instructions->push_back(aiagent::web::InstructionSourceInfo{
                                file.path, file.content.size(), file.truncated});
                    }
                    return true;
                });
        // 删除会话：删除其全部任务，并清理缓存/标题/压缩记录。
        web_gateway.SetDeleteHandler(
                [&](const std::string& session_id, std::string* error) {
                    std::vector<std::string> task_ids;
                    for (const auto& task : task_store->ListAll()) {
                        if (task.context_id == session_id) {
                            task_ids.push_back(task.id);
                        }
                    }
                    if (task_ids.empty()) {
                        if (error != nullptr) {
                            *error = "session not found";
                        }
                        return false;
                    }
                    for (const auto& task_id : task_ids) {
                        task_store->Delete(task_id);
                    }
                    history_cache.Remove(session_id);
                    session_meta.Remove(session_id);
                    {
                        std::lock_guard<std::mutex> lock(compaction_records_mutex);
                        compaction_records.erase(session_id);
                    }
                    aiagent::DefaultLogger().Log(
                            aiagent::LogLevel::kInfo, "main",
                            "session=" + session_id + " event=session_deleted tasks=" +
                                    std::to_string(task_ids.size()));
                    return true;
                });
        // 复制会话：以最新完整历史创建新会话（/clone、/fork）。
        web_gateway.SetCloneHandler(
                [&](const std::string& source_id, std::string* new_id,
                    std::string* error) {
                    std::vector<a2a::models::Message> history =
                            LoadLatestHistory(history_cache, *task_store, source_id);
                    if (history.empty()) {
                        if (error != nullptr) {
                            *error = "session not found";
                        }
                        return false;
                    }
                    const std::string target = std::string(a2a::core::NewUuid());
                    a2a::models::Task task;
                    task.id = std::string(a2a::core::NewUuid());
                    task.context_id = target;
                    task.status = a2a::models::MakeTaskStatus(
                            a2a::models::TaskState::kCompleted);
                    task.history = history;
                    task_store->Put(std::move(task));
                    history_cache.Put(target, history);
                    std::string base;
                    if (auto stored = session_meta.GetTitle(source_id);
                        stored.has_value()) {
                        base = *stored;
                    } else {
                        base = DeriveShortTitle(history);
                    }
                    session_meta.SetTitle(target, base + " (copy)");
                    if (auto source_cwd = session_meta.GetCwd(source_id);
                        source_cwd.has_value()) {
                        session_meta.SetCwd(target, *source_cwd);
                    }
                    if (new_id != nullptr) {
                        *new_id = target;
                    }
                    aiagent::DefaultLogger().Log(
                            aiagent::LogLevel::kInfo, "main",
                            "session=" + source_id + " event=session_cloned target=" +
                                    target);
                    return true;
                });
        // 工具清单：内置 coding 工具 + MCP 工具。
        web_gateway.SetToolsHandler([&]() {
            std::vector<aiagent::web::ToolInfo> tools_info;
            for (const auto& spec : tools->ListTools()) {
                tools_info.push_back({spec.name, spec.description});
            }
            return tools_info;
        });
        // 权限模式：按工作路径查询/设置/重置，供 CLI /permissions 使用。
        web_gateway.SetPermissionsListHandler([&]() {
            aiagent::web::PermissionsSnapshot snapshot;
            snapshot.default_mode =
                    aiagent::permissions::ModeName(permission_settings.default_mode);
            for (const auto& [path, mode] : permission_store->List()) {
                snapshot.entries.push_back(
                        {path, aiagent::permissions::ModeName(mode)});
            }
            return snapshot;
        });
        web_gateway.SetPermissionResolveHandler(
                [&](const std::string& path, std::string* mode, std::string* source) {
                    if (const auto entry = permission_store->Resolve(path);
                        entry.has_value()) {
                        *mode = aiagent::permissions::ModeName(entry->mode);
                        *source = entry->inherited ? "inherited" : "exact";
                    } else {
                        *mode = aiagent::permissions::ModeName(
                                permission_settings.default_mode);
                        *source = "default";
                    }
                    return true;
                });
        web_gateway.SetPermissionSetHandler(
                [&](const std::string& path, const std::string& mode_text,
                    std::string* effective_mode, std::string* error) {
                    const auto mode = aiagent::permissions::ParseMode(mode_text);
                    if (!mode.has_value()) {
                        if (error != nullptr) {
                            *error = "invalid mode: " + mode_text;
                        }
                        return false;
                    }
                    const auto clamped = aiagent::permissions::ClampMode(
                            *mode, permission_settings.max_mode,
                            permission_settings.allow_full_access);
                    permission_store->Set(path, clamped);
                    if (effective_mode != nullptr) {
                        *effective_mode = aiagent::permissions::ModeName(clamped);
                    }
                    aiagent::DefaultLogger().Log(
                            aiagent::LogLevel::kInfo, "main",
                            "event=permission_changed path=" + path + " mode=" +
                                    aiagent::permissions::ModeName(clamped));
                    return true;
                });
        web_gateway.SetPermissionRemoveHandler(
                [&](const std::string& path, std::string* error) {
                    (void)error;
                    // 幂等：未显式配置时也视为成功（重置为默认）。
                    permission_store->Remove(path);
                    aiagent::DefaultLogger().Log(
                            aiagent::LogLevel::kInfo, "main",
                            "event=permission_reset path=" + path);
                    return true;
                });
        // 项目级 `.de/`：查询 / 信任 / 初始化。
        web_gateway.SetProjectPermissionHandler([&](const std::string& path) {
            aiagent::web::ProjectPermissionInfo info;
            info.path = path;
            info.root = aiagent::permissions::FindProjectRoot(path);
            if (!info.root.empty()) {
                info.has_config = project_config_cache->Get(path) != nullptr;
                info.trusted = trust_store->IsTrusted(info.root);
            }
            return info;
        });
        web_gateway.SetTrustSetHandler(
                [&](const std::string& path, bool trusted, std::string* root,
                    std::string* error) {
                    const std::string project_root =
                            aiagent::permissions::FindProjectRoot(
                                    path, /*fallback_to_cwd=*/true);
                    if (project_root.empty()) {
                        if (error != nullptr) {
                            *error = "no project root for path";
                        }
                        return false;
                    }
                    trust_store->Set(project_root, trusted);
                    if (root != nullptr) {
                        *root = project_root;
                    }
                    aiagent::DefaultLogger().Log(
                            aiagent::LogLevel::kInfo, "main",
                            std::string("event=project_trust path=") + project_root +
                                    " trusted=" + (trusted ? "true" : "false"));
                    return true;
                });
        web_gateway.SetPermissionInitHandler(
                [&](const std::string& path, bool seed_from_global, std::string* root,
                    std::vector<std::string>* created, std::string* error) {
                    const std::string project_root =
                            aiagent::permissions::FindProjectRoot(
                                    path, /*fallback_to_cwd=*/true);
                    if (project_root.empty()) {
                        if (error != nullptr) {
                            *error = "no project root for path";
                        }
                        return false;
                    }
                    const std::filesystem::path de_dir =
                            std::filesystem::path(project_root) / ".de";
                    std::error_code ec;
                    std::filesystem::create_directories(de_dir, ec);
                    if (ec) {
                        if (error != nullptr) {
                            *error = "cannot create " + de_dir.string();
                        }
                        return false;
                    }
                    const auto ensure_file = [&](const std::string& name,
                                                 const std::string& content) {
                        const std::filesystem::path target = de_dir / name;
                        if (std::filesystem::exists(target, ec)) {
                            return true;
                        }
                        std::ofstream output(target, std::ios::trunc);
                        if (!output) {
                            return false;
                        }
                        output << content;
                        if (created != nullptr) {
                            created->push_back(name);
                        }
                        return output.good();
                    };
                    if (!ensure_file("permissions.json",
                                     "{\n  \"version\": 1,\n  \"paths\": {}\n}\n")) {
                        if (error != nullptr) {
                            *error = "cannot write permissions.json";
                        }
                        return false;
                    }
                    std::string bash_content;
                    if (seed_from_global && !bash_policy_path.empty() &&
                        std::filesystem::exists(bash_policy_path, ec)) {
                        std::ifstream input(bash_policy_path);
                        std::stringstream buffer;
                        buffer << input.rdbuf();
                        bash_content = buffer.str();
                    } else {
                        bash_content =
                                aiagent::permissions::DefaultBashPolicyFileContent();
                    }
                    if (!ensure_file("bash_policy.rules", bash_content)) {
                        if (error != nullptr) {
                            *error = "cannot write bash_policy.rules";
                        }
                        return false;
                    }
                    if (!ensure_file(".gitignore", "*\n")) {
                        if (error != nullptr) {
                            *error = "cannot write .gitignore";
                        }
                        return false;
                    }
                    // 新写入的项目配置立即生效。
                    project_config_cache->Clear();
                    if (root != nullptr) {
                        *root = project_root;
                    }
                    aiagent::DefaultLogger().Log(
                            aiagent::LogLevel::kInfo, "main",
                            "event=project_init path=" + project_root);
                    return true;
                });
        // 工具审批：CLI 收到 approval_request 后回传决定，唤醒阻塞中的 Agent。
        web_gateway.SetApproveHandler(
                [approval_broker](const std::string& session_id,
                                  const std::string& tool_call_id,
                                  const std::string& decision, std::string* error) {
                    using aiagent::agent::ApprovalDecision;
                    ApprovalDecision parsed;
                    if (decision == "allow_once") {
                        parsed = ApprovalDecision::kAllowOnce;
                    } else if (decision == "allow_session") {
                        parsed = ApprovalDecision::kAllowSession;
                    } else if (decision == "deny") {
                        parsed = ApprovalDecision::kDeny;
                    } else {
                        if (error != nullptr) {
                            *error = "invalid decision: " + decision;
                        }
                        return false;
                    }
                    return approval_broker->Resolve(session_id, tool_call_id, parsed);
                });
        web_gateway.SetCancelObserver([approval_broker](const std::string& session_id) {
            approval_broker->CancelSession(session_id);
        });
        web_gateway.SetStreamEventHandler(
                [&](const std::string& session_id,
                    const std::string& content,
                    const std::string& cwd,
                    const aiagent::agent::AgentEventCallback& on_event,
                    const aiagent::web::WebGateway::CancelCallback& should_cancel) {
                    if (!cwd.empty()) {
                        session_meta.SetCwd(session_id, cwd);
                    }
                    std::vector<a2a::models::Message> history =
                            LoadLatestHistory(history_cache, *task_store, session_id);

                    a2a::models::Message user = a2a::models::MakeTextMessage(
                            a2a::models::Role::kUser, content, "text/plain");
                    history.push_back(user);

                    const std::string reply = runtime.HandleMessageStreamingEvents(
                            user, history, session_id, on_event, cwd, should_cancel);

                    a2a::models::Task task;
                    task.id = std::string(a2a::core::NewUuid());
                    task.context_id = session_id;
                    task.status =
                            a2a::models::MakeTaskStatus(a2a::models::TaskState::kCompleted);
                    task.history = std::move(history);
                    task.history.push_back(a2a::models::MakeTextMessage(
                            a2a::models::Role::kAgent, reply, "text/plain"));
                    task_store->Put(task);
                    history_cache.Put(session_id, std::move(task.history));
                });
        if (!web_gateway.Start("127.0.0.1", options.web_port)) {
            std::cerr << "failed to start Web Gateway on port " << options.web_port << '\n';
            return 1;
        }

        a2a::server::AgentHttpServer http_server(agent_server);
        if (!http_server.Start("127.0.0.1", options.port)) {
            std::cerr << "failed to start A2A HTTP server on port " << options.port << '\n';
            return 1;
        }

        std::signal(SIGINT, OnSignal);
        std::signal(SIGTERM, OnSignal);

        std::cout << aiagent::kAgentName << " listening on http://127.0.0.1:"
                  << options.port << "/\n";
        std::cout << "Web Gateway: http://127.0.0.1:" << options.web_port << "/\n";
        std::cout << "Agent Card: http://127.0.0.1:" << options.port
                  << "/.well-known/agent-card.json\n";
        std::cout << "Model: " << config->model << '\n';
        std::cout << "Task DB: " << options.db_path << '\n';
        std::cout << "Press Ctrl+C to stop.\n";
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kInfo, "main",
                "listening on " + std::to_string(options.port) + " / web " +
                        std::to_string(options.web_port));

        while (!g_stop_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        http_server.Stop();
        web_gateway.Stop();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "aiagent_app failed: " << e.what() << '\n';
        aiagent::DefaultLogger().Log(aiagent::LogLevel::kFatal, "main",
                                     std::string("fatal: ") + e.what());
        return 1;
    }
}
