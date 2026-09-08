#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
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
#include "aiagent/agent/history_cache.hpp"
#include "aiagent/config.hpp"
#include "aiagent/llm/deepseek_client.hpp"
#include "aiagent/mcp/mcp_client_bridge.hpp"
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
        } else {
            options.port = std::stoi(arg);
        }
    }
    return options;
}

// 缓存优先地取某个会话的最新完整历史；缓存未命中时回退存储层（允许一次
// 全表扫描），并在结果非空时回填缓存，避免冷启动反复扫描。
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
    std::size_t latest_history_size = 0;
    for (const auto& task : store.ListAll()) {
        if (task.context_id == context_id && task.history.size() > latest_history_size) {
            history = task.history;
            latest_history_size = task.history.size();
        }
    }
    if (!history.empty()) {
        cache.Put(context_id, history);
    }
    return history;
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
    skill.id = "deepseek-chat";
    skill.name = "DeepSeek Chat";
    skill.description = "多轮对话，使用 DeepSeek 模型回复用户消息。";
    skill.tags = {"chat", "deepseek"};

    AgentCard card("ai-agent-cpp DeepSeek Agent",
                   "A2A agent backed by DeepSeek Chat Completions.",
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

        const std::filesystem::path db_path(options.db_path);
        if (db_path.has_parent_path()) {
            std::filesystem::create_directories(db_path.parent_path());
        }

        auto task_store =
                std::make_shared<a2a::server::SqliteTaskStore>(db_path.string());
        auto model = std::make_shared<aiagent::llm::DeepSeekClient>(*config);
        const std::string coding_system_prompt =
                "You are a terminal coding agent implemented in C++. You help the user by "
                "reading, editing, and writing files and running bash commands. Use the read "
                "tool to inspect files before editing them. After making changes, summarize "
                "what you did concisely.";
        aiagent::agent::AgentRuntime runtime(model, coding_system_prompt);
        // 会话最新历史内存缓存：避免每次请求 ListAll() 全表扫描（长会话 O(n²)）。
        aiagent::agent::HistoryCache history_cache;

        // 内置 coding 工具（read/write/edit/bash）+ 可选的 MCP 工具，合并为一个 provider。
        auto tools = std::make_shared<aiagent::tools::CompositeToolProvider>();
        tools->Add(std::make_shared<aiagent::tools::LocalCodingTools>(
                std::filesystem::current_path().string()));
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
        web_gateway.SetStreamEventHandler(
                [&](const std::string& session_id,
                    const std::string& content,
                    const aiagent::agent::AgentEventCallback& on_event) {
                    std::vector<a2a::models::Message> history =
                            LoadLatestHistory(history_cache, *task_store, session_id);

                    a2a::models::Message user = a2a::models::MakeTextMessage(
                            a2a::models::Role::kUser, content, "text/plain");
                    history.push_back(user);

                    const std::string reply = runtime.HandleMessageStreamingEvents(
                            user, history, session_id, on_event);

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

        std::cout << "ai-agent-cpp listening on http://127.0.0.1:" << options.port << "/\n";
        std::cout << "Web Gateway: http://127.0.0.1:" << options.web_port << "/\n";
        std::cout << "Agent Card: http://127.0.0.1:" << options.port
                  << "/.well-known/agent-card.json\n";
        std::cout << "Model: " << config->model << '\n';
        std::cout << "Task DB: " << options.db_path << '\n';
        std::cout << "Press Ctrl+C to stop.\n";

        while (!g_stop_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        http_server.Stop();
        web_gateway.Stop();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "aiagent_app failed: " << e.what() << '\n';
        return 1;
    }
}
