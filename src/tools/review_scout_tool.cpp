#include "aiagent/tools/review_scout_tool.hpp"

#include <json.hpp>

#include <algorithm>
#include <set>
#include <sstream>

#include "aiagent/platform/paths.hpp"
#include "aiagent/review/project_verify_config.hpp"
#include "aiagent/review/review_pack.hpp"
#include "aiagent/review/review_plan.hpp"
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
    prompt << "Complete a delegated primary review assignment.\n\n"
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
           << "- You are the primary reviewer for this bounded slice. Review every "
              "assigned file as if you were the only reviewer for it.\n"
           << "- Read only. NEVER edit/write files, run commands, or use a shell. Do not "
              "write the formal report; the parent owns it.\n"
           << "- Use review_read/review_context/review_git only for assigned files and "
              "their callers/consumers; generic read does not count as review coverage.\n"
           << "- For every assigned file, call review_read (full file) or "
              "review_context(mode=diff/untracked, pack=...) so the backend records a "
              "coverage receipt.\n"
           << "- Return findings that are ready for the parent to verify, not vague "
              "candidates. Each finding MUST have path:line, evidence, impact, trigger, "
              "suggestion, severity (blocker/major/minor), and confidence "
              "(high/medium/low).\n"
           << "- At most " << max_findings
           << " findings. If evidence is incomplete, put it in unverified instead.\n"
           << "- If you cannot cover a file or your output is truncated, say so "
              "explicitly; the parent will treat missing coverage as a failed slice.\n"
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

struct ScoutTaskSpec {
    std::string name;
    std::vector<std::string> files;
    std::vector<std::string> focus;
};

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
            "Fan out bounded read-only primary-review slices to review-scout "
            "sub-agents. Each scout sees only its files/focus, reviews them as the "
            "primary reviewer, cannot write or execute commands, and returns findings "
            "ready for parent verification plus tool-level coverage receipts. The "
            "parent remains the only formal report writer and must re-read cited "
            "path:line evidence before including a finding.";
    tool.prompt_snippet =
            "Delegate 1–4 bounded primary-review slices to review scouts";
    tool.prompt_guidelines = {
            "Use review_scout(mode=main_review, pack=..., group_ids=[...]) after "
            "review_context(mode=plan); do not invent files outside the pack scope.",
            "Scouts are delegated primary reviewers: do not re-read every file assigned "
            "to a successful slice; only re-read each finding's cited path:line/evidence.",
            "Scouts must use review_read/review_context so their coverage is recorded by "
            "the backend; model-claimed coverage is display-only.",
            "If a slice is partial/failed, cover its missing files yourself or write a "
            "blocked final report; failed slices cannot pass the final gate."};
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
             {{"pack", {{"type", "string"},
                        {"description", "Review pack path; required for main_review."}}},
              {"mode",
               {{"type", "string"},
                {"enum", {"main_review", "candidate"}},
                {"description",
                 "main_review = delegated primary slices (default); candidate = "
                 "targeted parent checks only."}}},
              {"group_ids",
               {{"type", "array"},
                {"items", {{"type", "string"}}},
                {"description",
                 "Group ids from review_context(mode=plan); main_review only."}}},
              {"tasks", {{"type", "array"},
                         {"items", task_item},
                         {"description",
                          "Manual bounded slices (max " + std::to_string(max_tasks_) +
                                  "); main_review requires pack scope."}}},
              {"max_findings_per_task",
               {{"type", "integer"},
                {"description", "1–8; default " + std::to_string(kMaxScoutFindings) + "."}}}}}};
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

    const std::string mode = arguments.value("mode", std::string("main_review"));
    if (mode != "main_review" && mode != "candidate") {
        result.error = "review_scout mode must be main_review or candidate";
        return result;
    }
    const bool main_review = mode == "main_review";
    const std::string pack_arg = arguments.value("pack", std::string());
    if (main_review && pack_arg.empty()) {
        result.error = "review_scout mode=main_review requires pack";
        return result;
    }

    std::string pack_path;
    review::ReviewPack pack;
    bool state_ready = false;
    if (!pack_arg.empty()) {
        const auto resolved = aiagent::platform::ResolveCanonicalPath(
                aiagent::platform::FromUtf8(context.working_directory), pack_arg);
        pack_path = aiagent::platform::ToUtf8(resolved);
        std::string pack_error;
        if (!review::LoadReviewPack(pack_path, &pack, &pack_error)) {
            result.error = "cannot load review pack: " + pack_error;
            return result;
        }
        state_ready = true;
    } else if (current_pack_path_) {
        pack_path = current_pack_path_();
        if (!pack_path.empty()) {
            std::string pack_error;
            state_ready = review::LoadReviewPack(pack_path, &pack, &pack_error);
        }
    }
    if (main_review && !state_ready) {
        result.error = "cannot load review pack for mode=main_review";
        return result;
    }

    review::ReviewScope scope;
    if (main_review) {
        scope = review::BuildReviewScope(pack);
        review::ReviewState scope_state;
        std::string state_error;
        review::LoadReviewState(config_, pack.repo_root, pack.review_id, &scope_state,
                                &state_error);
        const auto owned =
                review::ReviewerOwnedTestPaths(scope_state, pack.repo_root);
        const review::ReviewStaleResult stale =
                review::CheckReviewStale(pack, config_, pack.repo_root, owned);
        if (stale.stale) {
            result.error =
                    "review pack is stale; refresh review_context(mode=manifest) "
                    "before review_scout(mode=main_review)";
            return result;
        }
    }

    std::vector<ScoutTaskSpec> specs;
    if (arguments.contains("group_ids")) {
        if (!main_review) {
            result.error = "review_scout group_ids requires mode=main_review";
            return result;
        }
        if (arguments.contains("tasks")) {
            result.error = "review_scout accepts either group_ids or tasks, not both";
            return result;
        }
        if (!arguments["group_ids"].is_array() || arguments["group_ids"].empty()) {
            result.error = "review_scout requires a non-empty group_ids array";
            return result;
        }
        const std::vector<review::ReviewPlanGroup> groups =
                review::BuildReviewPlanGroups(scope.required);
        std::set<std::string> seen_groups;
        for (const auto& value : arguments["group_ids"]) {
            if (!value.is_string()) {
                result.error = "review_scout group_ids must be strings";
                return result;
            }
            const std::string group_id = value.get<std::string>();
            if (!seen_groups.insert(group_id).second) {
                result.error = "duplicate review_scout group id: " + group_id;
                return result;
            }
            const review::ReviewPlanGroup* selected = nullptr;
            for (const auto& group : groups) {
                if (group.group_id == group_id) {
                    selected = &group;
                    break;
                }
            }
            if (selected == nullptr) {
                result.error = "unknown review_scout group id: " + group_id;
                return result;
            }
            specs.push_back({selected->group_id, selected->files, selected->focus});
        }
    } else {
        if (!arguments.contains("tasks") || !arguments["tasks"].is_array()) {
            result.error = "review_scout requires group_ids[] or tasks[]";
            return result;
        }
        const auto& items = arguments["tasks"];
        if (items.empty()) {
            result.error = "review_scout requires a non-empty tasks[] array";
            return result;
        }
        std::set<std::string> required;
        if (main_review) {
            required.insert(scope.required.begin(), scope.required.end());
        }
        std::set<std::string> seen_files;
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
                if (main_review) {
                    const std::string normalized =
                            review::NormalizeRelativePath(file);
                    if (required.count(normalized) == 0) {
                        result.error =
                                "review_scout file is not in pack scope: " + file;
                        return result;
                    }
                    if (!seen_files.insert(normalized).second) {
                        result.error = "review_scout files overlap: " + file;
                        return result;
                    }
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
            specs.push_back({task_name, files, focus});
        }
    }

    if (static_cast<int>(specs.size()) > max_tasks_) {
        result.error = "too many review_scout slices: " + std::to_string(specs.size()) +
                       " (max " + std::to_string(max_tasks_) + ")";
        return result;
    }
    std::size_t total_files = 0;
    for (const auto& spec : specs) {
        total_files += spec.files.size();
    }
    if (main_review && total_files > scope.required.size()) {
        result.error = "review_scout assigned more files than the pack scope";
        return result;
    }

    std::vector<agent::SubAgentRequest> requests;
    std::vector<std::string> names;
    std::vector<review::ReviewCoverageSlice> seeds;
    std::set<std::string> used_slice_ids;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const ScoutTaskSpec& spec = specs[i];
        std::string slice_id = spec.name;
        if (!used_slice_ids.insert(slice_id).second) {
            slice_id = spec.name + "#" + std::to_string(i + 1);
            used_slice_ids.insert(slice_id);
        }
        review::ReviewCoverageSlice seed;
        seed.slice_id = slice_id;
        seed.name = spec.name;
        seed.status = "running";
        seed.assigned_files = Normalized(spec.files);
        seed.started_at = review::LocalTimestampIso();
        seeds.push_back(seed);

        agent::SubAgentRequest request;
        request.agent_type = "review-scout";
        request.prompt =
                BuildAssignment(spec.name, spec.files, spec.focus, max_findings,
                                pack_path);
        request.parent_session_id = context.session_id;
        request.working_directory = context.working_directory;
        request.parent_tool_call_id = context.tool_call_id;
        request.review_slice_id = slice_id;
        request.review_pack_path = pack_path;
        request.emit = context.emit;
        request.should_cancel = context.should_cancel;
        requests.push_back(std::move(request));
        names.push_back(spec.name);
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
        task["mode"] = mode;
        task["status"] = status;
        task["ok"] = status == "done";
        task["partial"] = status != "done";
        task["covered_files"] = covered;
        task["missing_files"] = missing;
        for (auto& finding : task["findings"]) {
            if (!finding.contains("origin")) {
                finding["origin"] = "scout:" + seeds[i].slice_id;
            }
            if (!finding.contains("parent_verified")) {
                finding["parent_verified"] = false;
            }
        }
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
