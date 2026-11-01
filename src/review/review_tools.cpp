#include "aiagent/review/review_tools.hpp"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include "aiagent/permissions/project_config.hpp"
#include "aiagent/platform/paths.hpp"
#include "aiagent/platform/text_file.hpp"
#include "aiagent/review/project_verify_config.hpp"
#include "aiagent/review/review_process.hpp"
#include "aiagent/review/review_report.hpp"
#include "aiagent/review/review_state.hpp"
#include "aiagent/util/sha256.hpp"

namespace aiagent::review {
namespace {

namespace fs = std::filesystem;

tools::ToolExecutionResult Ok(nlohmann::json payload) {
    tools::ToolExecutionResult result;
    result.ok = true;
    result.text = payload.dump(2);
    return result;
}

tools::ToolExecutionResult Fail(const std::string& message) {
    tools::ToolExecutionResult result;
    result.ok = false;
    result.error = message;
    return result;
}

std::string PackRelative(const std::string& repo_root, const fs::path& pack_path) {
    return aiagent::platform::ToGenericUtf8(
            pack_path.lexically_relative(aiagent::platform::FromUtf8(repo_root)));
}

bool IsUnderReviewDir(const std::string& path, const ReviewConfig& config) {
    const std::string dir = NormalizeRelativePath(config.dir);
    const std::string normalized = NormalizeRelativePath(path);
    return normalized == dir || normalized.rfind(dir + "/", 0) == 0;
}

std::string RepoRootFor(const tools::ToolContext& context) {
    const std::string project = permissions::FindProjectRoot(
            context.working_directory, /*fallback_to_cwd=*/true);
    if (!project.empty()) {
        return project;
    }
    std::error_code ec;
    return aiagent::platform::ToUtf8(
            fs::weakly_canonical(aiagent::platform::FromUtf8(context.working_directory), ec));
}

std::string ConsentPath(const std::string& data_dir) {
    if (data_dir.empty()) {
        return {};
    }
    return aiagent::platform::ToUtf8(aiagent::platform::FromUtf8(data_dir) /
                                     "review_consent.json");
}

bool ResolvePackPath(const std::string& pack_arg, const tools::ToolContext& context,
                     fs::path* path, std::string* error) {
    if (pack_arg.empty()) {
        if (error != nullptr) {
            *error = "pack is required";
        }
        return false;
    }
    *path = aiagent::platform::ResolveCanonicalPath(
            aiagent::platform::FromUtf8(context.working_directory), pack_arg);
    std::error_code ec;
    if (!fs::is_regular_file(*path, ec) || ec) {
        if (error != nullptr) {
            *error = "review pack not found: " + pack_arg;
        }
        return false;
    }
    return true;
}

void LoadState(const ReviewPack& pack, const std::string& pack_rel,
               const ReviewConfig& config, ReviewState* state) {
    std::string error;
    if (!LoadReviewState(config, pack.repo_root, pack.review_id, state, &error)) {
        *state = ReviewState{};
        state->review_id = pack.review_id;
        state->pack_path = pack_rel;
    }
    if (state->pack_path.empty()) {
        state->pack_path = pack_rel;
    }
}

std::string ExtractFileDiff(const std::string& diff, const std::string& path) {
    std::istringstream lines(diff);
    std::string line;
    std::string current;
    bool matched = false;
    std::ostringstream output;
    while (std::getline(lines, line)) {
        if (line.rfind("diff --git ", 0) == 0) {
            if (matched) {
                break;
            }
            matched = false;
            current = line;
            // 头部同时含 a/ 与 b/；用 b/ 路径做匹配。
            const std::size_t b_pos = line.find(" b/");
            if (b_pos != std::string::npos) {
                const std::string candidate = line.substr(b_pos + 3);
                matched = candidate == path;
            }
        }
        if (matched) {
            output << line << '\n';
        }
    }
    return output.str();
}

std::vector<ReviewCommand> ParseCommands(const nlohmann::json& value, std::string* error) {
    std::vector<ReviewCommand> commands;
    if (!value.is_array()) {
        if (error != nullptr) {
            *error = "verification must be an array";
        }
        return {};
    }
    for (const auto& item : value) {
        if (!item.is_object()) {
            if (error != nullptr) {
                *error = "verification entries must be objects";
            }
            return {};
        }
        ReviewCommand command;
        command.id = item.value("id", std::string());
        command.cwd = item.value("cwd", std::string("."));
        command.timeout_s = item.value("timeout_s", 300L);
        command.declared_exit_code = item.value("declared_exit_code", 0);
        command.summary = item.value("summary", std::string());
        if (item.contains("argv") && item["argv"].is_array()) {
            command.argv = item["argv"].get<std::vector<std::string>>();
        }
        commands.push_back(std::move(command));
    }
    return commands;
}

std::optional<ReviewReportVerificationRef> ParseVerificationRef(const nlohmann::json& item) {
    ReviewReportVerificationRef ref;
    if (item.is_string()) {
        std::string value = item.get<std::string>();
        const std::size_t colon = value.find(':');
        if (colon != std::string::npos) {
            ref.source = value.substr(0, colon);
            ref.command_id = value.substr(colon + 1);
        } else {
            ref.command_id = std::move(value);
        }
        return ref;
    }
    if (item.is_object()) {
        ref.command_id = item.value("command_id", item.value("id", std::string()));
        ref.source = item.value("source", std::string());
        if (!ref.command_id.empty()) {
            return ref;
        }
    }
    return std::nullopt;
}

std::string OutputExcerpt(const std::string& output, std::size_t limit) {
    if (output.size() <= limit) {
        return output;
    }
    return aiagent::platform::TruncateUtf8(output, limit) + "\n[truncated]";
}

llm::ToolSpec MakeSpec(std::string name, std::string description, std::string snippet,
                       std::vector<std::string> guidelines, nlohmann::json parameters) {
    llm::ToolSpec spec;
    spec.name = std::move(name);
    spec.description = std::move(description);
    spec.prompt_snippet = std::move(snippet);
    spec.prompt_guidelines = std::move(guidelines);
    spec.parameters = std::move(parameters);
    return spec;
}

nlohmann::json ObjectSchema(nlohmann::json properties, std::vector<std::string> required = {}) {
    nlohmann::json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

nlohmann::json StringProp(const std::string& description) {
    return {{"type", "string"}, {"description", description}};
}

}  // namespace

ReviewTools::ReviewTools(ReviewToolOptions options, Mode mode,
                         HistoryProvider history_provider)
        : options_(std::move(options)),
          mode_(mode),
          history_provider_(std::move(history_provider)),
          pack_service_(ReviewPackServiceOptions{options_.config, options_.data_dir}) {}

void ReviewTools::EnsureClock(const ReviewPack& pack, const std::string& pack_relative,
                              ReviewState* state) {
    if (state->pack_path.empty()) {
        state->pack_path = pack_relative;
    }
    if (state->started_at_epoch_s <= 0) {
        state->started_at_epoch_s = static_cast<long long>(std::time(nullptr));
        state->updated_at = LocalTimestampIso();
        std::string error;
        SaveReviewState(options_.config, pack.repo_root, *state, &error);
    }
    const long budget = std::max<long>(1, options_.config.review_timeout_s);
    deadline_epoch_s_.store(state->started_at_epoch_s + budget);
}

bool ReviewTools::DeadlineExceeded() const {
    const long long deadline = deadline_epoch_s_.load();
    return deadline > 0 && static_cast<long long>(std::time(nullptr)) > deadline;
}

bool ReviewTools::LoadPack(const std::string& pack_arg,
                           const tools::ToolContext& context, ReviewPack* pack,
                           std::string* abs_path, std::string* rel_path,
                           std::string* error) {
    fs::path path;
    std::string rel;
    if (!ResolvePackPath(pack_arg, context, &path, error)) {
        return false;
    }
    std::error_code ec;
    const auto mtime = fs::last_write_time(path, ec);
    const auto size = ec ? 0 : fs::file_size(path, ec);
    const std::string abs = aiagent::platform::ToUtf8(path);
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        for (const auto& entry : pack_cache_) {
            if (entry.path == abs && !ec && entry.mtime == mtime && entry.size == size) {
                *pack = entry.pack;
                if (abs_path != nullptr) {
                    *abs_path = abs;
                }
                if (rel_path != nullptr) {
                    *rel_path = entry.relative_path;
                }
                return true;
            }
        }
    }
    if (!LoadReviewPack(abs, pack, error)) {
        return false;
    }
    const std::string repo = aiagent::platform::ToUtf8(
            fs::weakly_canonical(aiagent::platform::FromUtf8(pack->repo_root), ec));
    if (ec) {
        if (error != nullptr) {
            *error = "review pack has an invalid repo_root";
        }
        return false;
    }
    rel = PackRelative(repo, path);
    if (rel.empty() || !IsUnderReviewDir(rel, options_.config)) {
        if (error != nullptr) {
            *error = "review pack must live inside the configured review directory";
        }
        return false;
    }
    if (!ec) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        pack_cache_.push_back(PackCacheEntry{abs, mtime, size, rel, *pack});
        while (pack_cache_.size() > 4) {
            pack_cache_.pop_front();
        }
    }
    if (abs_path != nullptr) {
        *abs_path = abs;
    }
    if (rel_path != nullptr) {
        *rel_path = rel;
    }
    return true;
}

std::vector<llm::ToolSpec> ReviewTools::ListTools() {
    std::vector<llm::ToolSpec> tools;
    if (mode_ == Mode::kCodingHandoff) {
        tools.push_back(MakeSpec(
                "review_handoff",
                "Create a review pack (git diff + untracked + intent + declared verification) "
                "for a separate reviewer session. Does not modify the workspace.",
                "Prepare a review pack for a separate reviewer session",
                {"Call review_handoff only after implementing and verifying the change; pass "
                 "the feature name, base ref, context paths, notes, and declared verification "
                 "argv."},
                ObjectSchema({{"feature", StringProp("Feature name used in the report file")},
                              {"base", StringProp("Base git revision (default HEAD)")},
                              {"context_paths",
                               {{"type", "array"}, {"items", {{"type", "string"}}}}},
                              {"notes", StringProp("Implementation notes, assumptions, risks")},
                              {"verification",
                               {{"type", "array"},
                                {"items", {{"type", "object"},
                                           {"properties",
                                            {{"id", StringProp("command id")},
                                             {"argv", {{"type", "array"},
                                                       {"items", {{"type", "string"}}}}},
                                             {"cwd", StringProp("relative cwd")},
                                             {"timeout_s", {{"type", "integer"}}},
                                             {"declared_exit_code", {{"type", "integer"}}},
                                             {"summary", StringProp("declared result")}}}}}}},
                              {"previous_report", StringProp("Previous review report path")}},
                             {"feature"})));
        return tools;
    }

    tools.push_back(MakeSpec(
            "review_context",
            "Read the review pack. Modes: manifest (summary + stale), diff (single file slice), "
            "untracked (inline content), session (request/session excerpt).",
            "Read the review pack manifest/diff/untracked/session",
            {"Always start with mode=manifest and check stale=false.",
             "Read the diff per file; do not dump a huge diff at once."},
            ObjectSchema({{"pack", StringProp("Review pack path")},
                          {"mode", {{"type", "string"},
                                    {"enum", {"manifest", "diff", "untracked", "session", "delta"}}}},
                          {"path", StringProp("Changed/untracked file path for diff/untracked")},
                          {"offset", {{"type", "integer"}}},
                          {"limit", {{"type", "integer"}}}},
                         {"pack"})));

    tools.push_back(MakeSpec(
            "review_read",
            "Batch-read files by line range with per-file and total output limits.",
            "Read real code and callers (batch, line ranges)",
            {"Use review_read to read several files/callers in one call instead of many read "
             "calls."},
            ObjectSchema({{"files", {{"type", "array"},
                                     {"items", {{"type", "object"},
                                                {"properties",
                                                 {{"path", StringProp("file path")},
                                                  {"start", {{"type", "integer"}}},
                                                  {"limit", {{"type", "integer"}}}}}}}}},
                          {"max_bytes", {{"type", "integer"}}}},
                         {"files"})));

    tools.push_back(MakeSpec(
            "review_verify",
            "List or execute a declared build/test argv (no shell). Before/after source digests "
            "are compared; changing source is reported as source_modified.",
            "Run a declared build/test command",
            {"Use list=true first; execute only pack:<id> or project:<id>.",
             "Project commands require valid user consent; changed .de/review.json invalidates "
             "prior consent."},
            ObjectSchema({{"pack", StringProp("Review pack path")},
                          {"source", {{"type", "string"}, {"enum", {"pack", "project"}}}},
                          {"list", {{"type", "boolean"}}},
                          {"command_id", StringProp("Command id, optionally prefixed")},
                          {"timeout_s", {{"type", "integer"}}}},
                         {"pack"})));

    tools.push_back(MakeSpec(
            "review_test_write",
            "Create or append a classified test file. Never overwrites or deletes an existing "
            "test and never writes source/config.",
            "Write a minimal reproduction test (create/append only)",
            {"Only create/append test files; list them in the final report test_files[]."},
            ObjectSchema({{"pack", StringProp("Review pack path (optional after review_context)")},
                          {"path", StringProp("Test path relative to the repo root")},
                          {"content", StringProp("Test content")},
                          {"mode", {{"type", "string"}, {"enum", {"create", "append"}}}},
                          {"reason", StringProp("What finding this test reproduces")}},
                         {"path", "content", "mode"})));

    tools.push_back(MakeSpec(
            "review_setup",
            "Detect/show/save the project verification config `.de/review.json`. save requires "
            "explicit user approval and records consent bound to the file hash.",
            "Inspect or configure project build/test commands",
            {"Use detect then ask the user before save; show the exact argv in the request."},
            ObjectSchema({{"action", {{"type", "string"},
                                      {"enum", {"detect", "show", "save"}}}},
                          {"language", StringProp("Detected/declared language")},
                          {"commands", {{"type", "array"},
                                        {"items", {{"type", "object"},
                                                   {"properties",
                                                    {{"id", StringProp("command id")},
                                                     {"kind", {{"type", "string"},
                                                               {"enum", {"build", "test"}}}},
                                                     {"argv", {{"type", "array"},
                                                               {"items", {{"type", "string"}}}}},
                                                     {"cwd", StringProp("relative cwd")},
                                                     {"timeout_s", {{"type", "integer"}}}}}}}}},
                          {"test_globs", {{"type", "array"},
                                          {"items", {{"type", "string"}}}}}},
                         {"action"})));

    tools.push_back(MakeSpec(
            "review_report",
            "Render and atomically write the review report under the configured review directory. "
            "draft can be incomplete; final requires a verdict and consistent findings.",
            "Write the review report",
            {"Write a draft during long reviews, then a final report with verdict.",
             "Every finding needs path:line, evidence, impact, trigger, suggestion, confidence."},
            ObjectSchema({{"pack", StringProp("Review pack path")},
                          {"status", {{"type", "string"}, {"enum", {"draft", "final"}}}},
                          {"verdict", {{"type", "string"},
                                       {"enum", {"approved", "approved_with_notes",
                                                 "changes_requested", "blocked"}}}},
                          {"summary", StringProp("1-3 sentence conclusion")},
                          {"findings", {{"type", "array"},
                                        {"items", {{"type", "object"},
                                                   {"properties",
                                                    {{"id", StringProp("R1")},
                                                     {"severity", StringProp("blocker/major/minor/nit")},
                                                     {"title", StringProp("one-line finding")},
                                                     {"file", StringProp("path")},
                                                     {"line", {{"type", "integer"}}},
                                                     {"evidence", StringProp("quoted evidence")},
                                                     {"impact", StringProp("user-visible impact")},
                                                     {"trigger", StringProp("reproduction")},
                                                     {"suggestion", StringProp("fix direction")},
                                                     {"confidence", StringProp("high/medium/low")},
                                                     {"status", StringProp("open/resolved/still_open/regressed")}}}}}}},
                          {"checked", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                          {"verification", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                          {"test_files", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                          {"unverified", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                          {"open_questions", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                          {"notes", StringProp("optional notes")}},
                         {"pack", "summary"})));

    return tools;
}

std::vector<llm::ToolSpec> ReviewTools::ListToolsFor(const std::string& working_directory) {
    (void)working_directory;
    return ListTools();
}

tools::ToolExecutionResult ReviewTools::Execute(const std::string& name,
                                                const nlohmann::json& arguments,
                                                const tools::ToolContext& context) {
    if (mode_ == Mode::kCodingHandoff) {
        if (name == "review_handoff") {
            return Handoff(arguments, context);
        }
        return Fail("tool not available: " + name);
    }
    if (name == "review_context") {
        return Context(arguments, context);
    }
    if (name == "review_read") {
        return Read(arguments, context);
    }
    if (name == "review_verify") {
        return Verify(arguments, context);
    }
    if (name == "review_test_write") {
        return TestWrite(arguments, context);
    }
    if (name == "review_setup") {
        return Setup(arguments, context);
    }
    if (name == "review_report") {
        return Report(arguments, context);
    }
    return Fail("tool not available: " + name);
}

tools::ToolExecutionResult ReviewTools::Handoff(const nlohmann::json& arguments,
                                                const tools::ToolContext& context) {
    if (!options_.config.enabled) {
        return Fail("review is disabled");
    }
    if (context.session_id.empty()) {
        return Fail("review handoff requires an interactive session");
    }
    ReviewPackRequest request;
    request.cwd = context.working_directory;
    request.feature = arguments.value("feature", std::string("review"));
    request.base = arguments.value("base", std::string("HEAD"));
    request.notes = arguments.value("notes", std::string());
    request.previous_report = arguments.value("previous_report", std::string());
    request.producer.kind = "de-session";
    if (arguments.contains("context_paths") && arguments["context_paths"].is_array()) {
        request.context_paths =
                arguments["context_paths"].get<std::vector<std::string>>();
    }
    std::string error;
    if (arguments.contains("verification")) {
        request.verification = ParseCommands(arguments["verification"], &error);
        if (!error.empty()) {
            return Fail(error);
        }
    }

    ReviewSessionExcerpt session;
    session.id = context.session_id;
    if (history_provider_) {
        int count = 0;
        history_provider_(context.session_id, &session.title, &session.user_messages, &count);
        session.message_count = count;
    }
    if (!context.incoming_text.empty() &&
        (session.user_messages.empty() || session.user_messages.back() != context.incoming_text)) {
        session.user_messages.push_back(context.incoming_text);
        ++session.message_count;
    }
    if (session.user_messages.empty()) {
        return Fail("review handoff requires the current user message");
    }
    request.session = std::move(session);

    const ReviewPackResult result = pack_service_.CreatePack(request);
    if (!result.ok) {
        return Fail(result.error);
    }
    return Ok({{"review_id", result.review_id},
               {"pack_path", result.pack_path},
               {"report_path_hint", result.report_path_hint},
               {"changed_files", result.changed_files},
               {"diff_bytes", result.diff_bytes},
               {"truncated", result.truncated},
               {"intent_source", result.intent_source},
               {"context_missing", result.context_missing},
               {"next", "de review " + result.pack_path}});
}

tools::ToolExecutionResult ReviewTools::Context(const nlohmann::json& arguments,
                                                const tools::ToolContext& context) {
    const std::string pack_arg = arguments.value("pack", std::string());
    ReviewPack pack;
    std::string abs_path;
    std::string rel_path;
    std::string error;
    if (!LoadPack(pack_arg, context, &pack, &abs_path, &rel_path, &error)) {
        return Fail(error);
    }
    {
        std::lock_guard<std::mutex> lock(last_pack_mutex_);
        last_pack_path_ = abs_path;
    }
    ReviewState state;
    LoadState(pack, rel_path, options_.config, &state);
    EnsureClock(pack, rel_path, &state);
    const auto owned = ReviewerOwnedTestPaths(state, pack.repo_root);
    const ReviewStaleResult stale =
            CheckReviewStale(pack, options_.config, pack.repo_root, owned);

    const std::string mode = arguments.value("mode", std::string("manifest"));
    auto stale_json = [&stale]() {
        return nlohmann::json{{"stale", stale.stale},
                              {"stale_reasons", stale.reasons},
                              {"head_changed", stale.head_changed}};
    };

    if (mode == "manifest") {
        ReviewConsentStore consent(ConsentPath(options_.data_dir));
        ProjectVerifyConfig project_config;
        std::string project_error;
        const bool has_project = LoadProjectVerifyConfig(pack.repo_root, &project_config,
                                                         &project_error);
        const ConsentState consent_state =
                consent.Check(pack.repo_root, has_project ? project_config.sha256 : "");
        nlohmann::json manifest = stale_json();
        manifest["review_id"] = pack.review_id;
        manifest["feature"] = pack.feature;
        manifest["created_at"] = pack.created_at;
        manifest["cwd"] = pack.cwd;
        manifest["repo_root"] = pack.repo_root;
        manifest["pack"] = rel_path;
        manifest["base_ref"] = pack.git.base_ref;
        manifest["base_commit"] = pack.git.base_commit;
        manifest["head_commit"] = pack.git.head_commit;
        manifest["branch"] = pack.git.branch;
        manifest["dirty"] = pack.git.dirty;
        manifest["diff_sha256"] = pack.git.diff_sha256;
        manifest["untracked_sha256"] = pack.git.untracked_sha256;
        manifest["diff_bytes"] = pack.diff.size();
        manifest["diff_truncated"] = pack.diff_truncated;
        manifest["producer"] = {{"kind", pack.producer.kind}, {"agent", pack.producer.agent}};
        manifest["intent_source"] = pack.intent_source;
        manifest["context_missing"] = pack.context_missing;
        manifest["request_file"] = pack.producer.request_file;
        manifest["request_sha256"] = pack.producer.request_sha256;
        manifest["session"] = pack.session.has_value()
                                      ? nlohmann::json{{"id", pack.session->id},
                                                       {"title", pack.session->title},
                                                       {"message_count",
                                                        pack.session->message_count}}
                                      : nlohmann::json(nullptr);
        manifest["notes"] = pack.notes;
        manifest["context_paths"] = nlohmann::json::array();
        for (const auto& item : pack.context_paths) {
            manifest["context_paths"].push_back(
                    {{"path", item.path}, {"sha256", item.sha256}});
        }
        manifest["changed_files"] = nlohmann::json::array();
        for (const auto& file : pack.changed_files) {
            manifest["changed_files"].push_back({{"path", file.path},
                                                 {"status", file.status},
                                                 {"added", file.added},
                                                 {"removed", file.removed},
                                                 {"binary", file.binary},
                                                 {"diff_bytes", file.diff_bytes},
                                                 {"truncated", file.truncated}});
        }
        manifest["untracked"] = nlohmann::json::array();
        for (const auto& file : pack.untracked) {
            manifest["untracked"].push_back({{"path", file.path},
                                             {"truncated", file.truncated},
                                             {"redacted", file.redacted},
                                             {"reason", file.reason},
                                             {"bytes", file.content.size()}});
        }
        manifest["omitted"] = nlohmann::json::array();
        for (const auto& file : pack.omitted) {
            manifest["omitted"].push_back({{"path", file.path}, {"reason", file.reason}});
        }
        manifest["verification"] = nlohmann::json::array();
        for (const auto& command : pack.verification) {
            manifest["verification"].push_back({{"command_id",
                                                 command.source + ":" + command.id},
                                                {"source", command.source},
                                                {"argv", command.argv},
                                                {"cwd", command.cwd},
                                                {"timeout_s", command.timeout_s}});
        }
        manifest["project_verify_config_sha256"] = pack.project_verify_config_sha256;
        manifest["project_verify_config_loaded"] = has_project;
        manifest["project_verify_config_error"] = project_error;
        manifest["project_verify_consent"] = consent_state.status;
        manifest["previous_report"] = pack.previous_report.empty()
                                             ? nlohmann::json(nullptr)
                                             : nlohmann::json(pack.previous_report);
        manifest["chunk_files"] = options_.config.chunk_files;
        manifest["chunk_diff_bytes"] = options_.config.chunk_diff_bytes;
        manifest["review_state"] = {
                {"status", state.status},
                {"reviewed_files", state.reviewed_files},
                {"reviewer_tests", state.reviewer_tests.size()},
                {"verification", state.verification.size()},
                {"next_chunk", state.next_chunk},
                {"started_at_epoch_s", state.started_at_epoch_s},
                {"deadline_epoch_s",
                 state.started_at_epoch_s + options_.config.review_timeout_s},
                {"timed_out", DeadlineExceeded() || state.timed_out}};
        return Ok(manifest);
    }

    if (mode == "diff") {
        const std::string path = NormalizeRelativePath(arguments.value("path", std::string()));
        std::string content;
        if (!path.empty()) {
            content = ExtractFileDiff(pack.diff, path);
            if (content.empty()) {
                return Fail("no diff for path: " + path);
            }
            if (std::find(state.reviewed_files.begin(), state.reviewed_files.end(), path) ==
                state.reviewed_files.end()) {
                state.reviewed_files.push_back(path);
                state.updated_at = LocalTimestampIso();
                std::string state_error;
                SaveReviewState(options_.config, pack.repo_root, state, &state_error);
            }
        } else {
            if (pack.diff.size() > 65536) {
                return Fail("diff is larger than 64 KiB; specify path");
            }
            content = pack.diff;
        }
        std::size_t offset = arguments.value("offset", 0);
        std::size_t limit = arguments.value("limit", 32768);
        limit = std::min<std::size_t>(std::max<std::size_t>(limit, 1), 65536);
        if (offset > content.size()) {
            offset = content.size();
        }
        const std::string slice = content.substr(offset, limit);
        nlohmann::json payload = stale_json();
        payload["mode"] = "diff";
        payload["path"] = path;
        payload["offset"] = offset;
        payload["content"] = slice;
        payload["truncated"] = offset + slice.size() < content.size();
        payload["next_offset"] = offset + slice.size();
        payload["total_bytes"] = content.size();
        return Ok(payload);
    }

    if (mode == "untracked") {
        const std::string path = NormalizeRelativePath(arguments.value("path", std::string()));
        if (path.empty()) {
            return Fail("untracked mode requires path");
        }
        for (const auto& file : pack.untracked) {
            if (file.path == path) {
                nlohmann::json payload = stale_json();
                payload["mode"] = "untracked";
                payload["path"] = file.path;
                payload["content"] = file.content;
                payload["truncated"] = file.truncated;
                payload["redacted"] = file.redacted;
                payload["reason"] = file.reason;
                return Ok(payload);
            }
        }
        for (const auto& file : pack.omitted) {
            if (file.path == path) {
                nlohmann::json payload = stale_json();
                payload["mode"] = "untracked";
                payload["path"] = file.path;
                payload["content"] = "";
                payload["omitted"] = true;
                payload["reason"] = file.reason;
                return Ok(payload);
            }
        }
        return Fail("untracked path is not in the pack: " + path);
    }

    if (mode == "session") {
        nlohmann::json payload = stale_json();
        payload["mode"] = "session";
        payload["intent_source"] = pack.intent_source;
        payload["request_text"] = pack.request_text;
        payload["context_missing"] = pack.context_missing;
        if (pack.session.has_value()) {
            payload["session"] = {{"id", pack.session->id},
                                  {"title", pack.session->title},
                                  {"message_count", pack.session->message_count},
                                  {"user_messages", pack.session->user_messages}};
        } else {
            payload["session"] = nullptr;
        }
        return Ok(payload);
    }

    if (mode == "delta") {
        return Fail("review_context(mode=delta) is a P1 feature");
    }
    return Fail("unknown review_context mode: " + mode);
}

tools::ToolExecutionResult ReviewTools::Read(const nlohmann::json& arguments,
                                             const tools::ToolContext& context) {
    if (!context.policy.allow_read) {
        return Fail("permission denied: read is disabled");
    }
    if (DeadlineExceeded()) {
        return Fail("review_timeout_s exceeded; stop reading and write a partial blocked "
                    "report now");
    }
    const std::string repo = RepoRootFor(context);
    const fs::path repo_path = aiagent::platform::FromUtf8(repo);
    std::size_t max_bytes = arguments.value("max_bytes", 262144);
    max_bytes = std::min<std::size_t>(std::max<std::size_t>(max_bytes, 1), 1048576);
    const auto files = arguments.value("files", nlohmann::json::array());
    if (!files.is_array() || files.empty()) {
        return Fail("files must be a non-empty array");
    }
    nlohmann::json payload = nlohmann::json::array();
    std::size_t used = 0;
    bool overall_truncated = false;
    for (const auto& item : files) {
        if (used >= max_bytes) {
            overall_truncated = true;
            break;
        }
        const std::string raw_path = item.value("path", std::string());
        if (raw_path.empty()) {
            return Fail("file path must not be empty");
        }
        const fs::path path = aiagent::platform::ResolveCanonicalPath(repo_path, raw_path);
        std::error_code ec;
        if (!aiagent::platform::IsPathWithin(path, repo_path)) {
            return Fail("path escapes the review workspace: " + raw_path);
        }
        if (!fs::is_regular_file(path, ec) || ec) {
            return Fail("not a file: " + raw_path);
        }
        const auto size = fs::file_size(path, ec);
        if (ec || size > 8 * 1024 * 1024) {
            return Fail("file is too large to review: " + raw_path);
        }
        std::string prefix;
        bool prefix_truncated = false;
        std::string read_error;
        aiagent::platform::ReadFilePrefix(path, 8192, &prefix, &prefix_truncated,
                                          &read_error);
        if (aiagent::platform::LooksBinary(prefix)) {
            return Fail("binary file cannot be read as text: " + raw_path);
        }
        const int start = std::max(1, item.value("start", 1));
        const int limit = std::max(1, item.value("limit", 2000));
        std::ifstream input(path, std::ios::binary);
        std::string line;
        int line_number = 0;
        std::ostringstream content;
        int end = start - 1;
        std::size_t body_bytes = 0;
        const std::size_t file_limit =
                std::min<std::size_t>(65536, max_bytes - std::min(used, max_bytes));
        bool truncated_by_limit = false;
        bool truncated_by_bytes = false;
        while (std::getline(input, line)) {
            ++line_number;
            if (line_number < start) {
                continue;
            }
            if (line_number >= start + limit) {
                truncated_by_limit = true;
                break;
            }
            std::string rendered = line;
            rendered.push_back('\n');
            if (body_bytes + rendered.size() > file_limit) {
                truncated_by_bytes = true;
                break;
            }
            content << rendered;
            body_bytes += rendered.size();
            end = line_number;
        }
        const std::string body = content.str();
        used += body.size();
        payload.push_back({{"path", raw_path},
                           {"start", start},
                           {"end", end},
                           {"lines_read", end >= start ? end - start + 1 : 0},
                           {"truncated", truncated_by_limit || truncated_by_bytes},
                           {"content", body}});
    }
    nlohmann::json result = {{"files", payload},
                             {"max_bytes", max_bytes},
                             {"truncated", overall_truncated || used >= max_bytes}};
    return Ok(result);
}

tools::ToolExecutionResult ReviewTools::Verify(const nlohmann::json& arguments,
                                               const tools::ToolContext& context) {
    if (!options_.config.verify_enabled) {
        return Fail("review verification is disabled");
    }
    if (!context.policy.allow_command) {
        return Fail("permission denied: command execution is disabled");
    }
    ReviewPack pack;
    std::string abs_path;
    std::string rel_path;
    std::string error;
    if (!LoadPack(arguments.value("pack", std::string()), context, &pack, &abs_path,
                  &rel_path, &error)) {
        return Fail(error);
    }
    {
        std::lock_guard<std::mutex> lock(last_pack_mutex_);
        last_pack_path_ = abs_path;
    }
    ReviewState state;
    LoadState(pack, rel_path, options_.config, &state);
    EnsureClock(pack, rel_path, &state);
    const auto owned = ReviewerOwnedTestPaths(state, pack.repo_root);
    const ReviewStaleResult stale =
            CheckReviewStale(pack, options_.config, pack.repo_root, owned);

    const bool source_explicit =
            arguments.contains("source") && arguments["source"].is_string() &&
            !arguments["source"].get<std::string>().empty();
    std::string source = source_explicit
                                 ? arguments["source"].get<std::string>()
                                 : std::string("pack");
    std::string command_id = arguments.value("command_id", std::string());
    if (!command_id.empty()) {
        const std::size_t colon = command_id.find(':');
        if (colon != std::string::npos) {
            const std::string prefix = command_id.substr(0, colon);
            if (prefix != "pack" && prefix != "project") {
                return Fail("unknown command id prefix: " + prefix);
            }
            if (source_explicit && prefix != source) {
                return Fail("command source does not match the selected source");
            }
            source = prefix;
            command_id = command_id.substr(colon + 1);
        }
    }
    if (source != "pack" && source != "project") {
        return Fail("source must be pack or project");
    }

    auto project_commands = [&]() {
        ProjectVerifyConfig project_config;
        std::string project_error;
        if (!LoadProjectVerifyConfig(pack.repo_root, &project_config, &project_error)) {
            return std::vector<ReviewCommand>{};
        }
        ReviewConsentStore consent(ConsentPath(options_.data_dir));
        if (consent.Check(pack.repo_root, project_config.sha256).status != "valid") {
            return std::vector<ReviewCommand>{};
        }
        std::vector<ReviewCommand> commands;
        for (const auto& command : project_config.commands) {
            ReviewCommand converted;
            converted.id = command.id;
            converted.source = "project";
            converted.argv = command.argv;
            converted.cwd = command.cwd;
            converted.timeout_s = command.timeout_s;
            commands.push_back(std::move(converted));
        }
        return commands;
    };

    std::vector<ReviewCommand> commands;
    if (source == "project") {
        commands = project_commands();
    } else {
        for (const auto& command : pack.verification) {
            if (command.source == "pack") {
                commands.push_back(command);
            }
        }
    }

    if (arguments.value("list", false)) {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& command : commands) {
            list.push_back({{"command_id", command.source + ":" + command.id},
                            {"source", command.source},
                            {"argv", command.argv},
                            {"cwd", command.cwd},
                            {"timeout_s", command.timeout_s},
                            {"declared_exit_code", command.declared_exit_code}});
        }
        return Ok({{"source", source},
                   {"commands", list},
                   {"stale", stale.stale},
                   {"stale_reasons", stale.reasons}});
    }

    if (stale.stale) {
        return Fail("review pack is stale; regenerate it before running verification");
    }
    if (DeadlineExceeded()) {
        return Fail("review_timeout_s exceeded; write a partial blocked report instead of "
                    "running more commands");
    }

    if (command_id.empty()) {
        return Fail("command_id is required");
    }
    const ReviewCommand* selected = nullptr;
    for (const auto& command : commands) {
        if (command.id == command_id) {
            selected = &command;
            break;
        }
    }
    if (selected == nullptr) {
        return Fail("unknown verification command: " + source + ":" + command_id);
    }
    if (selected->argv.empty()) {
        return Fail("verification command has no argv");
    }
    if (source == "pack" &&
        (!IsPackCommandAllowed(selected->argv) ||
         IsForbiddenProjectCommand(selected->argv))) {
        return Fail("pack command is not in the allowlist");
    }
    if (source == "project" && IsForbiddenProjectCommand(selected->argv)) {
        return Fail("project command uses a forbidden install/publish subcommand");
    }
    const fs::path repo_path = aiagent::platform::FromUtf8(pack.repo_root);
    if (!IsSafeRelativePath(selected->cwd, true)) {
        return Fail("verification cwd must stay inside the repository");
    }
    const fs::path cwd =
            aiagent::platform::ResolveCanonicalPath(repo_path, selected->cwd);
    std::error_code ec;
    if (!fs::is_directory(cwd, ec) || ec ||
        !aiagent::platform::IsPathWithin(cwd, repo_path)) {
        return Fail("verification cwd is not a directory inside the repository");
    }

    long timeout_s = selected->timeout_s > 0 ? selected->timeout_s
                                             : options_.config.verify_timeout_s;
    if (arguments.contains("timeout_s")) {
        timeout_s = arguments.value("timeout_s", timeout_s);
    }
    timeout_s = std::min<long>(std::max<long>(timeout_s, 1),
                               options_.config.verify_timeout_s);

    const ReviewWorkspaceDigest before = ComputeWorkspaceDigest(
            pack.repo_root, pack.git.base_commit, options_.config, owned);
    if (!before.ok) {
        return Fail("cannot compute source digest before verification: " + before.error);
    }
    const ArgvRunResult run =
            RunArgvCapture(selected->argv, aiagent::platform::ToUtf8(cwd), timeout_s,
                           options_.config.verify_output_bytes, /*keep_tail=*/true,
                           context.should_cancel);
    const ReviewWorkspaceDigest after = ComputeWorkspaceDigest(
            pack.repo_root, pack.git.base_commit, options_.config, owned);
    bool source_modified = false;
    if (!after.ok) {
        source_modified = true;
    } else if (before.diff_sha256 != after.diff_sha256 ||
               before.untracked_sha256 != after.untracked_sha256) {
        source_modified = true;
    }

    ReviewVerificationRecord record;
    record.command_id = command_id;
    record.source = source;
    record.exit_code = run.exit_code;
    record.duration_ms = run.duration_ms;
    record.timed_out = run.timed_out;
    record.source_modified = source_modified;
    record.invalid = source_modified || run.cancelled;
    record.output_excerpt = OutputExcerpt(run.output, 8192);
    bool replaced = false;
    for (auto& existing : state.verification) {
        if (existing.command_id == record.command_id && existing.source == record.source) {
            existing = record;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        state.verification.push_back(record);
    }
    state.updated_at = LocalTimestampIso();
    std::string state_error;
    if (!SaveReviewState(options_.config, pack.repo_root, state, &state_error)) {
        return Fail("verification ran but state could not be saved: " + state_error);
    }

    nlohmann::json payload = {
            {"command_id", source + ":" + command_id},
            {"source", source},
            {"argv", selected->argv},
            {"exit_code", run.exit_code},
            {"duration_ms", run.duration_ms},
            {"timed_out", run.timed_out},
            {"cancelled", run.cancelled},
            {"truncated", run.truncated},
            {"source_modified", source_modified},
            {"invalid", source_modified || run.cancelled},
            {"output", run.output},
            {"stale", false},
            {"stale_reasons", nlohmann::json::array()}};
    if (!run.error.empty()) {
        payload["error"] = run.error;
    }
    return Ok(payload);
}

tools::ToolExecutionResult ReviewTools::TestWrite(const nlohmann::json& arguments,
                                                  const tools::ToolContext& context) {
    if (!context.policy.allow_write) {
        return Fail("permission denied: test writes are disabled");
    }
    std::string pack_arg = arguments.value("pack", std::string());
    if (pack_arg.empty()) {
        std::lock_guard<std::mutex> lock(last_pack_mutex_);
        pack_arg = last_pack_path_;
    }
    ReviewPack pack;
    std::string abs_path;
    std::string rel_path;
    std::string error;
    if (!LoadPack(pack_arg, context, &pack, &abs_path, &rel_path, &error)) {
        return Fail(error.empty() ? "call review_context(pack) first or pass pack" : error);
    }
    {
        std::lock_guard<std::mutex> lock(last_pack_mutex_);
        last_pack_path_ = abs_path;
    }
    const std::string mode = arguments.value("mode", std::string());
    if (mode != "create" && mode != "append") {
        return Fail("mode must be create or append");
    }
    const std::string content = arguments.value("content", std::string());
    if (content.empty()) {
        return Fail("content must not be empty");
    }
    if (content.size() > 128 * 1024) {
        return Fail("test content exceeds 128 KiB");
    }
    std::string raw_path = arguments.value("path", std::string());
    const std::string path = NormalizeRelativePath(raw_path);
    if (path.empty() || !IsSafeRelativePath(path, false)) {
        return Fail("test path must be a relative path inside the repository");
    }

    ProjectVerifyConfig project_config;
    std::string project_error;
    std::vector<std::string> test_globs;
    if (LoadProjectVerifyConfig(pack.repo_root, &project_config, &project_error)) {
        test_globs = project_config.test_globs;
    }
    if (!IsTestPath(path, test_globs)) {
        return Fail("path is not classified as a test file: " + path);
    }
    const fs::path repo_path = aiagent::platform::FromUtf8(pack.repo_root);
    const fs::path target = aiagent::platform::ResolveCanonicalPath(repo_path, path);
    if (!aiagent::platform::IsPathWithin(target, repo_path)) {
        return Fail("test path escapes the repository");
    }
    std::error_code ec;
    const bool exists = fs::exists(target, ec) && !ec;
    std::string final_content;
    if (mode == "create") {
        if (exists) {
            return Fail("mode=create requires a path that does not exist: " + path);
        }
        final_content = content;
    } else {
        if (!exists || !fs::is_regular_file(target, ec) || ec) {
            return Fail("mode=append requires an existing test file: " + path);
        }
        std::string existing;
        bool truncated = false;
        std::string read_error;
        if (!aiagent::platform::ReadFilePrefix(target, 128 * 1024, &existing, &truncated,
                                               &read_error)) {
            return Fail("cannot read existing test file: " + read_error);
        }
        if (truncated) {
            return Fail("existing test file exceeds 128 KiB");
        }
        if (existing.size() + content.size() > 128 * 1024) {
            return Fail("appended test file would exceed 128 KiB");
        }
        final_content = existing;
        final_content += content;
    }

    const fs::path parent = target.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            return Fail("cannot create test directory: " + ec.message());
        }
    }
    const fs::path temp = target.string() + ".tmp";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            return Fail("cannot create test file: " + path);
        }
        output << final_content;
        if (!output.good()) {
            return Fail("cannot write test file: " + path);
        }
    }
    fs::rename(temp, target, ec);
    if (ec) {
        fs::remove(target, ec);
        fs::rename(temp, target, ec);
    }
    if (ec) {
        return Fail("cannot replace test file: " + path);
    }
    const std::string sha = aiagent::util::Sha256Hex(final_content);

    ReviewState state;
    LoadState(pack, rel_path, options_.config, &state);
    EnsureClock(pack, rel_path, &state);
    if (DeadlineExceeded()) {
        return Fail("review_timeout_s exceeded; write a partial blocked report instead of "
                    "adding more tests");
    }
    bool replaced = false;
    for (auto& record : state.reviewer_tests) {
        if (record.path == path) {
            record = {path, sha, mode, arguments.value("reason", std::string())};
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        state.reviewer_tests.push_back(
                {path, sha, mode, arguments.value("reason", std::string())});
    }
    state.updated_at = LocalTimestampIso();
    std::string state_error;
    if (!SaveReviewState(options_.config, pack.repo_root, state, &state_error)) {
        return Fail("test written but state could not be saved: " + state_error);
    }
    return Ok({{"review_id", pack.review_id},
               {"path", path},
               {"mode", mode},
               {"sha256", sha},
               {"bytes", final_content.size()}});
}

tools::ToolExecutionResult ReviewTools::Setup(const nlohmann::json& arguments,
                                              const tools::ToolContext& context) {
    const std::string action = arguments.value("action", std::string());
    if (action != "detect" && action != "show" && action != "save") {
        return Fail("action must be detect, show, or save");
    }
    const std::string repo = RepoRootFor(context);
    if (action == "detect") {
        const VerifyDetectResult detected = DetectProjectVerifyConfig(repo);
        nlohmann::json commands = nlohmann::json::array();
        for (const auto& command : detected.commands) {
            commands.push_back({{"id", command.id},
                                {"kind", command.kind},
                                {"argv", command.argv},
                                {"cwd", command.cwd},
                                {"timeout_s", command.timeout_s}});
        }
        return Ok({{"found", detected.found},
                   {"language", detected.language},
                   {"detected_from", detected.detected_from},
                   {"commands", commands},
                   {"test_globs", detected.test_globs},
                   {"path", ProjectVerifyConfigPath(repo)}});
    }

    ProjectVerifyConfig current;
    std::string load_error;
    const bool has_config = LoadProjectVerifyConfig(repo, &current, &load_error);
    ReviewConsentStore consent(ConsentPath(options_.data_dir));
    const ConsentState consent_state =
            consent.Check(repo, has_config ? current.sha256 : "");

    if (action == "show") {
        nlohmann::json commands = nlohmann::json::array();
        for (const auto& command : current.commands) {
            commands.push_back({{"id", command.id},
                                {"kind", command.kind},
                                {"argv", command.argv},
                                {"cwd", command.cwd},
                                {"timeout_s", command.timeout_s}});
        }
        return Ok({{"path", ProjectVerifyConfigPath(repo)},
                   {"exists", has_config},
                   {"error", load_error},
                   {"language", current.language},
                   {"detected_from", current.detected_from},
                   {"commands", commands},
                   {"test_globs", current.test_globs},
                   {"config_sha256", current.sha256},
                   {"consent", consent_state.status},
                   {"approved_at", consent_state.approved_at}});
    }

    if (!context.policy.allow_write) {
        return Fail("permission denied: review_setup(save) needs write access");
    }
    ProjectVerifyConfig config;
    config.version = 1;
    config.language = arguments.value("language", std::string());
    config.approved_at = LocalTimestampIso();
    if (arguments.contains("test_globs") && arguments["test_globs"].is_array()) {
        config.test_globs = arguments["test_globs"].get<std::vector<std::string>>();
    }
    if (!arguments.contains("commands") || !arguments["commands"].is_array()) {
        return Fail("save requires commands[] with argv");
    }
    for (const auto& item : arguments["commands"]) {
        ProjectVerifyCommand command;
        command.id = item.value("id", std::string());
        command.kind = item.value("kind", std::string("test"));
        command.cwd = item.value("cwd", std::string("."));
        command.timeout_s = item.value("timeout_s", 600L);
        if (item.contains("argv") && item["argv"].is_array()) {
            command.argv = item["argv"].get<std::vector<std::string>>();
        }
        config.commands.push_back(std::move(command));
    }
    std::string write_error;
    if (!WriteProjectVerifyConfig(repo, config, &write_error)) {
        return Fail(write_error);
    }
    ProjectVerifyConfig written;
    if (!LoadProjectVerifyConfig(repo, &written, &write_error)) {
        return Fail("review.json was written but cannot be reloaded: " + write_error);
    }
    if (!consent.Approve(repo, written.sha256, &write_error)) {
        return Fail("review.json was written but consent could not be recorded: " + write_error);
    }
    return Ok({{"path", ProjectVerifyConfigPath(repo)},
               {"language", written.language},
               {"commands", written.commands.size()},
               {"test_globs", written.test_globs},
               {"config_sha256", written.sha256},
               {"consent", "valid"}});
}

tools::ToolExecutionResult ReviewTools::Report(const nlohmann::json& arguments,
                                               const tools::ToolContext& context) {
    ReviewPack pack;
    std::string abs_path;
    std::string rel_path;
    std::string error;
    if (!LoadPack(arguments.value("pack", std::string()), context, &pack, &abs_path,
                  &rel_path, &error)) {
        return Fail(error);
    }
    {
        std::lock_guard<std::mutex> lock(last_pack_mutex_);
        last_pack_path_ = abs_path;
    }
    ReviewState state;
    LoadState(pack, rel_path, options_.config, &state);
    EnsureClock(pack, rel_path, &state);
    const bool timed_out = state.timed_out || DeadlineExceeded();
    const auto owned = ReviewerOwnedTestPaths(state, pack.repo_root);
    const ReviewStaleResult stale =
            CheckReviewStale(pack, options_.config, pack.repo_root, owned);

    ReviewReportInput input;
    input.status = arguments.value("status", std::string("final"));
    input.verdict = arguments.value("verdict", std::string());
    input.summary = arguments.value("summary", std::string());
    input.notes = arguments.value("notes", std::string());
    if (arguments.contains("checked") && arguments["checked"].is_array()) {
        input.checked = arguments["checked"].get<std::vector<std::string>>();
    }
    if (arguments.contains("unverified") && arguments["unverified"].is_array()) {
        input.unverified = arguments["unverified"].get<std::vector<std::string>>();
    }
    if (arguments.contains("open_questions") && arguments["open_questions"].is_array()) {
        input.open_questions =
                arguments["open_questions"].get<std::vector<std::string>>();
    }
    if (arguments.contains("test_files") && arguments["test_files"].is_array()) {
        input.test_files = arguments["test_files"].get<std::vector<std::string>>();
    }
    if (arguments.contains("findings") && arguments["findings"].is_array()) {
        for (const auto& item : arguments["findings"]) {
            ReviewFinding finding;
            finding.id = item.value("id", std::string());
            finding.severity = item.value("severity", std::string());
            finding.title = item.value("title", std::string());
            finding.file = item.value("file", std::string());
            finding.line = item.value("line", 0);
            finding.evidence = item.value("evidence", std::string());
            finding.impact = item.value("impact", std::string());
            finding.trigger = item.value("trigger", std::string());
            finding.suggestion = item.value("suggestion", std::string());
            finding.confidence = item.value("confidence", std::string());
            finding.status = item.value("status", std::string("open"));
            input.findings.push_back(std::move(finding));
        }
    }
    if (arguments.contains("verification") && arguments["verification"].is_array()) {
        for (const auto& item : arguments["verification"]) {
            const auto ref = ParseVerificationRef(item);
            if (!ref.has_value()) {
                return Fail("invalid verification reference");
            }
            input.verification.push_back(*ref);
        }
    }

    if (timed_out) {
        state.timed_out = true;
        input.status = "final";
        input.verdict = "blocked";
        input.unverified.push_back(
                "review_timeout_s exceeded; this report is partial and covers only the "
                "evidence collected so far");
        if (input.summary.empty()) {
            input.summary = "Review timed out; partial report only.";
        }
    }

    const std::string pack_sha = aiagent::util::Sha256HexFile(abs_path, &error);
    if (pack_sha.empty()) {
        return Fail("cannot hash review pack: " + error);
    }
    ReviewReportWriter writer(options_.config);
    const ReviewReportResult written =
            writer.Write(pack.repo_root, pack, rel_path, pack_sha, stale, state, input);
    if (!written.ok) {
        return Fail(written.error);
    }
    state.status = input.status;
    state.updated_at = LocalTimestampIso();
    std::string state_error;
    SaveReviewState(options_.config, pack.repo_root, state, &state_error);

    return Ok({{"path", written.path},
               {"bytes", written.bytes},
               {"verdict", written.verdict},
               {"findings", written.findings},
               {"status", input.status},
               {"stale", stale.stale}});
}

}  // namespace aiagent::review
