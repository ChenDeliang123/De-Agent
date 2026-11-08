#include "aiagent/tools/review_scout_tool.hpp"

#include <json.hpp>

#include <algorithm>
#include <set>
#include <sstream>

#include "aiagent/platform/paths.hpp"
#include "aiagent/review/project_verify_config.hpp"
#include "aiagent/review/review_pack.hpp"
#include "aiagent/review/review_state.hpp"

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
                            int max_findings,
                            const std::string& pack_path) {
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
    if (!pack_path.empty()) {
        prompt << "\n# Review pack\n" << pack_path << "\n";
    }
    prompt << "\n# Rules\n"
           << "- Read only. NEVER edit/write files, run commands, or use a shell.\n"
           << "- Use review_read/review_context/review_git only for assigned files and "
              "their callers/consumers; generic read does not count as review coverage.\n"
           << "- For every assigned file, call review_read (full file) or "
              "review_context(mode=diff/untracked, pack=...) so the backend records a "
              "coverage receipt.\n"
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

review::ReviewUsage UsageFromResult(const agent::SubAgentResult& result) {
    review::ReviewUsage usage;
    usage.prompt_tokens = result.prompt_tokens;
    usage.completion_tokens = result.completion_tokens;
    usage.cache_hit_tokens = result.cache_hit_tokens;
    usage.tool_calls = result.tool_calls;
    usage.requests = result.requests;
    usage.duration_ms = result.duration_ms;
    return usage;
}

nlohmann::json ParseScoutText(const agent::SubAgentResult& result,
                              std::size_t max_result_chars) {
    nlohmann::json task;
    task["ok"] = result.ok;
    task["agent_id"] = result.agent_id;
    task["duration_ms"] = result.duration_ms;
    task["tool_calls"] = result.tool_calls;
    task["requests"] = result.requests;
    task["usage"] = review::ReviewUsageToJson(UsageFromResult(result));
    task["findings"] = nlohmann::json::array();
    task["unverified"] = nlohmann::json::array();
    task["coverage"] = nlohmann::json{{"covered", 0}, {"total", 0}};
    task["model_claimed_coverage"] = task["coverage"];
    task["truncated"] =
            result.truncated ||
            (max_result_chars > 0 && result.text.size() >= max_result_chars);
    if (!result.ok) {
        task["status"] = result.timed_out
                                 ? "timed_out"
                                 : (result.cancelled ? "cancelled" : "failed");
        task["error"] = result.error.empty() ? "review_scout failed" : result.error;
        return task;
    }
    if (result.text.empty()) {
        task["status"] = "invalid";
        task["error"] = "review_scout returned empty output";
        return task;
    }
    try {
        const auto parsed = nlohmann::json::parse(result.text);
        if (!parsed.is_object() || !parsed.contains("findings") ||
            !parsed["findings"].is_array()) {
            task["status"] = "invalid";
            task["error"] = "review_scout output is not a findings JSON object";
            task["raw"] = result.text;
            return task;
        }
        task["findings"] = parsed["findings"];
        if (parsed.contains("unverified") && parsed["unverified"].is_array()) {
            task["unverified"] = parsed["unverified"];
        }
        if (parsed.contains("coverage") && parsed["coverage"].is_object()) {
            task["coverage"] = parsed["coverage"];
            task["model_claimed_coverage"] = parsed["coverage"];
        }
        task["status"] = task["truncated"].get<bool>() ? "truncated" : "done";
    } catch (const nlohmann::json::exception&) {
        task["status"] = "invalid";
        task["error"] = "review_scout output is not valid JSON";
        task["raw"] = result.text;
    }
    return task;
}

std::string PackRelative(const review::ReviewPack& pack, const std::string& abs_path) {
    return aiagent::platform::ToGenericUtf8(
            aiagent::platform::FromUtf8(abs_path).lexically_relative(
                    aiagent::platform::FromUtf8(pack.repo_root)));
}

std::vector<std::string> Normalized(const std::vector<std::string>& paths) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& path : paths) {
        const std::string normalized = review::NormalizeRelativePath(path);
        if (!normalized.empty() && seen.insert(normalized).second) {
            out.push_back(normalized);
        }
    }
    return out;
}

}  // namespace

ReviewScoutToolProvider::ReviewScoutToolProvider(ScoutRunner runner, int max_tasks,
                                                 ReviewConfig config,
                                                 std::function<std::string()> current_pack_path)
    : runner_(std::move(runner)),
      max_tasks_(std::min(std::max(max_tasks, 0), kMaxScoutTasks)),
      config_(std::move(config)),
      current_pack_path_(std::move(current_pack_path)) {}

std::vector<llm::ToolSpec> ReviewScoutToolProvider::ListTools() {
    llm::ToolSpec tool;
    tool.name = kReviewScoutToolName;
    tool.description =
            "Fan out bounded read-only review slices to review-scout sub-agents. "
            "Each scout sees only its files/focus, cannot write or execute commands, "
            "and returns candidate findings plus tool-level coverage receipts. The "
            "parent reviewer must still re-read cited evidence before including a "
            "finding in the final report.";
    tool.prompt_snippet =
            "Delegate 1–4 bounded read-only review slices to review scouts";
    tool.prompt_guidelines = {
            "Use review_scout only for large or independent slices; do not send the whole "
            "branch in one task.",
            "Scouts must use review_read/review_context so their coverage is recorded by "
            "the backend; generic read does not count as coverage.",
            "After review_scout returns, re-read every cited path:line yourself. Scout "
            "findings are candidates, not verified report entries.",
            "If a scout task is partial/failed, its missing assigned files remain "
            "uncovered and must be read by the parent or recorded as blocked."};
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

    std::string pack_path;
    review::ReviewPack pack;
    bool state_ready = false;
    if (current_pack_path_) {
        pack_path = current_pack_path_();
        if (!pack_path.empty()) {
            std::string pack_error;
            state_ready = review::LoadReviewPack(pack_path, &pack, &pack_error);
        }
    }

    std::vector<agent::SubAgentRequest> requests;
    std::vector<std::string> names;
    std::vector<review::ReviewCoverageSlice> seeds;
    std::set<std::string> used_slice_ids;
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
        std::string slice_id = task_name;
        if (!used_slice_ids.insert(slice_id).second) {
            slice_id = task_name + "#" + std::to_string(i + 1);
            used_slice_ids.insert(slice_id);
        }
        review::ReviewCoverageSlice seed;
        seed.slice_id = slice_id;
        seed.name = task_name;
        seed.status = "running";
        seed.assigned_files = Normalized(files);
        seed.started_at = review::LocalTimestampIso();
        seeds.push_back(seed);

        agent::SubAgentRequest request;
        request.agent_type = "review-scout";
        request.prompt = BuildAssignment(task_name, files, focus, max_findings, pack_path);
        request.parent_session_id = context.session_id;
        request.working_directory = context.working_directory;
        request.parent_tool_call_id = context.tool_call_id;
        request.review_slice_id = slice_id;
        request.review_pack_path = pack_path;
        request.emit = context.emit;
        request.should_cancel = context.should_cancel;
        requests.push_back(std::move(request));
        names.push_back(task_name);
    }

    if (state_ready) {
        std::string state_error;
        review::UpdateReviewState(
                config_, pack.repo_root, pack.review_id,
                PackRelative(pack, pack_path),
                [&seeds](review::ReviewState* state) {
                    for (const auto& seed : seeds) {
                        auto* slice = review::FindCoverageSlice(
                                &state->coverage_ledger, seed.slice_id);
                        if (slice == nullptr) {
                            state->coverage_ledger.slices.push_back(seed);
                            continue;
                        }
                        slice->name = seed.name;
                        slice->status = "running";
                        slice->assigned_files = seed.assigned_files;
                        slice->started_at = seed.started_at;
                        slice->finished_at.clear();
                        slice->error.clear();
                        slice->truncated = false;
                        slice->usage = review::ReviewUsage{};
                    }
                },
                &state_error);
    }

    const std::vector<agent::SubAgentResult> results = runner_(requests, 0);
    if (results.size() != requests.size()) {
        result.error = "review_scout runner returned an unexpected result count";
        return result;
    }

    review::ReviewState state;
    bool state_loaded = false;
    if (state_ready) {
        std::string load_error;
        state_loaded = review::LoadReviewState(config_, pack.repo_root, pack.review_id,
                                               &state, &load_error);
    }

    nlohmann::json tasks = nlohmann::json::array();
    std::vector<review::ReviewCoverageSlice> finished;
    review::ReviewUsage total_usage;
    std::size_t success = 0;
    std::size_t failed = 0;
    std::size_t findings = 0;
    for (std::size_t i = 0; i < results.size(); ++i) {
        nlohmann::json task = ParseScoutText(results[i], 8000);
        task["name"] = names[i];
        task["slice_id"] = seeds[i].slice_id;
        task["assigned_files"] = seeds[i].assigned_files;
        std::vector<std::string> covered;
        if (state_loaded) {
            if (const auto* slice = review::FindCoverageSlice(
                        state.coverage_ledger, seeds[i].slice_id);
                slice != nullptr) {
                covered = Normalized(slice->covered_files);
            }
        }
        std::set<std::string> covered_set(covered.begin(), covered.end());
        std::vector<std::string> missing;
        for (const auto& file : seeds[i].assigned_files) {
            if (covered_set.count(file) == 0) {
                missing.push_back(file);
            }
        }
        std::string status = task.value("status", std::string("invalid"));
        if (status == "done") {
            if (task.value("truncated", false)) {
                status = missing.empty() ? "partial" : "truncated";
            } else if (!missing.empty()) {
                status = "partial";
            }
        }
        task["status"] = status;
        task["ok"] = status == "done";
        task["partial"] = status != "done";
        task["covered_files"] = covered;
        task["missing_files"] = missing;
        if (!task.contains("error")) {
            task["error"] = std::string();
        }
        if (!task["findings"].empty()) {
            findings += task["findings"].size();
        }
        if (status == "done") {
            ++success;
        } else {
            ++failed;
        }
        review::ReviewCoverageSlice slice = seeds[i];
        slice.status = status;
        slice.covered_files = covered;
        slice.finished_at = review::LocalTimestampIso();
        slice.error = task.value("error", std::string());
        slice.truncated = task.value("truncated", false);
        if (task.contains("usage") && task["usage"].is_object()) {
            const auto& usage = task["usage"];
            slice.usage.prompt_tokens = usage.value("prompt_tokens", 0LL);
            slice.usage.completion_tokens = usage.value("completion_tokens", 0LL);
            slice.usage.cache_hit_tokens = usage.value("cache_hit_tokens", 0LL);
            slice.usage.tool_calls = usage.value("tool_calls", 0LL);
            slice.usage.requests = usage.value("requests", 0LL);
            slice.usage.duration_ms = usage.value("duration_ms", 0LL);
            total_usage.Add(slice.usage);
        }
        finished.push_back(std::move(slice));
        tasks.push_back(std::move(task));
    }

    if (state_ready) {
        std::string state_error;
        review::UpdateReviewState(
                config_, pack.repo_root, pack.review_id,
                PackRelative(pack, pack_path),
                [&finished](review::ReviewState* latest) {
                    for (const auto& completed : finished) {
                        auto* slice = review::FindCoverageSlice(
                                &latest->coverage_ledger, completed.slice_id);
                        if (slice == nullptr) {
                            latest->coverage_ledger.slices.push_back(completed);
                            continue;
                        }
                        slice->name = completed.name;
                        slice->status = completed.status;
                        slice->assigned_files = completed.assigned_files;
                        slice->finished_at = completed.finished_at;
                        slice->error = completed.error;
                        slice->truncated = completed.truncated;
                        slice->usage = completed.usage;
                        // covered_files/methods 由 review_read/review_context 回执维护；
                        // 不回写模型自报的 coverage。
                    }
                },
                &state_error);
    }

    result.ok = true;
    nlohmann::json payload;
    payload["tasks"] = tasks;
    payload["total_findings"] = findings;
    payload["failed_tasks"] = failed;
    payload["partial"] = failed > 0;
    payload["max_findings_per_task"] = max_findings;
    payload["total_usage"] = review::ReviewUsageToJson(total_usage);
    result.text = payload.dump(2, ' ', false,
                               nlohmann::json::error_handler_t::replace);
    return result;
}

std::shared_ptr<IToolProvider> MakeReviewScoutToolProvider(
        ReviewScoutToolProvider::ScoutRunner runner, int max_tasks,
        ReviewConfig config, std::function<std::string()> current_pack_path) {
    return std::make_shared<ReviewScoutToolProvider>(
            std::move(runner), max_tasks, std::move(config),
            std::move(current_pack_path));
}

}  // namespace aiagent::tools
