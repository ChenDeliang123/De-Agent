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
bool ParseTaskItem(const nlohmann::json& item, const std::string& location,
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
                                    {"required", json::array({"prompt"})}}}}}}},
            {"required", json::array()}};
    return {task};
}

ToolExecutionResult SubAgentToolProvider::Execute(const std::string& name,
                                                  const nlohmann::json& arguments,
                                                  const ToolContext& context) {
    ToolExecutionResult result;
    if (name != kTaskToolName) {
        result.error = "unknown tool: " + name;
        return result;
    }
    std::shared_ptr<agent::SubAgentManager> manager = manager_.lock();
    if (manager == nullptr || !manager->Enabled()) {
        result.error = "sub-agents are not available";
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
            if (!ParseTaskItem(items[i], "tasks[" + std::to_string(i) + "]",
                               &request, &parse_error)) {
                result.error = parse_error;
                return result;
            }
            requests.push_back(std::move(request));
        }
    } else if (has_prompt) {
        agent::SubAgentRequest request;
        std::string parse_error;
        if (!ParseTaskItem(arguments, "task", &request, &parse_error)) {
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
