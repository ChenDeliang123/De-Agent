#include "aiagent/agent/sub_agent.hpp"

#include "aiagent/platform/paths.hpp"

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

// 提示词摘要（/agents、task_result 展示用）：最多 80 字节，停在 UTF-8 边界上。
std::string PromptExcerpt(const std::string& prompt, std::size_t max_bytes) {
    if (prompt.size() <= max_bytes) {
        return prompt;
    }
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(prompt[end]) & 0xC0) == 0x80) {
        --end;
    }
    return prompt.substr(0, end) + "...";
}

// 后台任务终态映射：ok → done；其余按超时/取消优先级落到对应终态。
BackgroundState TerminalStateFor(const SubAgentResult& result) {
    if (result.ok) {
        return BackgroundState::kDone;
    }
    if (result.timed_out) {
        return BackgroundState::kTimedOut;
    }
    if (result.cancelled) {
        return BackgroundState::kCancelled;
    }
    return BackgroundState::kFailed;
}

bool ReadTextFile(const std::string& path, std::string* content, std::string* error) {
    std::ifstream input(aiagent::platform::FromUtf8(path), std::ios::binary);
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
    // 白名单过滤 + 剔除派发类工具（task / task_result）：递归派生由深度单独控制，
    // 不允许白名单绕过；结果查询对拿不到 task 的子 Agent 也没有意义。
    composite->Add(std::make_shared<tools::FilteredToolProvider>(
            parent_tools_, definition.tools,
            std::vector<std::string>{tools::kTaskToolName, tools::kTaskResultToolName}));
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

const char* BackgroundStateName(BackgroundState state) {
    switch (state) {
        case BackgroundState::kRunning:
            return "running";
        case BackgroundState::kDone:
            return "done";
        case BackgroundState::kFailed:
            return "failed";
        case BackgroundState::kCancelled:
            return "cancelled";
        case BackgroundState::kTimedOut:
            return "timed_out";
    }
    return "unknown";
}

std::optional<std::string> SubAgentManager::ValidateRequest(const SubAgentRequest& request,
                                                           int depth,
                                                           const AgentDefinition** definition) const {
    if (!settings_.enabled) {
        return std::string("sub-agents are disabled");
    }
    const std::string type = request.agent_type.empty() ? kDefaultAgentType : request.agent_type;
    *definition = FindDefinition(type);
    if (*definition == nullptr) {
        std::string available;
        for (const auto& def : definitions_) {
            if (!available.empty()) {
                available += ", ";
            }
            available += def.name;
        }
        return "unknown agent type: " + type + " (available: " + available + ")";
    }
    if (depth >= settings_.max_depth) {
        return "sub-agent depth limit reached (max_depth=" +
               std::to_string(settings_.max_depth) + ")";
    }
    if (request.prompt.empty()) {
        return std::string("task prompt must not be empty");
    }
    if (runtime_factory_ == nullptr) {
        return std::string("sub-agent runtime factory is not configured");
    }
    return std::nullopt;
}

SubAgentManager::RunIdentity SubAgentManager::MakeRunIdentity(const AgentDefinition& definition,
                                                             const SubAgentRequest& request) {
    const unsigned long long index = ++run_counter_;
    const std::string parent =
            request.parent_session_id.empty() ? std::string("subagent") : request.parent_session_id;
    RunIdentity identity;
    identity.agent_id = definition.name + "#" + std::to_string(index);
    identity.tool_prefix = "sub" + std::to_string(index) + ":";
    identity.child_context =
            parent + "#" + definition.name + "#" + std::to_string(index);
    return identity;
}

SubAgentResult SubAgentManager::Run(const SubAgentRequest& request, int depth) {
    SubAgentResult result;
    const AgentDefinition* definition = nullptr;
    if (const auto error = ValidateRequest(request, depth, &definition)) {
        result.error = *error;
        return result;
    }
    return RunWithIdentity(request, depth, *definition, MakeRunIdentity(*definition, request));
}

SubAgentResult SubAgentManager::RunWithIdentity(const SubAgentRequest& request, int depth,
                                                const AgentDefinition& definition,
                                                const RunIdentity& identity,
                                                std::shared_ptr<MessageInbox> inbox) {
    const auto started = std::chrono::steady_clock::now();
    SubAgentResult result;
    result.agent_id = identity.agent_id;

    if (!AcquireSlot(request.should_cancel, &result)) {
        return result;
    }
    SlotLease slot_guard(this);
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
                         : (!definition.model.empty() ? definition.model
                                                      : settings_.default_model);
    spec.tools = BuildChildTools(definition, depth);
    spec.system_prompt = definition.system_prompt;
    spec.child_context_id = identity.child_context;
    spec.parent_session_id = request.parent_session_id;
    spec.tool_id_prefix = identity.tool_prefix;
    spec.working_directory = request.working_directory;
    spec.max_tool_rounds = settings_.max_rounds;
    // BuildChildTools 只有在 depth+1 < max_depth 时才给子 Agent 挂 task 工具。
    spec.delegation_enabled = depth + 1 < settings_.max_depth;
    spec.on_usage = [usage_totals](const llm::ChatUsage& usage) {
        usage_totals->Add(usage);
    };

    aiagent::DefaultLogger().Log(
            aiagent::LogLevel::kInfo, "sub_agent",
            "event=subagent_start agent_id=" + identity.agent_id + " type=" + definition.name +
                    " depth=" + std::to_string(depth) +
                    " timeout_s=" + std::to_string(timeout_s) + " model=" +
                    (spec.model.empty() ? std::string("inherit") : spec.model) +
                    " prompt_bytes=" + std::to_string(request.prompt.size()));

    // 事件转发：始终统计工具调用次数；开启 stream_events 且有父事件通道时
    // 才把子事件（含归属与前缀化 tool_call_id）写回父会话流。
    const bool forward_events = settings_.stream_events && request.emit != nullptr;
    const AgentEventCallback emit = request.emit;
    AgentEventCallback child_events =
            [emit, forward_events, agent_id = identity.agent_id,
             parent_tool_call_id = request.parent_tool_call_id,
             prefix = identity.tool_prefix, tool_calls](const AgentEvent& event) {
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
        start_event.agent_id = identity.agent_id;
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
                incoming, {}, identity.child_context, child_events, request.working_directory,
                should_cancel, std::move(inbox));
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
        end_event.agent_id = identity.agent_id;
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
            "event=subagent_end agent_id=" + identity.agent_id + " ok=" + (result.ok ? "true" : "false") +
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
    // 校验与身份分配放在调用线程、按输入顺序完成：若让并发线程各自 ++run_counter_，
    // 同批任务的 agent_id 顺序会随调度变化（结果向量按位置返回，id 会与位置错位）。
    struct Planned {
        const AgentDefinition* definition = nullptr;
        RunIdentity identity;
        bool ready = false;
    };
    std::vector<Planned> planned(requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const AgentDefinition* definition = nullptr;
        if (ValidateRequest(requests[i], depth, &definition).has_value()) {
            continue;  // 交给 Run 生成错误结果（校验失败不消耗运行序号）
        }
        planned[i].definition = definition;
        planned[i].identity = MakeRunIdentity(*definition, requests[i]);
        planned[i].ready = true;
    }
    std::vector<std::thread> threads;
    threads.reserve(requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i) {
        threads.emplace_back([this, i, &requests, &results, &planned, depth]() {
            if (planned[i].ready) {
                results[i] = RunWithIdentity(requests[i], depth, *planned[i].definition,
                                             planned[i].identity);
            } else {
                results[i] = Run(requests[i], depth);
            }
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

// 结果表记录：对外只暴露 BackgroundTaskSnapshot。worker 持有 shared_ptr，
// 因此记录被回收后 worker 仍能安全写完最后一次结果。
struct SubAgentManager::BackgroundTask {
    std::string task_id;         // bg-3
    std::string agent_id;        // explorer#7
    std::string agent_type;      // explorer
    std::string session_id;      // 派发它的父会话
    BackgroundState state = BackgroundState::kRunning;
    SubAgentResult result;       // 终态填充
    std::chrono::steady_clock::time_point started;
    std::shared_ptr<std::atomic<bool>> cancel_requested;  // Stop() / Shutdown 置位
    // 纠偏收件箱（M5b）：Steer() 入队，子 runtime 在回合边界排空；运行结束由 runtime 关闭。
    std::shared_ptr<MessageInbox> inbox;
    std::string prompt_excerpt;
    // 结果已就绪并写入本记录：ShutdownBackground 的有界等待以它为收敛点。
    bool delivered = false;
    // 结果已送出（CLI 打印后 ack）：PendingResults 只列未送出的终态结果。
    bool result_reported = false;
    // 结果就绪前持有的线程句柄；worker 在收敛临界区内对它自 detach，
    // 保证记录被回收时不存在"可 join 句柄被析构 → std::terminate"的路径。
    std::thread worker;
};

BackgroundStartResult SubAgentManager::RunBackground(const SubAgentRequest& request, int depth) {
    BackgroundStartResult dispatch;
    const AgentDefinition* definition = nullptr;
    if (const auto error = ValidateRequest(request, depth, &definition)) {
        dispatch.error = *error;
        return dispatch;
    }
    if (settings_.max_background <= 0) {
        dispatch.error = "background tasks are disabled (agents.max_background=0)";
        return dispatch;
    }
    // worker 持 Manager 的强引用：Manager 的生命周期覆盖所有在跑任务。
    // 非 shared 持有的 Manager（如栈对象）无法保证这一点，直接拒绝而不是抛 bad_weak_ptr。
    auto self = weak_from_this().lock();
    if (self == nullptr) {
        dispatch.error = "background dispatch requires a shared SubAgentManager";
        return dispatch;
    }
    auto task = std::make_shared<BackgroundTask>();
    {
        std::lock_guard<std::mutex> lock(background_mutex_);
        if (background_shutdown_->load()) {
            dispatch.error = "sub-agent manager is shutting down";
            return dispatch;
        }
        // 超 max_background 直接拒绝（不排队，见规划 §9 决策 ①）。
        if (background_running_ >= static_cast<std::size_t>(settings_.max_background)) {
            dispatch.error = "background limit reached (max_background=" +
                             std::to_string(settings_.max_background) +
                             "); run this task in the foreground instead";
            return dispatch;
        }
        const RunIdentity identity = MakeRunIdentity(*definition, request);
        task->task_id = "bg-" + std::to_string(++background_counter_);
        task->agent_id = identity.agent_id;
        task->agent_type = definition->name;
        task->session_id = request.parent_session_id;
        task->started = std::chrono::steady_clock::now();
        task->cancel_requested = std::make_shared<std::atomic<bool>>(false);
        task->inbox = std::make_shared<MessageInbox>();
        task->prompt_excerpt = PromptExcerpt(request.prompt, 80);
        ++background_running_;
        background_tasks_[task->task_id] = task;
        // 线程句柄在持锁期间安装：worker 的收敛临界区用同一把锁，保证"自 detach"
        // 不会早于句柄赋值（否则记录里会留下可 join 句柄）。
        try {
            task->worker = std::thread([self, task, request, depth, definition, identity]() {
                SubAgentResult result;
                // 纠偏消息真正被投递（子 Agent 下一轮读到）时才发通知：帧由装配方推给
                // 该会话的活跃流，CLI 据此显示"已纠偏"（受理只是入队，不算投递）。
                if (task->inbox != nullptr) {
                    task->inbox->SetDeliveryCallback([self, task](const std::string& text) {
                        BackgroundNotification notification;
                        notification.kind = BackgroundNotificationKind::kSteered;
                        notification.text = text;
                        notification.task.task_id = task->task_id;
                        notification.task.agent_id = task->agent_id;
                        notification.task.agent_type = task->agent_type;
                        notification.task.session_id = task->session_id;
                        notification.task.state = BackgroundState::kRunning;
                        self->NotifyBackground(notification);
                    });
                }
                try {
                    // 后台任务不继承回合作用域的 emit / should_cancel（回合结束后调用
                    // 即悬垂）；取消来源 = Stop() 或 Manager 关闭。
                    SubAgentRequest background_request = request;
                    background_request.emit = nullptr;
                    auto stop_observed = std::make_shared<std::atomic<bool>>(false);
                    auto shutdown_observed = std::make_shared<std::atomic<bool>>(false);
                    const auto stop_flag = task->cancel_requested;
                    const auto shutdown_flag = self->background_shutdown_;
                    background_request.should_cancel =
                            [stop_flag, shutdown_flag, stop_observed, shutdown_observed]() {
                                // 关闭优先于单任务停止：关闭期间所有任务都取消，
                                // 先判 stop 会把关闭原因误报成 "stopped"。
                                if (shutdown_flag->load()) {
                                    shutdown_observed->store(true);
                                    return true;
                                }
                                if (stop_flag->load()) {
                                    stop_observed->store(true);
                                    return true;
                                }
                                return false;
                            };
                    result = self->RunWithIdentity(background_request, depth, *definition,
                                                   identity, task->inbox);
                    if (!result.ok) {
                        // 覆盖 RunWithIdentity 里的通用 "cancelled"，让停止/关闭原因可读。
                        if (stop_observed->load()) {
                            result.cancelled = true;
                            result.timed_out = false;
                            result.error = "stopped";
                        } else if (shutdown_observed->load()) {
                            result.cancelled = true;
                            result.timed_out = false;
                            result.error = "cancelled: sub-agent manager is shutting down";
                        }
                    }
                } catch (const std::exception& e) {
                    // 任何逃逸异常都必须收敛，否则结果表会永久停在 running。
                    result = SubAgentResult{};
                    result.agent_id = identity.agent_id;
                    result.error = std::string("background task crashed: ") + e.what();
                } catch (...) {
                    result = SubAgentResult{};
                    result.agent_id = identity.agent_id;
                    result.error = "background task crashed";
                }
                self->ConvergeBackground(task, result);
            });
        } catch (const std::exception& e) {
            // 线程创建失败（资源耗尽）：撤销记录，不留"永远 running"的僵尸任务。
            background_tasks_.erase(task->task_id);
            if (background_running_ > 0) {
                --background_running_;
            }
            dispatch.error = std::string("failed to start background worker: ") + e.what();
            return dispatch;
        }
    }
    dispatch.ok = true;
    dispatch.task_id = task->task_id;
    dispatch.agent_id = task->agent_id;
    // 派发通知：装配方（网关/CLI）据此立刻显示"已派发"；state 固定为 running，
    // 与任务是否恰好已经跑完无关（保证 started 先于 finished 送达）。
    BackgroundNotification notification;
    notification.kind = BackgroundNotificationKind::kStarted;
    notification.task.task_id = task->task_id;
    notification.task.agent_id = task->agent_id;
    notification.task.agent_type = task->agent_type;
    notification.task.session_id = task->session_id;
    notification.task.state = BackgroundState::kRunning;
    notification.task.prompt_excerpt = task->prompt_excerpt;
    notification.task.started = task->started;
    NotifyBackground(notification);
    aiagent::DefaultLogger().Log(
            aiagent::LogLevel::kInfo, "sub_agent",
            "event=background_start task_id=" + dispatch.task_id +
                    " agent_id=" + dispatch.agent_id + " type=" + task->agent_type +
                    " session=" + task->session_id +
                    " prompt_bytes=" + std::to_string(request.prompt.size()));
    return dispatch;
}

std::optional<BackgroundTaskSnapshot> SubAgentManager::PollResult(const std::string& id) const {
    std::lock_guard<std::mutex> lock(background_mutex_);
    const BackgroundTask* task = FindBackgroundLocked(id);
    if (task == nullptr) {
        return std::nullopt;
    }
    return SnapshotLocked(*task);
}

BackgroundStopOutcome SubAgentManager::Stop(const std::string& id, std::string* message) {
    std::lock_guard<std::mutex> lock(background_mutex_);
    const BackgroundTask* task = FindBackgroundLocked(id);
    if (task == nullptr) {
        if (message != nullptr) {
            *message = "unknown background task: " + id;
        }
        return BackgroundStopOutcome::kNotFound;
    }
    if (task->state != BackgroundState::kRunning) {
        if (message != nullptr) {
            *message = task->task_id + " already finished (" +
                       BackgroundStateName(task->state) + ")";
        }
        return BackgroundStopOutcome::kFinished;
    }
    task->cancel_requested->store(true);
    if (message != nullptr) {
        *message = "stopping " + task->task_id + " (" + task->agent_id + ")";
    }
    return BackgroundStopOutcome::kRequested;
}

std::vector<BackgroundTaskSnapshot> SubAgentManager::RunningSnapshot() const {
    std::vector<BackgroundTaskSnapshot> snapshots;
    {
        std::lock_guard<std::mutex> lock(background_mutex_);
        for (const auto& [task_id, task] : background_tasks_) {
            if (task->state == BackgroundState::kRunning) {
                snapshots.push_back(SnapshotLocked(*task));
            }
        }
    }
    // map 按字符串键排序（bg-10 < bg-2），这里恢复成派发顺序。
    std::sort(snapshots.begin(), snapshots.end(),
              [](const BackgroundTaskSnapshot& a, const BackgroundTaskSnapshot& b) {
                  return a.started < b.started;
              });
    return snapshots;
}

std::vector<BackgroundTaskSnapshot> SubAgentManager::BackgroundSnapshot() const {
    std::vector<BackgroundTaskSnapshot> snapshots;
    {
        std::lock_guard<std::mutex> lock(background_mutex_);
        snapshots.reserve(background_tasks_.size());
        for (const auto& [task_id, task] : background_tasks_) {
            snapshots.push_back(SnapshotLocked(*task));
        }
    }
    std::sort(snapshots.begin(), snapshots.end(),
              [](const BackgroundTaskSnapshot& a, const BackgroundTaskSnapshot& b) {
                  return a.started < b.started;
              });
    return snapshots;
}

void SubAgentManager::ShutdownBackground(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(background_mutex_);
    // 取消来源只有关闭标志本身：per-task 的 cancel_requested 只表示用户 Stop，
    // 混用会让终态原因（stopped / shutting down）互相覆盖。
    background_shutdown_->store(true);
    std::size_t cancelled = 0;
    for (const auto& [task_id, task] : background_tasks_) {
        if (!task->delivered) {
            ++cancelled;
        }
    }
    // 收敛点 = delivered：worker 在写结果与终态的同一临界区里置位并发通知。
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (background_running_ > 0 && std::chrono::steady_clock::now() < deadline) {
        background_cv_.wait_until(lock, deadline);
    }
    const std::size_t remaining = background_running_;
    lock.unlock();
    if (remaining > 0) {
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kWarning, "sub_agent",
                "event=background_shutdown_timeout cancelled=" + std::to_string(cancelled) +
                        " remaining=" + std::to_string(remaining) +
                        " (workers keep running detached; not joined)");
    } else {
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kInfo, "sub_agent",
                "event=background_shutdown cancelled=" + std::to_string(cancelled));
    }
}

BackgroundSteerOutcome SubAgentManager::Steer(const std::string& id, const std::string& text,
                                               std::string* message) {
    const auto reject = [message](BackgroundSteerOutcome outcome, const std::string& text_) {
        if (message != nullptr) {
            *message = text_;
        }
        return outcome;
    };

    std::shared_ptr<MessageInbox> inbox;
    std::string task_id;
    std::string finished_message;
    {
        std::lock_guard<std::mutex> lock(background_mutex_);
        BackgroundTask* task = FindBackgroundLocked(id);
        if (task == nullptr) {
            return reject(BackgroundSteerOutcome::kNotFound, "unknown background task: " + id);
        }
        task_id = task->task_id;
        if (task->state != BackgroundState::kRunning) {
            // 已收敛：收件箱已关闭，明确拒绝而不是假装受理。
            return reject(BackgroundSteerOutcome::kFinished,
                          task->task_id + " already finished (" +
                                  BackgroundStateName(task->state) + ")");
        }
        inbox = task->inbox;
    }
    if (inbox == nullptr) {
        return reject(BackgroundSteerOutcome::kNotFound, "unknown background task: " + id);
    }
    const MessageInbox::Limits limits = inbox->LimitsSnapshot();
    if (IsSteerTextEmpty(text)) {
        return reject(BackgroundSteerOutcome::kTooLarge, "steer message is required");
    }
    if (text.size() > limits.max_text_bytes) {
        return reject(BackgroundSteerOutcome::kTooLarge,
                      "steer message is too long (" +
                              std::to_string(limits.max_text_bytes) + " bytes max)");
    }
    // 入队在收件箱自己的锁下完成；运行恰好在此刻收尾时会拿到 kClosed → 归到"已结束"。
    switch (inbox->Push(text)) {
        case MessageInbox::PushResult::kAccepted:
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kInfo, "sub_agent",
                    "event=steer_accepted task_id=" + task_id + " bytes=" +
                            std::to_string(text.size()));
            if (message != nullptr) {
                *message = "steering " + task_id;
            }
            return BackgroundSteerOutcome::kAccepted;
        case MessageInbox::PushResult::kFull:
            return reject(BackgroundSteerOutcome::kFull,
                          "background task inbox is full (" +
                                  std::to_string(limits.max_messages) + ")");
        case MessageInbox::PushResult::kTooLarge:
            return reject(BackgroundSteerOutcome::kTooLarge, "steer message is required");
        case MessageInbox::PushResult::kClosed:
            break;
    }
    return reject(BackgroundSteerOutcome::kFinished, task_id + " already finished");
}

std::vector<BackgroundTaskSnapshot> SubAgentManager::PendingResults() const {
    std::vector<BackgroundTaskSnapshot> snapshots;
    {
        std::lock_guard<std::mutex> lock(background_mutex_);
        for (const auto& [task_id, task] : background_tasks_) {
            if (task->state != BackgroundState::kRunning && !task->result_reported) {
                snapshots.push_back(SnapshotLocked(*task));
            }
        }
    }
    std::sort(snapshots.begin(), snapshots.end(),
              [](const BackgroundTaskSnapshot& a, const BackgroundTaskSnapshot& b) {
                  return a.started < b.started;
              });
    return snapshots;
}

std::size_t SubAgentManager::AcknowledgeResults(const std::vector<std::string>& ids) {
    std::lock_guard<std::mutex> lock(background_mutex_);
    std::size_t acknowledged = 0;
    for (const auto& id : ids) {
        BackgroundTask* task = FindBackgroundLocked(id);
        if (task == nullptr || task->state == BackgroundState::kRunning ||
            task->result_reported) {
            continue;
        }
        task->result_reported = true;
        ++acknowledged;
    }
    return acknowledged;
}

void SubAgentManager::SetBackgroundNotifyCallback(BackgroundNotifyCallback callback) {
    std::lock_guard<std::mutex> lock(background_mutex_);
    background_notify_ = std::move(callback);
}

void SubAgentManager::NotifyBackground(const BackgroundNotification& notification) const {
    BackgroundNotifyCallback callback;
    {
        std::lock_guard<std::mutex> lock(background_mutex_);
        callback = background_notify_;
    }
    if (!callback) {
        return;
    }
    try {
        callback(notification);
    } catch (const std::exception& e) {
        // 推送/注入失败不能拖垮 worker（结果已在结果表里，父仍可用 task_result 取到）。
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kWarning, "sub_agent",
                "event=background_notify_failed task_id=" + notification.task.task_id +
                        " message=" + e.what());
    } catch (...) {
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kWarning, "sub_agent",
                "event=background_notify_failed task_id=" + notification.task.task_id +
                        " message=unknown error");
    }
}

const SubAgentManager::BackgroundTask* SubAgentManager::FindBackgroundLocked(
        const std::string& id) const {
    const auto it = background_tasks_.find(id);
    if (it != background_tasks_.end()) {
        return it->second.get();
    }
    // 也接受 agent_id：CLI 展示用 explorer#7，可直接按它停任务。
    for (const auto& [task_id, task] : background_tasks_) {
        if (task->agent_id == id) {
            return task.get();
        }
    }
    return nullptr;
}

SubAgentManager::BackgroundTask* SubAgentManager::FindBackgroundLocked(const std::string& id) {
    return const_cast<BackgroundTask*>(std::as_const(*this).FindBackgroundLocked(id));
}

BackgroundTaskSnapshot SubAgentManager::SnapshotLocked(const BackgroundTask& task) const {
    BackgroundTaskSnapshot snapshot;
    snapshot.task_id = task.task_id;
    snapshot.agent_id = task.agent_id;
    snapshot.agent_type = task.agent_type;
    snapshot.session_id = task.session_id;
    snapshot.state = task.state;
    snapshot.prompt_excerpt = task.prompt_excerpt;
    snapshot.started = task.started;
    snapshot.result = task.result;
    snapshot.elapsed_ms =
            task.state == BackgroundState::kRunning
                    ? std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - task.started)
                              .count()
                    : task.result.duration_ms;
    return snapshot;
}

void SubAgentManager::ConvergeBackground(const std::shared_ptr<BackgroundTask>& task,
                                         const SubAgentResult& result) {
    BackgroundNotification notification;
    notification.kind = BackgroundNotificationKind::kFinished;
    {
        std::lock_guard<std::mutex> lock(background_mutex_);
        task->result = result;
        task->state = TerminalStateFor(result);
        task->delivered = true;
        notification.task = SnapshotLocked(*task);
        if (task->worker.joinable()) {
            // 结果就绪后立即 detach：记录被回收时不再持有可 join 句柄。
            task->worker.detach();
        }
        if (background_running_ > 0) {
            --background_running_;
        }
        background_completion_order_.push_back(task->task_id);
        while (background_completion_order_.size() > kBackgroundResultRetention) {
            background_tasks_.erase(background_completion_order_.front());
            background_completion_order_.pop_front();
        }
    }
    background_cv_.notify_all();
    aiagent::DefaultLogger().Log(
            result.ok ? aiagent::LogLevel::kInfo : aiagent::LogLevel::kWarning, "sub_agent",
            "event=background_end task_id=" + task->task_id + " agent_id=" + task->agent_id +
                    " state=" + BackgroundStateName(task->state) +
                    " duration_ms=" + std::to_string(result.duration_ms) +
                    " prompt_tokens=" + std::to_string(result.prompt_tokens) +
                    " completion_tokens=" + std::to_string(result.completion_tokens) +
                    " cache_hit_tokens=" + std::to_string(result.cache_hit_tokens));
    // 收敛通知在结果表写入之后：装配方（历史注入/SSE 推送）可能阻塞，不影响等待方。
    NotifyBackground(notification);
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
