#include "aiagent/tools/review_scout_tool.hpp"

#include <json.hpp>

#include <algorithm>
#include <sstream>

#include "aiagent/review/project_verify_config.hpp"

namespace aiagent::tools {
namespace {

constexpr int kMaxScoutTasks = 4;
constexpr int kMaxScoutFiles = 20;
constexpr int kMaxScoutFocus = 8;
constexpr int kMaxScoutFindings = 8;

bool ValidText(const std::string& text, std::size_t max_bytes = 256) {
    return !text.empty() && text.size() <= max_bytes &&
           text.find('\0') == std::string::npos &&
           text.find('\n') == std::string::npos &&
           text.find('\r') == std::string::npos;
}

std::string BuildAssignment(const std::string& name,
                            const std::vector<std::string>& files,
                            const std::vector<std::string>& focus,
                            int max_findings) {
    std::ostringstream prompt;
    prompt << "Complete a read-only review scout assignment.\n\n"
           << "# Assignment\n" << name << "\n\n# Files\n";
    for (const auto& file : files) {
        prompt << "- " << file << "\n";
    }
    prompt << "\n# Focus\n";
    for (const auto& item : focus) {
        prompt << "- " << item << "\n";
    }
    prompt << "\n# Rules\n"
           << "- Read only. NEVER edit/write files, run commands, or use a shell.\n"
           << "- Use read/grep/glob/ls/review_git only; review_git is read-only.\n"
           << "- Read the assigned files and the callers/consumers needed to judge them.\n"
           << "- Each finding MUST have path:line, evidence, impact, trigger, suggestion, "
              "severity (blocker/major/minor), and confidence (high/medium/low).\n"
           << "- At most " << max_findings
           << " findings. If evidence is incomplete, put it in unverified instead.\n"
           << "- Return ONLY a JSON object with this shape:\n"
           << "{\"findings\":[{\"severity\":\"major\",\"file\":\"path\",\"line\":12,"
              "\"title\":\"...\",\"evidence\":\"...\",\"impact\":\"...\","
              "\"trigger\":\"...\",\"suggestion\":\"...\",\"confidence\":\"high\"}],"
              "\"unverified\":[\"...\"],\"coverage\":{\"covered\":1,\"total\":1}}\n";
    return prompt.str();
}

nlohmann::json ParseScoutText(const agent::SubAgentResult& result,
                              std::size_t max_result_chars) {
    nlohmann::json task;
    task["ok"] = result.ok;
    task["agent_id"] = result.agent_id;
    task["duration_ms"] = result.duration_ms;
    task["tool_calls"] = result.tool_calls;
    if (!result.ok) {
        task["error"] = result.error.empty() ? "review_scout failed" : result.error;
        return task;
    }
    task["findings"] = nlohmann::json::array();
    task["unverified"] = nlohmann::json::array();
    task["coverage"] = nlohmann::json{{"covered", 0}, {"total", 0}};
    if (max_result_chars > 0 && result.text.size() >= max_result_chars) {
        task["truncated"] = true;
    }
    try {
        const auto parsed = nlohmann::json::parse(result.text);
        if (!parsed.is_object()) {
            task["raw"] = result.text;
            return task;
        }
        if (parsed.contains("findings") && parsed["findings"].is_array()) {
            task["findings"] = parsed["findings"];
        }
        if (parsed.contains("unverified") && parsed["unverified"].is_array()) {
            task["unverified"] = parsed["unverified"];
        }
        if (parsed.contains("coverage") && parsed["coverage"].is_object()) {
            task["coverage"] = parsed["coverage"];
        }
        if (!parsed.contains("findings") || !parsed["findings"].is_array()) {
            task["raw"] = result.text;
        }
    } catch (const nlohmann::json::exception&) {
        task["raw"] = result.text;
    }
    return task;
}

}  // namespace

ReviewScoutToolProvider::ReviewScoutToolProvider(ScoutRunner runner, int max_tasks)
    : runner_(std::move(runner)), max_tasks_(std::min(std::max(max_tasks, 0), kMaxScoutTasks)) {}

std::vector<llm::ToolSpec> ReviewScoutToolProvider::ListTools() {
    llm::ToolSpec tool;
    tool.name = kReviewScoutToolName;
    tool.description =
            "Fan out bounded read-only review slices to review-scout sub-agents. "
            "Each scout sees only its files/focus, cannot write or execute commands, and "
            "returns candidate findings plus coverage. The parent reviewer must re-read "
            "evidence before including a finding in the final report.";
    tool.prompt_snippet =
            "Delegate 1–4 bounded read-only review slices to review scouts";
    tool.prompt_guidelines = {
            "Use review_scout only for large or independent slices; do not send the whole "
            "branch in one task.",
            "After review_scout returns, re-read every cited path:line yourself. Scout findings "
            "are candidates, not verified report entries.",
            "Scout coverage does not replace review_context(mode=plan) coverage for the parent."};
    const nlohmann::json task_item = {
            {"type", "object"},
            {"properties",
             {{"name", {{"type", "string"}, {"description", "Short slice name"}}},
              {"files", {{"type", "array"},
                         {"items", {{"type", "string"}}},
                         {"description", "Repo-relative files (max 20)"}}},
              {"focus", {{"type", "array"},
                         {"items", {{"type", "string"}}},
                         {"description", "Specific checks/hypotheses (max 8)"}}}}},
            {"required", nlohmann::json::array({"name", "files", "focus"})}};
    tool.parameters = {
            {"type", "object"},
            {"properties",
             {{"tasks", {{"type", "array"},
                         {"items", task_item},
                         {"description",
                          "Bounded review slices (max " + std::to_string(max_tasks_) + ")."}}},
              {"max_findings_per_task",
               {{"type", "integer"},
                {"description", "1–8; default " + std::to_string(kMaxScoutFindings) + "."}}}}},
            {"required", nlohmann::json::array({"tasks"})}};
    return {tool};
}

ToolExecutionResult ReviewScoutToolProvider::Execute(const std::string& name,
                                                     const nlohmann::json& arguments,
                                                     const ToolContext& context) {
    ToolExecutionResult result;
    if (name != kReviewScoutToolName) {
        result.error = "unknown tool: " + name;
        return result;
    }
    if (max_tasks_ <= 0 || !runner_) {
        result.error = "review_scout is disabled; set [review] scout_max to 1–4";
        return result;
    }
    if (!context.policy.allow_read) {
        result.error = "permission denied: read is disabled";
        return result;
    }
    if (!arguments.contains("tasks") || !arguments["tasks"].is_array()) {
        result.error = "review_scout requires tasks[]";
        return result;
    }
    const auto& items = arguments["tasks"];
    if (items.empty()) {
        result.error = "review_scout requires a non-empty tasks[] array";
        return result;
    }
    if (static_cast<int>(items.size()) > max_tasks_) {
        result.error = "too many review_scout tasks: " + std::to_string(items.size()) +
                       " (max " + std::to_string(max_tasks_) + ")";
        return result;
    }
    int max_findings = kMaxScoutFindings;
    if (arguments.contains("max_findings_per_task")) {
        if (!arguments["max_findings_per_task"].is_number_integer()) {
            result.error = "max_findings_per_task must be an integer";
            return result;
        }
        max_findings = arguments["max_findings_per_task"].get<int>();
        if (max_findings < 1 || max_findings > kMaxScoutFindings) {
            result.error = "max_findings_per_task must be 1–8";
            return result;
        }
    }

    std::vector<agent::SubAgentRequest> requests;
    std::vector<std::string> names;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        if (!item.is_object()) {
            result.error = "tasks[" + std::to_string(i) + "] must be an object";
            return result;
        }
        const std::string task_name = item.value("name", std::string());
        if (!ValidText(task_name, 64)) {
            result.error = "tasks[" + std::to_string(i) + "].name must be 1–64 bytes";
            return result;
        }
        if (!item.contains("files") || !item["files"].is_array()) {
            result.error = "tasks[" + std::to_string(i) + "].files must be an array";
            return result;
        }
        if (item["files"].empty() ||
            static_cast<int>(item["files"].size()) > kMaxScoutFiles) {
            result.error = "tasks[" + std::to_string(i) + "].files must have 1–" +
                           std::to_string(kMaxScoutFiles) + " items";
            return result;
        }
        std::vector<std::string> files;
        for (const auto& value : item["files"]) {
            if (!value.is_string()) {
                result.error = "review_scout files must be strings";
                return result;
            }
            const std::string file = value.get<std::string>();
            if (!aiagent::review::IsSafeRelativePath(file, false)) {
                result.error = "review_scout file must be repo-relative: " + file;
                return result;
            }
            files.push_back(file);
        }
        if (!item.contains("focus") || !item["focus"].is_array() ||
            item["focus"].empty() ||
            static_cast<int>(item["focus"].size()) > kMaxScoutFocus) {
            result.error = "tasks[" + std::to_string(i) + "].focus must have 1–" +
                           std::to_string(kMaxScoutFocus) + " items";
            return result;
        }
        std::vector<std::string> focus;
        for (const auto& value : item["focus"]) {
            if (!value.is_string()) {
                result.error = "review_scout focus must be strings";
                return result;
            }
            const std::string text = value.get<std::string>();
            if (!ValidText(text, 256)) {
                result.error = "review_scout focus must be 1–256 bytes";
                return result;
            }
            focus.push_back(text);
        }
        agent::SubAgentRequest request;
        request.agent_type = "review-scout";
        request.prompt = BuildAssignment(task_name, files, focus, max_findings);
        request.parent_session_id = context.session_id;
        request.working_directory = context.working_directory;
        request.parent_tool_call_id = context.tool_call_id;
        request.emit = context.emit;
        request.should_cancel = context.should_cancel;
        requests.push_back(std::move(request));
        names.push_back(task_name);
    }

    const std::vector<agent::SubAgentResult> results = runner_(requests, 0);
    if (results.size() != requests.size()) {
        result.error = "review_scout runner returned an unexpected result count";
        return result;
    }
    nlohmann::json tasks = nlohmann::json::array();
    std::size_t success = 0;
    std::size_t findings = 0;
    for (std::size_t i = 0; i < results.size(); ++i) {
        nlohmann::json task = ParseScoutText(results[i], 8000);
        task["name"] = names[i];
        if (!task["findings"].empty()) {
            findings += task["findings"].size();
        }
        if (results[i].ok) {
            ++success;
        }
        tasks.push_back(std::move(task));
    }
    if (success == 0) {
        result.error = "all review_scout tasks failed";
        return result;
    }
    result.ok = true;
    result.text = nlohmann::json{
            {"tasks", tasks},
            {"total_findings", findings},
            {"max_findings_per_task", max_findings}}
                          .dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    return result;
}

std::shared_ptr<IToolProvider> MakeReviewScoutToolProvider(
        ReviewScoutToolProvider::ScoutRunner runner, int max_tasks) {
    return std::make_shared<ReviewScoutToolProvider>(std::move(runner), max_tasks);
}

}  // namespace aiagent::tools
