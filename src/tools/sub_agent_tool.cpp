#include "aiagent/tools/sub_agent_tool.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "aiagent/agent/sub_agent.hpp"

namespace aiagent::tools {
namespace {

std::string FormatSeconds(long long milliseconds) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1fs", static_cast<double>(milliseconds) / 1000.0);
    return buffer;
}

// 从单个任务对象解析出请求；type 的合法性由调用方统一预检。
// from_batch=true 表示对象是 tasks[] 的一项：逐条 background 会被明确拒绝
// （只有顶层 background 生效，避免"哪些任务在后台"变得难以解释）。
bool ParseTaskItem(const nlohmann::json& item, const std::string& location, bool from_batch,
                   agent::SubAgentRequest* request, std::string* error) {
    if (!item.is_object()) {
        *error = location + " must be an object";
        return false;
    }
    request->prompt = item.value("prompt", std::string());
    if (request->prompt.empty()) {
        *error = location + ".prompt must be non-empty";
        return false;
    }
    request->agent_type = item.value("type", std::string("general"));
    request->model = item.value("model", std::string());
    if (from_batch && item.contains("background")) {
        *error = location + ".background is not supported; pass background at the top level";
        return false;
    }
    if (item.contains("timeout_s") && item["timeout_s"].is_number_integer()) {
        request->timeout_seconds = item["timeout_s"].get<long>();
        if (request->timeout_seconds < 0) {
            *error = location + ".timeout_s must not be negative";
            return false;
        }
    }
    return true;
}

// 结果文本：标题行携带元数据（状态/耗时/工具调用数/token 用量），正文为子 Agent 报告。
std::string FormatResults(const std::vector<agent::SubAgentResult>& results) {
    std::string text;
    for (const auto& result : results) {
        if (!text.empty()) {
            text += "\n---\n";
        }
        text += "[" + result.agent_id + "] ";
        if (result.ok) {
            text += "done in " + FormatSeconds(result.duration_ms) + ", " +
                    std::to_string(result.tool_calls) + " tool calls";
            const long long total_tokens = result.prompt_tokens + result.completion_tokens;
            if (total_tokens > 0) {
                if (total_tokens < 1000) {
                    text += ", " + std::to_string(total_tokens) + " tokens";
                } else {
                    char buffer[32];
                    std::snprintf(buffer, sizeof(buffer), ", %.1fk tokens",
                                  static_cast<double>(total_tokens) / 1000.0);
                    text += buffer;
                }
            }
        } else if (result.timed_out) {
            text += "failed: " + result.error;
        } else if (result.cancelled) {
            text += "cancelled";
        } else {
            text += "failed: " + result.error;
        }
        if (!result.text.empty()) {
            text += "\n" + result.text;
        }
    }
    return text;
}

// 后台任务的一行状态：`bg-3 running (explorer#7, 42.1s so far)`；
// 终态带上失败/取消原因，父据此决定是否重派。
std::string FormatBackgroundStatus(const agent::BackgroundTaskSnapshot& task) {
    std::string line = task.task_id + " " + agent::BackgroundStateName(task.state) + " (" +
                       task.agent_id + ", " + FormatSeconds(task.elapsed_ms);
    if (task.state == agent::BackgroundState::kRunning) {
        line += " so far";
    }
    line += ")";
    if (!task.result.ok && !task.result.error.empty()) {
        line += ": " + task.result.error;
    }
    return line;
}

// 后台派发：逐条受理，成功回句柄，失败带上原因（不静默丢弃任何一条）。
ToolExecutionResult DispatchBackground(
        const std::shared_ptr<agent::SubAgentManager>& manager,
        const std::vector<agent::SubAgentRequest>& requests, int depth) {
    ToolExecutionResult result;
    std::string text;
    std::size_t started = 0;
    for (const auto& request : requests) {
        const agent::BackgroundStartResult dispatch = manager->RunBackground(request, depth);
        if (!text.empty()) {
            text += "\n";
        }
        if (dispatch.ok) {
            ++started;
            text += dispatch.task_id + " started (" + dispatch.agent_id +
                    "); you will be notified when it finishes.";
        } else {
            text += "background dispatch failed: " + dispatch.error;
        }
    }
    if (started > 0) {
        result.ok = true;
        result.text = text;
    } else {
        // 全部未受理：走 error 通道，让模型看到每一条的拒绝原因。
        result.error = text;
    }
    return result;
}

// task_result：省略 task_id 列全部；否则取一条的状态（运行中）或报告（终态）。
ToolExecutionResult CollectBackgroundResult(
        const std::shared_ptr<agent::SubAgentManager>& manager,
        const nlohmann::json& arguments) {
    ToolExecutionResult result;
    const std::string task_id = arguments.value("task_id", std::string());
    if (task_id.empty()) {
        const std::vector<agent::BackgroundTaskSnapshot> tasks = manager->BackgroundSnapshot();
        if (tasks.empty()) {
            result.ok = true;
            result.text = "no background tasks";
            return result;
        }
        std::string text;
        for (const auto& task : tasks) {
            if (!text.empty()) {
                text += "\n";
            }
            text += FormatBackgroundStatus(task);
        }
        result.ok = true;
        result.text = text;
        return result;
    }
    const std::optional<agent::BackgroundTaskSnapshot> task = manager->PollResult(task_id);
    if (!task.has_value()) {
        std::string available;
        for (const auto& candidate : manager->BackgroundSnapshot()) {
            if (!available.empty()) {
                available += ", ";
            }
            available += candidate.task_id + " (" + candidate.agent_id + ", " +
                         agent::BackgroundStateName(candidate.state) + ")";
        }
        result.error = "unknown background task: " + task_id +
                       (available.empty() ? std::string(" (no background tasks yet)")
                                          : " (available: " + available + ")");
        return result;
    }
    if (task->state == agent::BackgroundState::kRunning) {
        result.ok = true;
        result.text = FormatBackgroundStatus(*task);
        return result;
    }
    // 终态：与前台 task 同一条标题行 + 报告正文（重复调用幂等，不消耗内容）。
    result.ok = true;
    result.text = FormatResults({task->result});
    return result;
}

}  // namespace

SubAgentToolProvider::SubAgentToolProvider(std::weak_ptr<agent::SubAgentManager> manager,
                                           int depth)
    : manager_(std::move(manager)), depth_(depth) {}

std::vector<llm::ToolSpec> SubAgentToolProvider::ListTools() {
    using nlohmann::json;

    llm::ToolSpec task;
    task.name = kTaskToolName;
    task.description =
            "Delegate one or more self-contained subtasks to sub-agents. Each sub-agent runs in "
            "a separate context: it sees ONLY the prompt you give it (no conversation history, "
            "no follow-up questions) and returns a compressed report. Use it to parallelize "
            "independent work (exploring separate parts of a codebase, running independent "
            "investigations) or to keep bulky intermediate results out of your own context. "
            "Put every fact the sub-agent needs into the prompt; you will receive only its "
            "final report.";
    const json task_item_properties = {
            {"prompt", {{"type", "string"},
                        {"description", "Self-contained instruction; the sub-agent sees no "
                                        "conversation history."}}},
            {"type", {{"type", "string"},
                      {"description", "Agent definition name, e.g. general or explorer."}}},
            {"model", {{"type", "string"},
                       {"description", "Optional model override for this subtask."}}},
            {"timeout_s", {{"type", "integer"},
                           {"description", "Optional hard timeout in seconds for this subtask."}}}};
    task.parameters = {
            {"type", "object"},
            {"properties",
             {{"prompt", {{"type", "string"},
                          {"description", "Single subtask: self-contained instruction."}}},
              {"type", {{"type", "string"},
                        {"description", "Single subtask: agent definition name (default "
                                        "general)."}}},
              {"model", {{"type", "string"},
                         {"description", "Single subtask: optional model override."}}},
              {"timeout_s", {{"type", "integer"},
                             {"description", "Single subtask: optional hard timeout in "
                                             "seconds."}}},
              {"tasks", {{"type", "array"},
                         {"description", "Batch fan-out: run several subtasks concurrently "
                                         "(max " + std::to_string(kMaxTaskBatch) + ")."},
                         {"items", {{"type", "object"},
                                    {"properties", task_item_properties},
                                    {"required", json::array({"prompt"})}}}}},
              {"background",
               {{"type", "boolean"},
                {"description",
                 "Run in the background; returns a task id immediately. Use for long "
                 "investigations while you keep working. Collect the status or report later "
                 "with task_result. Applies to the whole call (single task or tasks[])."}}}}},
            {"required", json::array()}};

    llm::ToolSpec task_result;
    task_result.name = kTaskResultToolName;
    task_result.description =
            "Check background subtasks started with task(background=true): returns the status "
            "while they run, or the same title line and report the foreground task tool would "
            "have returned once they finish. Omit task_id to list every background task that "
            "has not been recycled yet.";
    task_result.parameters = {
            {"type", "object"},
            {"properties",
             {{"task_id", {{"type", "string"},
                           {"description", "Task id returned by task(background=true). Omit to "
                                           "list all."}}}}},
            {"required", json::array()}};
    return {task, task_result};
}

ToolExecutionResult SubAgentToolProvider::Execute(const std::string& name,
                                                  const nlohmann::json& arguments,
                                                  const ToolContext& context) {
    ToolExecutionResult result;
    if (name != kTaskToolName && name != kTaskResultToolName) {
        result.error = "unknown tool: " + name;
        return result;
    }
    std::shared_ptr<agent::SubAgentManager> manager = manager_.lock();
    if (manager == nullptr || !manager->Enabled()) {
        result.error = "sub-agents are not available";
        return result;
    }
    if (name == kTaskResultToolName) {
        if (arguments.contains("task_id") && !arguments["task_id"].is_string()) {
            result.error = "task_result.task_id must be a string";
            return result;
        }
        return CollectBackgroundResult(manager, arguments);
    }
    if (arguments.contains("background") && !arguments["background"].is_boolean()) {
        result.error = "task.background must be a boolean";
        return result;
    }

    std::vector<agent::SubAgentRequest> requests;
    const bool has_tasks = arguments.contains("tasks") && arguments["tasks"].is_array();
    const bool has_prompt = arguments.contains("prompt");
    if (has_tasks && has_prompt) {
        result.error = "task accepts either prompt or tasks, not both";
        return result;
    }
    if (has_tasks) {
        const auto& items = arguments["tasks"];
        if (items.empty()) {
            result.error = "task requires a non-empty tasks[] array";
            return result;
        }
        if (static_cast<int>(items.size()) > kMaxTaskBatch) {
            result.error = "too many tasks: " + std::to_string(items.size()) + " (max " +
                           std::to_string(kMaxTaskBatch) + ")";
            return result;
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            agent::SubAgentRequest request;
            std::string parse_error;
            if (!ParseTaskItem(items[i], "tasks[" + std::to_string(i) + "]", /*from_batch=*/true,
                               &request, &parse_error)) {
                result.error = parse_error;
                return result;
            }
            requests.push_back(std::move(request));
        }
    } else if (has_prompt) {
        agent::SubAgentRequest request;
        std::string parse_error;
        if (!ParseTaskItem(arguments, "task", /*from_batch=*/false, &request, &parse_error)) {
            result.error = parse_error;
            return result;
        }
        requests.push_back(std::move(request));
    } else {
        result.error = "task requires prompt or a tasks[] array";
        return result;
    }

    // 先做类型预检：任何一个未知 type 都直接拒绝整次调用，不启动部分子 Agent。
    for (const auto& request : requests) {
        const agent::AgentDefinition* definition = manager->FindDefinition(request.agent_type);
        if (definition == nullptr) {
            std::string available;
            for (const auto& candidate : manager->Definitions()) {
                if (!available.empty()) {
                    available += ", ";
                }
                available += candidate.name;
            }
            result.error = "unknown agent type: " + request.agent_type +
                           " (available: " + available + ")";
            return result;
        }
    }

    // 父上下文透传：会话键（审批）、工作目录、事件通道、取消、归属 id。
    for (auto& request : requests) {
        request.parent_session_id = context.session_id;
        request.working_directory = context.working_directory;
        request.parent_tool_call_id = context.tool_call_id;
        request.emit = context.emit;
        request.should_cancel = context.should_cancel;
    }

    if (arguments.value("background", false)) {
        // 后台：立即返回句柄（emit/should_cancel 由 Manager 丢弃，不绑回合作用域）。
        return DispatchBackground(manager, requests, depth_);
    }

    const std::vector<agent::SubAgentResult> results =
            manager->RunBatch(requests, depth_);
    const std::string text = FormatResults(results);

    bool any_ok = false;
    for (const auto& sub_result : results) {
        any_ok = any_ok || sub_result.ok;
    }
    result.ok = any_ok;
    if (any_ok) {
        result.text = text;
    } else {
        // 全部失败：走 error 通道（Runtime 会以 "tool error: ..." 回给模型），
        // 保证每个子 Agent 的失败原因都能被模型看到。
        result.error = text.empty() ? "all sub-agents failed" : text;
    }
    return result;
}

std::shared_ptr<IToolProvider> MakeSubAgentToolProvider(
        std::weak_ptr<agent::SubAgentManager> manager, int depth) {
    return std::make_shared<SubAgentToolProvider>(std::move(manager), depth);
}

}  // namespace aiagent::tools
