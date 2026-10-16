#include "aiagent/agent/sub_agent.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "a2a/models/message.hpp"

#include "aiagent/log.hpp"
#include "aiagent/tools/composite_tool_provider.hpp"
#include "aiagent/tools/filtered_tool_provider.hpp"
#include "aiagent/tools/sub_agent_tool.hpp"

namespace aiagent::agent {
namespace {

constexpr const char* kDefaultAgentType = "general";

// 头尾保留式截断：中间省略，保留两端上下文供父模型判断结果质量。
std::string TruncateMiddle(const std::string& text, std::size_t max_chars) {
    if (max_chars == 0 || text.size() <= max_chars) {
        return text;
    }
    const std::size_t head = max_chars / 2;
    const std::size_t tail = max_chars - head;
    const std::size_t dropped = text.size() - max_chars;
    return text.substr(0, head) + "\n... (truncated " + std::to_string(dropped) +
           " chars) ...\n" + text.substr(text.size() - tail);
}

bool ReadTextFile(const std::string& path, std::string* content, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open agent prompt file: " + path;
        }
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    *content = buffer.str();
    return true;
}

bool HasPathSeparator(const std::string& path) {
    return path.find('/') != std::string::npos;
}

std::string JoinPaths(const std::vector<std::string>& paths) {
    std::string joined;
    for (const auto& path : paths) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += path;
    }
    return joined;
}

// 提示词解析：含 '/' 的按显式路径（绝对或相对 cwd）；否则依次在 dirs 中查找。
std::optional<std::string> ReadPromptFile(const std::string& file,
                                          const std::vector<std::string>& dirs,
                                          std::string* error) {
    std::vector<std::string> candidates;
    if (HasPathSeparator(file)) {
        candidates.push_back(file);
    } else {
        for (const auto& dir : dirs) {
            if (!dir.empty()) {
                candidates.push_back(dir + "/" + file);
            }
        }
        if (candidates.empty()) {
            candidates.push_back(file);
        }
    }
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidate, ec)) {
            continue;
        }
        std::string content;
        if (!ReadTextFile(candidate, &content, error)) {
            continue;
        }
        return content;
    }
    if (error != nullptr) {
        *error = "agent prompt file not found: " + file +
                 " (searched: " + JoinPaths(candidates) + ")";
    }
    return std::nullopt;
}

}  // namespace

SubAgentManager::SubAgentManager(AgentsConfig settings, std::vector<AgentDefinition> definitions,
                                 ChildRuntimeFactory runtime_factory,
                                 std::shared_ptr<tools::IToolProvider> parent_tools)
    : settings_(std::move(settings)),
      definitions_(std::move(definitions)),
      runtime_factory_(std::move(runtime_factory)),
      parent_tools_(std::move(parent_tools)) {}

const AgentDefinition* SubAgentManager::FindDefinition(const std::string& name) const {
    for (const auto& definition : definitions_) {
        if (definition.name == name) {
            return &definition;
        }
    }
    return nullptr;
}

bool SubAgentManager::Enabled() const {
    return settings_.enabled && !definitions_.empty() && runtime_factory_ != nullptr &&
           parent_tools_ != nullptr;
}

std::shared_ptr<tools::IToolProvider> SubAgentManager::BuildChildTools(
        const AgentDefinition& definition, int depth) {
    auto composite = std::make_shared<tools::CompositeToolProvider>();
    // 白名单过滤 + 剔除 task：递归派生由深度单独控制，不允许白名单绕过。
    composite->Add(std::make_shared<tools::FilteredToolProvider>(
            parent_tools_, definition.tools,
            std::vector<std::string>{tools::kTaskToolName}));
    if (depth + 1 < settings_.max_depth) {
        composite->Add(tools::MakeSubAgentToolProvider(weak_from_this(), depth + 1));
    }
    return composite;
}

bool SubAgentManager::AcquireSlot(const std::function<bool()>& should_cancel,
                                  SubAgentResult* result) {
    std::unique_lock<std::mutex> lock(gate_mutex_);
    const int max_concurrent = settings_.max_concurrent > 0 ? settings_.max_concurrent : 1;
    while (active_ >= max_concurrent) {
        if (should_cancel && should_cancel()) {
            result->cancelled = true;
            result->error = "cancelled while waiting for a sub-agent slot";
            return false;
        }
        gate_cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    ++active_;
    return true;
}

void SubAgentManager::ReleaseSlot() {
    {
        std::lock_guard<std::mutex> lock(gate_mutex_);
        if (active_ > 0) {
            --active_;
        }
    }
    gate_cv_.notify_one();
}

SubAgentResult SubAgentManager::Run(const SubAgentRequest& request, int depth) {
    const auto started = std::chrono::steady_clock::now();
    SubAgentResult result;

    if (!settings_.enabled) {
        result.error = "sub-agents are disabled";
        return result;
    }
    const std::string type = request.agent_type.empty() ? kDefaultAgentType : request.agent_type;
    const AgentDefinition* definition = FindDefinition(type);
    if (definition == nullptr) {
        std::string available;
        for (const auto& def : definitions_) {
            if (!available.empty()) {
                available += ", ";
            }
            available += def.name;
        }
        result.error = "unknown agent type: " + type + " (available: " + available + ")";
        return result;
    }
    if (depth >= settings_.max_depth) {
        result.error = "sub-agent depth limit reached (max_depth=" +
                       std::to_string(settings_.max_depth) + ")";
        return result;
    }
    if (request.prompt.empty()) {
        result.error = "task prompt must not be empty";
        return result;
    }
    if (runtime_factory_ == nullptr) {
        result.error = "sub-agent runtime factory is not configured";
        return result;
    }
    if (!AcquireSlot(request.should_cancel, &result)) {
        return result;
    }
    SlotLease slot_guard(this);

    const unsigned long long index = ++run_counter_;
    const std::string agent_id = definition->name + "#" + std::to_string(index);
    const std::string prefix = "sub" + std::to_string(index) + ":";
    const std::string child_context =
            (request.parent_session_id.empty() ? std::string("subagent")
                                               : request.parent_session_id) +
            "#" + definition->name + "#" + std::to_string(index);
    const long timeout_s =
            request.timeout_seconds > 0 ? request.timeout_seconds : settings_.default_timeout_s;
    const auto deadline = started + std::chrono::seconds(timeout_s > 0 ? timeout_s : 1);

    auto timed_out = std::make_shared<std::atomic<bool>>(false);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto tool_calls = std::make_shared<std::atomic<int>>(0);
    // 用量归集：子 Agent 的模型请求全部经由 on_usage sink 累加到这里。
    struct UsageTotals {
        std::mutex mutex;
        long long prompt = 0;
        long long completion = 0;
        long long cache_hit = 0;

        void Add(const llm::ChatUsage& usage) {
            std::lock_guard<std::mutex> lock(mutex);
            prompt += usage.prompt_tokens;
            completion += usage.completion_tokens;
            cache_hit += usage.prompt_cache_hit_tokens;
        }
    };
    auto usage_totals = std::make_shared<UsageTotals>();
    const auto should_cancel = [timed_out, cancelled, deadline,
                                base = request.should_cancel]() {
        if (base && base()) {
            cancelled->store(true);
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out->store(true);
            return true;
        }
        return false;
    };

    ChildRuntimeSpec spec;
    spec.model = !request.model.empty()
                         ? request.model
                         : (!definition->model.empty() ? definition->model
                                                       : settings_.default_model);
    spec.tools = BuildChildTools(*definition, depth);
    spec.system_prompt = definition->system_prompt;
    spec.child_context_id = child_context;
    spec.parent_session_id = request.parent_session_id;
    spec.tool_id_prefix = prefix;
    spec.working_directory = request.working_directory;
    spec.max_tool_rounds = settings_.max_rounds;
    // BuildChildTools 只有在 depth+1 < max_depth 时才给子 Agent 挂 task 工具。
    spec.delegation_enabled = depth + 1 < settings_.max_depth;
    spec.on_usage = [usage_totals](const llm::ChatUsage& usage) {
        usage_totals->Add(usage);
    };

    result.agent_id = agent_id;
    aiagent::DefaultLogger().Log(
            aiagent::LogLevel::kInfo, "sub_agent",
            "event=subagent_start agent_id=" + agent_id + " type=" + definition->name +
                    " depth=" + std::to_string(depth) +
                    " timeout_s=" + std::to_string(timeout_s) + " model=" +
                    (spec.model.empty() ? std::string("inherit") : spec.model) +
                    " prompt_bytes=" + std::to_string(request.prompt.size()));

    // 事件转发：始终统计工具调用次数；开启 stream_events 且有父事件通道时
    // 才把子事件（含归属与前缀化 tool_call_id）写回父会话流。
    const bool forward_events = settings_.stream_events && request.emit != nullptr;
    const AgentEventCallback emit = request.emit;
    AgentEventCallback child_events =
            [emit, forward_events, agent_id, parent_tool_call_id = request.parent_tool_call_id,
             prefix, tool_calls](const AgentEvent& event) {
                if (event.kind == AgentEventKind::kToolStart) {
                    tool_calls->fetch_add(1);
                }
                if (!forward_events) {
                    return;
                }
                AgentEvent outbound = event;
                outbound.agent_id = agent_id;
                outbound.parent_tool_call_id = parent_tool_call_id;
                if (!outbound.tool_call_id.empty()) {
                    outbound.tool_call_id = prefix + outbound.tool_call_id;
                }
                emit(outbound);
            };

    // 子 Agent 生命周期事件（CLI 折叠展示与耗时统计用；受 stream_events 控制）。
    if (forward_events) {
        AgentEvent start_event;
        start_event.kind = AgentEventKind::kSubAgentStart;
        start_event.agent_id = agent_id;
        start_event.parent_tool_call_id = request.parent_tool_call_id;
        emit(start_event);
    }

    try {
        std::shared_ptr<AgentRuntime> runtime = runtime_factory_(spec);
        if (runtime == nullptr) {
            throw std::runtime_error("sub-agent runtime factory returned null");
        }
        const a2a::models::Message incoming =
                a2a::models::MakeTextMessage(a2a::models::Role::kUser, request.prompt);
        const std::string reply = runtime->HandleMessageStreamingEvents(
                incoming, {}, child_context, child_events, request.working_directory,
                should_cancel);
        result.ok = true;
        result.text = TruncateMiddle(reply, settings_.result_max_chars);
    } catch (const std::exception& e) {
        result.ok = false;
        const std::string what = e.what();
        if (timed_out->load()) {
            result.timed_out = true;
            result.error = "timeout after " + std::to_string(timeout_s) + "s";
        } else if (cancelled->load() || what == "task interrupted") {
            result.cancelled = true;
            result.error = "cancelled";
        } else {
            result.error = what;
        }
    } catch (...) {
        result.ok = false;
        result.error = "sub-agent failed with an unknown error";
    }

    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
    result.tool_calls = tool_calls->load();
    {
        std::lock_guard<std::mutex> lock(usage_totals->mutex);
        result.prompt_tokens = usage_totals->prompt;
        result.completion_tokens = usage_totals->completion;
        result.cache_hit_tokens = usage_totals->cache_hit;
    }
    if (forward_events) {
        AgentEvent end_event;
        end_event.kind = AgentEventKind::kSubAgentEnd;
        end_event.agent_id = agent_id;
        end_event.parent_tool_call_id = request.parent_tool_call_id;
        end_event.ok = result.ok;
        end_event.duration_ms = static_cast<int>(result.duration_ms);
        end_event.tool_calls = result.tool_calls;
        end_event.prompt_tokens = result.prompt_tokens;
        end_event.completion_tokens = result.completion_tokens;
        end_event.cache_hit_tokens = result.cache_hit_tokens;
        end_event.text = result.ok ? std::string() : result.error;
        emit(end_event);
    }
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.runs += 1;
        stats_.total_ms += result.duration_ms;
        stats_.prompt_tokens += result.prompt_tokens;
        stats_.completion_tokens += result.completion_tokens;
        stats_.cache_hit_tokens += result.cache_hit_tokens;
        if (!result.ok) {
            stats_.failures += 1;
        }
        if (result.cancelled) {
            stats_.cancelled += 1;
        }
    }
    aiagent::DefaultLogger().Log(
            result.ok ? aiagent::LogLevel::kInfo : aiagent::LogLevel::kWarning, "sub_agent",
            "event=subagent_end agent_id=" + agent_id + " ok=" + (result.ok ? "true" : "false") +
                    " cancelled=" + (result.cancelled ? "true" : "false") +
                    " timed_out=" + (result.timed_out ? "true" : "false") +
                    " duration_ms=" + std::to_string(result.duration_ms) +
                    " tool_calls=" + std::to_string(result.tool_calls) +
                    " prompt_tokens=" + std::to_string(result.prompt_tokens) +
                    " completion_tokens=" + std::to_string(result.completion_tokens) +
                    " cache_hit_tokens=" + std::to_string(result.cache_hit_tokens) +
                    " text_bytes=" + std::to_string(result.text.size()));
    return result;
}

std::vector<SubAgentResult> SubAgentManager::RunBatch(
        const std::vector<SubAgentRequest>& requests, int depth) {
    std::vector<SubAgentResult> results(requests.size());
    if (requests.size() == 1) {
        results[0] = Run(requests[0], depth);
        return results;
    }
    std::vector<std::thread> threads;
    threads.reserve(requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i) {
        threads.emplace_back([this, i, &requests, &results, depth]() {
            results[i] = Run(requests[i], depth);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    return results;
}

SubAgentStats SubAgentManager::Stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

std::optional<AgentDefinitionLoadResult> LoadAgentDefinitions(
        const AgentsConfig& config, const std::vector<std::string>& prompt_dirs,
        std::string* error) {
    std::vector<AgentDefinition> definitions;
    definitions.push_back(AgentDefinition{kDefaultAgentType,
                                         "General-purpose sub-agent with the parent's tools.",
                                         "", "", {}});
    definitions.push_back(AgentDefinition{"explorer",
                                         "Read-only codebase exploration "
                                         "(read/grep/glob/ls).",
                                         "", "", {"read", "grep", "glob", "ls"}});

    std::vector<std::string> prompt_files;
    std::vector<bool> builtin;
    for (const auto& definition : definitions) {
        prompt_files.push_back(definition.name + ".md");
        builtin.push_back(true);
    }

    for (const auto& configured : config.agents) {
        if (configured.name.empty()) {
            if (error != nullptr) {
                *error = "[agents.<name>] requires a non-empty name";
            }
            return std::nullopt;
        }
        auto it = std::find_if(definitions.begin(), definitions.end(),
                               [&configured](const AgentDefinition& definition) {
                                   return definition.name == configured.name;
                               });
        std::size_t index = 0;
        if (it == definitions.end()) {
            definitions.push_back(AgentDefinition{configured.name, configured.description, "",
                                                  configured.model, configured.tools});
            prompt_files.push_back(configured.name + ".md");
            builtin.push_back(false);
            index = definitions.size() - 1;
        } else {
            index = static_cast<std::size_t>(std::distance(definitions.begin(), it));
            AgentDefinition& definition = definitions[index];
            if (!configured.description.empty()) {
                definition.description = configured.description;
            }
            if (!configured.model.empty()) {
                definition.model = configured.model;
            }
            if (!configured.tools.empty()) {
                definition.tools = configured.tools;
            }
            // 显式配置（含对内置定义的覆盖）按「用户定义」对待：缺提示词即报错。
            if (!configured.system_prompt_file.empty()) {
                builtin[index] = false;
            }
        }
        if (!configured.system_prompt_file.empty()) {
            prompt_files[index] = configured.system_prompt_file;
        }
    }

    AgentDefinitionLoadResult result;
    std::vector<AgentDefinition> loaded;
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        std::string read_error;
        const std::optional<std::string> content =
                ReadPromptFile(prompt_files[i], prompt_dirs, &read_error);
        if (!content.has_value()) {
            if (builtin[i]) {
                // 内置定义缺提示词 = 安装不完整：跳过该定义，由调用方决定是否降级。
                result.skipped_builtins.push_back(definitions[i].name);
                continue;
            }
            if (error != nullptr) {
                *error = read_error;
            }
            return std::nullopt;
        }
        if (content->empty()) {
            if (error != nullptr) {
                *error = "agent prompt file is empty: " + prompt_files[i];
            }
            return std::nullopt;
        }
        AgentDefinition definition = definitions[i];
        definition.system_prompt = *content;
        loaded.push_back(std::move(definition));
    }
    result.definitions = std::move(loaded);
    return result;
}

}  // namespace aiagent::agent
