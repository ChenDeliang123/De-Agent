#include "aiagent/review/review_tools.hpp"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "aiagent/log.hpp"
#include "aiagent/permissions/project_config.hpp"
#include "aiagent/platform/paths.hpp"
#include "aiagent/platform/text_file.hpp"
#include "aiagent/review/project_verify_config.hpp"
#include "aiagent/review/review_plan.hpp"
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
    result.text = payload.dump(2, ' ', false,
                               nlohmann::json::error_handler_t::replace);
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

struct FailedSliceSummary {
    std::string slice_id;
    std::string name;
    std::string status;
    std::string error;
    std::string last_receipt_at;
    std::vector<std::string> files;  // 仍未被任何 receipt 覆盖的 assigned files
};

struct ReviewCoverageSummary {
    std::vector<std::string> required;
    std::vector<std::string> omitted;
    std::vector<std::string> covered;
    std::vector<std::string> uncovered;
    std::vector<std::string> scout_covered;
    std::vector<std::string> parent_verified;
    std::vector<FailedSliceSummary> failed_slices;
};

std::string NormalizeReviewPath(const std::string& path) {
    return NormalizeRelativePath(path);
}

std::string LowerAscii(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

ReviewCoverageSummary BuildReviewCoverage(const ReviewPack& pack,
                                          const ReviewState& state) {
    const ReviewScope scope = BuildReviewScope(pack);
    const std::set<std::string> required(scope.required.begin(), scope.required.end());
    const std::set<std::string> omitted(scope.omitted.begin(), scope.omitted.end());

    std::set<std::string> parent;
    for (const auto& path : ParentVerifiedFiles(state)) {
        const std::string normalized = NormalizeReviewPath(path);
        if (!normalized.empty()) {
            parent.insert(normalized);
        }
    }
    std::set<std::string> scout;
    for (const auto& slice : state.coverage_ledger.slices) {
        for (const auto& path : slice.covered_files) {
            const std::string normalized = NormalizeReviewPath(path);
            if (!normalized.empty()) {
                scout.insert(normalized);
            }
        }
    }
    std::set<std::string> covered = parent;
    covered.insert(scout.begin(), scout.end());

    ReviewCoverageSummary coverage;
    coverage.required.assign(required.begin(), required.end());
    coverage.omitted.assign(omitted.begin(), omitted.end());
    for (const auto& path : coverage.required) {
        if (parent.count(path) > 0) {
            coverage.parent_verified.push_back(path);
        }
        if (scout.count(path) > 0) {
            coverage.scout_covered.push_back(path);
        }
        if (covered.count(path) > 0) {
            coverage.covered.push_back(path);
        } else {
            coverage.uncovered.push_back(path);
        }
    }
    std::set<std::string> seen_failed;
    for (const auto& slice : state.coverage_ledger.slices) {
        if (slice.status == "done" || slice.status == "running") {
            continue;
        }
        if (!seen_failed.insert(slice.slice_id).second) {
            continue;
        }
        FailedSliceSummary summary;
        summary.slice_id = slice.slice_id;
        summary.name = slice.name;
        summary.status = slice.status;
        summary.error = slice.error;
        summary.last_receipt_at = slice.finished_at.empty()
                                          ? state.coverage_ledger.updated_at
                                          : slice.finished_at;
        for (const auto& path : slice.assigned_files) {
            const std::string normalized = NormalizeReviewPath(path);
            if (!normalized.empty() && covered.count(normalized) == 0) {
                summary.files.push_back(normalized);
            }
        }
        coverage.failed_slices.push_back(std::move(summary));
    }
    return coverage;
}

bool MarkReviewCovered(ReviewState* state, const std::string& slice_id,
                       const std::string& method, const std::string& path) {
    return AddCoverageReceipt(state, slice_id, method, path);
}

std::string PackPathRelative(const ReviewPack& pack, const std::string& abs_path) {
    return aiagent::platform::ToGenericUtf8(
            aiagent::platform::FromUtf8(abs_path).lexically_relative(
                    aiagent::platform::FromUtf8(pack.repo_root)));
}

std::size_t Utf8CharLength(const std::string& text, std::size_t offset) {
    if (offset >= text.size()) {
        return 0;
    }
    const unsigned char lead = static_cast<unsigned char>(text[offset]);
    if (lead < 0x80U) {
        return 1;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return 4;
    }
    return 1;
}

std::size_t AlignUtf8Start(const std::string& text, std::size_t offset) {
    if (offset >= text.size()) {
        return text.size();
    }
    while (offset < text.size() &&
           (static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U) {
        ++offset;
    }
    return offset;
}

std::string ConsentPath(const std::string& data_dir) {
    if (data_dir.empty()) {
        return {};
    }
    return aiagent::platform::ToUtf8(aiagent::platform::FromUtf8(data_dir) /
                                     "review_consent.json");
}

struct WorktreeGuard {
    std::string repo;
    std::string path;
    bool active = false;

    ~WorktreeGuard() {
        if (!active || path.empty()) {
            return;
        }
        RunArgvCapture({"git", "worktree", "remove", "--force", path}, repo, 120,
                       65536);
        std::error_code ec;
        std::filesystem::remove_all(aiagent::platform::FromUtf8(path), ec);
    }
};

bool PrepareCopyWorktree(const ReviewPack& pack, const ReviewConfig& config,
                         WorktreeGuard* guard, std::string* error) {
    namespace fs = std::filesystem;
    const fs::path repo = aiagent::platform::FromUtf8(pack.repo_root);
    std::error_code ec;
    if (fs::exists(repo / ".gitmodules", ec) && !ec) {
        if (error != nullptr) {
            *error = "verify_mode=copy does not support submodules yet; use in_place";
        }
        return false;
    }
    if (!IsValidReviewId(pack.review_id)) {
        if (error != nullptr) {
            *error = "review pack has an invalid review_id";
        }
        return false;
    }
    const fs::path worktree = aiagent::platform::TempDir() /
                              aiagent::platform::FromUtf8(
                                      "de-review-" + pack.review_id + "-" +
                                      NewReviewId());
    if (fs::exists(worktree, ec) && !ec) {
        fs::remove_all(worktree, ec);
    }
    const std::string worktree_text = aiagent::platform::ToUtf8(worktree);
    const ArgvRunResult add = RunArgvCapture(
            {"git", "worktree", "add", "--detach", worktree_text, pack.git.base_commit},
            pack.repo_root, 120, 65536);
    if (!add.started || add.exit_code != 0) {
        if (error != nullptr) {
            *error = "cannot create verification worktree: " + add.output + add.error;
        }
        return false;
    }
    guard->repo = pack.repo_root;
    guard->path = worktree_text;
    guard->active = true;

    auto cleanup_fail = [&](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    const ArgvRunResult diff = RunArgvCapture(
            {"git", "diff", "--no-color", "--no-ext-diff", pack.git.base_commit},
            pack.repo_root, config.review_timeout_s, config.max_diff_bytes);
    if (!diff.started || diff.exit_code != 0) {
        return cleanup_fail("cannot read working-tree diff for copy: " + diff.output);
    }
    if (diff.truncated) {
        return cleanup_fail("working-tree diff exceeds max_diff_bytes; cannot copy");
    }
    if (!diff.output.empty()) {
        const fs::path patch = worktree / ".de-review.patch";
        {
            std::ofstream output(patch, std::ios::binary | std::ios::trunc);
            if (!output) {
                return cleanup_fail("cannot write verification patch");
            }
            output << diff.output;
        }
        const ArgvRunResult apply = RunArgvCapture(
                {"git", "apply", "--whitespace=nowarn", aiagent::platform::ToUtf8(patch)},
                worktree_text, 120, 65536);
        fs::remove(patch, ec);
        if (!apply.started || apply.exit_code != 0) {
            return cleanup_fail("cannot apply working-tree diff in copy: " + apply.output);
        }
    }

    const ArgvRunResult untracked = RunArgvCapture(
            {"git", "ls-files", "--others", "--exclude-standard"}, pack.repo_root, 60,
            4 * 1024 * 1024);
    if (!untracked.started || untracked.exit_code != 0) {
        return cleanup_fail("cannot list untracked files for copy: " + untracked.output);
    }
    std::istringstream lines(untracked.output);
    std::string relative;
    while (std::getline(lines, relative)) {
        if (relative.empty() || IsUnderReviewDir(relative, config)) {
            continue;
        }
        const fs::path source = aiagent::platform::ResolveCanonicalPath(repo, relative);
        if (!aiagent::platform::IsPathWithin(source, repo) ||
            !fs::is_regular_file(source, ec) || ec) {
            return cleanup_fail("unsupported untracked entry in copy: " + relative);
        }
        const fs::path target =
                aiagent::platform::ResolveCanonicalPath(worktree, relative);
        if (!aiagent::platform::IsPathWithin(target, worktree)) {
            return cleanup_fail("untracked path escapes the verification copy: " +
                                relative);
        }
        fs::create_directories(target.parent_path(), ec);
        fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return cleanup_fail("cannot copy untracked file: " + relative);
        }
    }
    return true;
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

std::string CleanDiffPath(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                              value.back() == '\t')) {
        value.pop_back();
    }
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
        std::string unescaped;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size()) {
                const char next = value[++i];
                if (next == 'n') {
                    unescaped.push_back('\n');
                } else if (next == 't') {
                    unescaped.push_back('\t');
                } else if (next == 'r') {
                    unescaped.push_back('\r');
                } else {
                    unescaped.push_back(next);
                }
            } else {
                unescaped.push_back(value[i]);
            }
        }
        return unescaped;
    }
    return value;
}

std::string DiffHeaderPathLocal(const std::string& line, char side) {
    const std::string prefix = side == 'b' ? "+++ " : "--- ";
    if (line.rfind(prefix, 0) != 0) {
        return {};
    }
    std::string value = CleanDiffPath(line.substr(4));
    if (value.empty() || value == "/dev/null") {
        return {};
    }
    if (value.size() >= 2 && value[0] == side && value[1] == '/') {
        value = value.substr(2);
    }
    return value;
}

std::string ExtractFileDiff(const std::string& diff, const std::string& path) {
    std::istringstream lines(diff);
    std::string line;
    std::string section;
    std::string section_path;
    std::string output;
    auto flush = [&]() {
        if (!section.empty() && section_path == path) {
            output += section;
        }
        section.clear();
        section_path.clear();
    };
    while (std::getline(lines, line)) {
        if (line.rfind("diff --git ", 0) == 0) {
            flush();
            section = line;
            section.push_back('\n');
            continue;
        }
        if (section.empty()) {
            continue;
        }
        section += line;
        section.push_back('\n');
        if (section_path.empty()) {
            section_path = DiffHeaderPathLocal(line, 'b');
            if (section_path.empty()) {
                section_path = DiffHeaderPathLocal(line, 'a');
            }
        }
    }
    flush();
    return output;
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

ReviewTools::~ReviewTools() {
    CleanupCopyWorktree();
}

bool ReviewTools::EnsureCopyWorktree(const ReviewPack& pack, std::string* path,
                                     std::string* error) {
    std::lock_guard<std::mutex> lock(copy_mutex_);
    std::error_code ec;
    if (copy_worktree_review_id_ == pack.review_id &&
        !copy_worktree_path_.empty() &&
        std::filesystem::is_directory(
                aiagent::platform::FromUtf8(copy_worktree_path_), ec) &&
        !ec) {
        if (path != nullptr) {
            *path = copy_worktree_path_;
        }
        return true;
    }
    if (!copy_worktree_path_.empty()) {
        RunArgvCapture({"git", "worktree", "remove", "--force", copy_worktree_path_},
                       copy_worktree_repo_, 120, 65536);
        std::filesystem::remove_all(aiagent::platform::FromUtf8(copy_worktree_path_), ec);
        copy_worktree_path_.clear();
        copy_worktree_repo_.clear();
        copy_worktree_review_id_.clear();
    }
    WorktreeGuard guard;
    if (!PrepareCopyWorktree(pack, options_.config, &guard, error)) {
        return false;
    }
    guard.active = false;
    copy_worktree_path_ = guard.path;
    copy_worktree_repo_ = guard.repo;
    copy_worktree_review_id_ = pack.review_id;
    if (path != nullptr) {
        *path = copy_worktree_path_;
    }
    return true;
}

void ReviewTools::CleanupCopyWorktree() {
    std::lock_guard<std::mutex> lock(copy_mutex_);
    if (copy_worktree_path_.empty()) {
        return;
    }
    RunArgvCapture({"git", "worktree", "remove", "--force", copy_worktree_path_},
                   copy_worktree_repo_, 120, 65536);
    std::error_code ec;
    std::filesystem::remove_all(aiagent::platform::FromUtf8(copy_worktree_path_), ec);
    copy_worktree_path_.clear();
    copy_worktree_repo_.clear();
    copy_worktree_review_id_.clear();
}

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

bool ReviewTools::LoadCachedLines(const std::string& path,
                                  std::vector<std::string>* lines, bool* cache_hit,
                                  std::string* error) {
    *cache_hit = false;
    const fs::path file = aiagent::platform::FromUtf8(path);
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) {
        if (error != nullptr) {
            *error = "cannot stat file: " + path;
        }
        return false;
    }
    if (size > 8 * 1024 * 1024) {
        if (error != nullptr) {
            *error = "file is too large to review: " + path;
        }
        return false;
    }
    const auto mtime = fs::last_write_time(file, ec);
    {
        std::lock_guard<std::mutex> lock(read_cache_mutex_);
        for (auto it = read_cache_.begin(); it != read_cache_.end(); ++it) {
            if (it->path == path && !ec && it->mtime == mtime && it->size == size) {
                *lines = it->lines;
                *cache_hit = true;
                ReadCacheEntry entry = std::move(*it);
                read_cache_.erase(it);
                read_cache_.push_back(std::move(entry));
                return true;
            }
        }
    }

    std::string prefix;
    bool prefix_truncated = false;
    std::string read_error;
    aiagent::platform::ReadFilePrefix(file, 8192, &prefix, &prefix_truncated, &read_error);
    if (aiagent::platform::LooksBinary(prefix)) {
        if (error != nullptr) {
            *error = "binary file cannot be read as text: " + path;
        }
        return false;
    }
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot read file: " + path;
        }
        return false;
    }
    std::vector<std::string> loaded;
    std::size_t bytes = 0;
    std::string line;
    while (std::getline(input, line)) {
        bytes += line.size() + 1;
        loaded.push_back(line);
    }
    if (input.bad()) {
        if (error != nullptr) {
            *error = "read error while caching file: " + path;
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(read_cache_mutex_);
        read_cache_.push_back(
                ReadCacheEntry{path, mtime, size, bytes, std::move(loaded)});
        read_cache_bytes_ += bytes;
        while (read_cache_bytes_ > 64 * 1024 * 1024 && !read_cache_.empty()) {
            read_cache_bytes_ -= read_cache_.front().bytes;
            read_cache_.pop_front();
        }
        *lines = read_cache_.back().lines;
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
            "Read the review pack. Modes: manifest (summary + stale + coverage), plan "
            "(coverage groups/focus), diff (single file slice), untracked (inline content), "
            "session (request/session excerpt).",
            "Read the review pack manifest/plan/diff/untracked/session",
            {"Always start with mode=manifest and check stale=false.",
             "Use mode=plan to cover every group before writing a final report.",
             "Read the diff per file; do not dump a huge diff at once."},
            ObjectSchema({{"pack", StringProp("Review pack path")},
                          {"mode", {{"type", "string"},
                                    {"enum", {"manifest", "plan", "diff", "untracked", "session",
                                              "delta"}}}},
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
            "review_git",
            "Read-only git query. Allowed subcommands: diff, log, show, blame, rev-parse, "
            "ls-files, status. No shell, no writes, no other branches/reflog.",
            "Inspect git diff/history for the current review",
            {"Use review_git instead of a generic shell; it is argv-only and read-only.",
             "Do not request checkout/reset/restore/stash/clean or --all/--reflog."},
            ObjectSchema({{"subcommand", {{"type", "string"},
                                          {"enum", {"diff", "log", "show", "blame",
                                                    "rev-parse", "ls-files", "status"}}}},
                          {"args", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                          {"max_bytes", {{"type", "integer"}}}},
                         {"subcommand"})));

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
             "Every finding needs path:line, evidence, impact, trigger, suggestion, confidence.",
             "verification[] only contains refs (pack:<id> / project:<id>) that were "
             "executed via review_verify; put human-readable verification notes in "
             "checked/unverified/notes."},
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
                          {"verification",
                           {{"type", "array"},
                            {"items",
                             {{"oneOf",
                               {{{"type", "string"}},
                                {{"type", "object"},
                                 {"properties",
                                  {{"command_id", StringProp("command id")},
                                   {"source", StringProp("pack or project")}}}}}}}},
                            {"description",
                             "Only command refs (pack:<id> / project:<id>) that were "
                             "actually executed via review_verify; do not put prose here."}}},
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
    if (name == "review_git") {
        return Git(arguments, context);
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
    if (context.review_slice_id.empty()) {
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
    const ReviewCoverageSummary coverage = BuildReviewCoverage(pack, state);
    const std::vector<ReviewPlanGroup> plan_groups =
            BuildReviewPlanGroups(coverage.required);
    if (state.coverage_ledger.required_files !=
                static_cast<int>(coverage.required.size()) ||
        state.coverage_ledger.omitted_files !=
                static_cast<int>(coverage.omitted.size())) {
        state.coverage_ledger.required_files =
                static_cast<int>(coverage.required.size());
        state.coverage_ledger.omitted_files =
                static_cast<int>(coverage.omitted.size());
        std::string scope_error;
        UpdateReviewState(
                options_.config, pack.repo_root, pack.review_id, rel_path,
                [&coverage](ReviewState* latest) {
                    latest->coverage_ledger.required_files =
                            static_cast<int>(coverage.required.size());
                    latest->coverage_ledger.omitted_files =
                            static_cast<int>(coverage.omitted.size());
                },
                &scope_error);
        if (!scope_error.empty()) {
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kWarning, "review",
                    "event=coverage_save_failed review_id=" + pack.review_id +
                            " error=" + scope_error);
        }
    }
    auto coverage_json = [&coverage, &plan_groups]() {
        nlohmann::json details = nlohmann::json::array();
        for (const auto& failed : coverage.failed_slices) {
            details.push_back({{"slice_id", failed.slice_id},
                               {"name", failed.name},
                               {"status", failed.status},
                               {"error", failed.error},
                               {"last_receipt_at", failed.last_receipt_at},
                               {"files", failed.files}});
        }
        return nlohmann::json{
                {"total_changed_files",
                 coverage.required.size() + coverage.omitted.size()},
                {"required_files", coverage.required.size()},
                {"scout_covered_files", coverage.scout_covered.size()},
                {"parent_verified_files", coverage.parent_verified.size()},
                {"covered_files", coverage.covered.size()},
                {"uncovered_files", coverage.uncovered.size()},
                {"omitted_files", coverage.omitted.size()},
                {"failed_slices", coverage.failed_slices.size()},
                {"failed_slice_details", details},
                {"groups", plan_groups.size()},
                {"uncovered", coverage.uncovered}};
    };
    auto usage_json = [this, &state, &coverage]() {
        const ReviewUsage scout = ScoutUsage(state.coverage_ledger);
        nlohmann::json usage;
        usage["scout"] = ReviewUsageToJson(scout);
        usage["scout"]["available"] = !scout.Empty();
        if (state.has_run_usage) {
            usage["run"] = ReviewUsageToJson(state.run_usage);
            usage["run"]["available"] = true;
            usage["parent"] = ReviewUsageToJson(ParentUsage(state));
        }
        if (const auto cost = EstimateReviewCostUsd(state, options_.config);
            cost.has_value()) {
            usage["estimated_cost_usd"] = *cost;
            if (!coverage.required.empty()) {
                usage["cost_per_required_file"] =
                        *cost / static_cast<double>(coverage.required.size());
            }
            if (!coverage.covered.empty()) {
                usage["cost_per_covered_file"] =
                        *cost / static_cast<double>(coverage.covered.size());
            }
        }
        return usage;
    };
    auto budget_json = [this, &state]() {
        return nlohmann::json{
                {"max_tool_rounds", options_.config.max_tool_rounds},
                {"review_timeout_s", options_.config.review_timeout_s},
                {"deadline_epoch_s",
                 state.started_at_epoch_s + options_.config.review_timeout_s},
                {"timed_out", DeadlineExceeded() || state.timed_out}};
    };
    auto record_receipts = [this, &pack, &rel_path, &context](
                                   const std::string& method,
                                   const std::vector<std::string>& paths) {
        if (paths.empty()) {
            return;
        }
        std::string save_error;
        if (!UpdateReviewState(
                    options_.config, pack.repo_root, pack.review_id, rel_path,
                    [&context, &method, &paths](ReviewState* latest) {
                        for (const auto& path : paths) {
                            MarkReviewCovered(latest, context.review_slice_id, method,
                                              path);
                        }
                    },
                    &save_error)) {
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kWarning, "review",
                    "event=coverage_save_failed review_id=" + pack.review_id +
                            " error=" + save_error);
        }
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
        manifest["coverage"] = coverage_json();
        manifest["usage"] = usage_json();
        manifest["budget"] = budget_json();
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kInfo, "review",
                "event=review_manifest review_id=" + pack.review_id +
                        " required=" + std::to_string(coverage.required.size()) +
                        " covered=" + std::to_string(coverage.covered.size()) +
                        " scout_covered=" + std::to_string(coverage.scout_covered.size()) +
                        " parent_verified=" +
                        std::to_string(coverage.parent_verified.size()) +
                        " uncovered=" + std::to_string(coverage.uncovered.size()) +
                        " failed_slices=" +
                        std::to_string(coverage.failed_slices.size()) +
                        " groups=" + std::to_string(plan_groups.size()));
        return Ok(manifest);
    }

    if (mode == "plan") {
        std::map<std::string, nlohmann::json> file_info;
        for (const auto& file : pack.changed_files) {
            file_info[NormalizeReviewPath(file.path)] = {
                    {"path", file.path},
                    {"status", file.status},
                    {"added", file.added},
                    {"removed", file.removed},
                    {"binary", file.binary},
                    {"untracked", false}};
        }
        for (const auto& file : pack.untracked) {
            file_info[NormalizeReviewPath(file.path)] = {
                    {"path", file.path},
                    {"status", "untracked"},
                    {"added", 0},
                    {"removed", 0},
                    {"binary", false},
                    {"untracked", true}};
        }
        std::set<std::string> covered_set(coverage.covered.begin(), coverage.covered.end());
        nlohmann::json groups = nlohmann::json::array();
        for (const auto& group : plan_groups) {
            nlohmann::json files = nlohmann::json::array();
            nlohmann::json uncovered = nlohmann::json::array();
            for (const auto& path : group.files) {
                const auto it = file_info.find(path);
                files.push_back(it == file_info.end() ? nlohmann::json{{"path", path}}
                                                      : it->second);
                if (covered_set.count(path) == 0) {
                    uncovered.push_back(path);
                }
            }
            groups.push_back({{"group_id", group.group_id},
                              {"files", files},
                              {"focus", group.focus},
                              {"covered", group.files.size() - uncovered.size()},
                              {"uncovered", uncovered}});
        }
        nlohmann::json plan = stale_json();
        plan["mode"] = "plan";
        plan["review_id"] = pack.review_id;
        plan["feature"] = pack.feature;
        plan["coverage"] = coverage_json();
        plan["usage"] = usage_json();
        plan["budget"] = budget_json();
        plan["groups"] = groups;
        return Ok(plan);
    }

    if (mode == "diff") {
        const std::string path = NormalizeRelativePath(arguments.value("path", std::string()));
        std::string content;
        if (!path.empty()) {
            content = ExtractFileDiff(pack.diff, path);
            if (content.empty()) {
                for (const auto& omitted : pack.omitted) {
                    if (omitted.path == path) {
                        nlohmann::json payload = stale_json();
                        payload["mode"] = "diff";
                        payload["path"] = path;
                        payload["content"] = "";
                        payload["omitted"] = true;
                        payload["reason"] = omitted.reason;
                        return Ok(payload);
                    }
                }
                return Fail("no diff for path: " + path);
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
        // 分页边界不能落在 UTF-8 续字节上，否则 JSON 序列化会拒绝整页。
        offset = AlignUtf8Start(content, offset);
        // limit 小于当前字符字节数时必须至少带出一个完整字符，否则
        // next_offset 不前进，调用方会原地重试。
        const std::size_t char_len = Utf8CharLength(content, offset);
        const std::size_t take = std::max(limit, char_len);
        std::string slice = content.substr(offset, take);
        aiagent::platform::TrimIncompleteUtf8Suffix(&slice);
        if (slice.empty() && offset < content.size() && char_len > 0) {
            slice = content.substr(offset, char_len);
        }
        nlohmann::json payload = stale_json();
        payload["mode"] = "diff";
        payload["path"] = path;
        payload["offset"] = offset;
        payload["content"] = slice;
        const bool full = !content.empty() && offset + slice.size() >= content.size();
        payload["truncated"] = !full;
        payload["next_offset"] = offset + slice.size();
        payload["total_bytes"] = content.size();
        payload["covered"] = full;
        if (full) {
            std::vector<std::string> paths;
            if (path.empty()) {
                for (const auto& file : pack.changed_files) {
                    paths.push_back(file.path);
                }
            } else {
                paths.push_back(path);
            }
            record_receipts("review_context:diff", paths);
        }
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
                const bool full =
                        !file.redacted && file.reason.empty() && !file.truncated;
                payload["covered"] = full;
                if (full) {
                    record_receipts("review_context:untracked", {file.path});
                }
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
        if (pack.previous_report.empty()) {
            return Fail("pack has no previous_report; delta is only for round 2");
        }
        PreviousReviewSummary previous;
        std::string previous_error;
        if (!LoadPreviousReviewSummary(pack.repo_root, pack.previous_report, &previous,
                                       &previous_error)) {
            return Fail("cannot read previous report: " + previous_error);
        }
        const fs::path previous_path = aiagent::platform::ResolveCanonicalPath(
                aiagent::platform::FromUtf8(pack.repo_root),
                previous.pack_path);
        const std::string previous_rel = PackRelative(pack.repo_root, previous_path);
        if (previous_rel.empty() || !IsUnderReviewDir(previous_rel, options_.config)) {
            return Fail("previous report points outside the review directory");
        }
        ReviewPack previous_pack;
        if (!LoadReviewPack(aiagent::platform::ToUtf8(previous_path), &previous_pack,
                            &previous_error)) {
            return Fail("cannot load previous pack: " + previous_error);
        }
        if (aiagent::platform::NormalizePathKey(previous_pack.repo_root) !=
            aiagent::platform::NormalizePathKey(pack.repo_root)) {
            return Fail("previous pack belongs to a different repository");
        }

        auto signatures = [](const ReviewPack& value) {
            std::map<std::string, std::string> map;
            for (const auto& file : value.changed_files) {
                map[file.path] = file.status + "|" + std::to_string(file.added) + "|" +
                                 std::to_string(file.removed) + "|" +
                                 std::to_string(file.diff_bytes) + "|" +
                                 ExtractFileDiff(value.diff, file.path);
            }
            for (const auto& file : value.untracked) {
                map[file.path] = "untracked|" + std::to_string(file.content.size()) + "|" +
                                 (file.redacted ? "redacted" : file.reason) + "|" +
                                 aiagent::util::Sha256Hex(file.content);
            }
            return map;
        };
        const auto current_sig = signatures(pack);
        const auto previous_sig = signatures(previous_pack);
        std::set<std::string> paths;
        for (const auto& [path, _] : current_sig) {
            paths.insert(path);
        }
        for (const auto& [path, _] : previous_sig) {
            paths.insert(path);
        }
        nlohmann::json files = nlohmann::json::array();
        for (const auto& path : paths) {
            const bool in_current = current_sig.count(path) > 0;
            const bool in_previous = previous_sig.count(path) > 0;
            std::string change;
            if (in_current && !in_previous) {
                change = "added";
            } else if (!in_current && in_previous) {
                change = "removed";
            } else if (current_sig.at(path) != previous_sig.at(path)) {
                change = "modified";
            } else {
                continue;
            }
            files.push_back({{"path", path}, {"change", change}});
        }
        nlohmann::json payload = stale_json();
        payload["mode"] = "delta";
        payload["previous_report"] = pack.previous_report;
        payload["previous_pack"] = previous_rel;
        payload["previous_review_id"] = previous.review_id;
        payload["previous_verdict"] = previous.verdict;
        payload["previous_finding_ids"] = previous.finding_ids;
        payload["current_pack"] = rel_path;
        payload["head_changed"] =
                previous_pack.git.head_commit != pack.git.head_commit;
        payload["files"] = files;
        return Ok(payload);
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
    const auto started = std::chrono::steady_clock::now();
    const std::string repo = RepoRootFor(context);
    const fs::path repo_path = aiagent::platform::FromUtf8(repo);
    ReviewPack coverage_pack;
    std::string coverage_pack_relative;
    bool coverage_ready = false;
    {
        std::string last_pack = context.review_pack_path;
        if (last_pack.empty()) {
            std::lock_guard<std::mutex> lock(last_pack_mutex_);
            last_pack = last_pack_path_;
        }
        if (!last_pack.empty()) {
            std::string abs_path;
            std::string rel_path;
            std::string load_error;
            if (LoadPack(last_pack, context, &coverage_pack, &abs_path, &rel_path,
                         &load_error)) {
                coverage_pack_relative = rel_path;
                coverage_ready = true;
            }
        }
    }
    std::vector<std::string> covered_paths;
    std::size_t max_bytes = arguments.value("max_bytes", 262144);
    max_bytes = std::min<std::size_t>(std::max<std::size_t>(max_bytes, 1), 1048576);
    const auto files = arguments.value("files", nlohmann::json::array());
    if (!files.is_array() || files.empty()) {
        return Fail("files must be a non-empty array");
    }
    nlohmann::json payload = nlohmann::json::array();
    std::size_t used = 0;
    std::size_t cache_hits = 0;
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
        const int start = std::max(1, item.value("start", 1));
        const int limit = std::max(1, item.value("limit", 2000));
        std::vector<std::string> lines;
        bool cache_hit = false;
        std::string read_error;
        if (!LoadCachedLines(aiagent::platform::ToUtf8(path), &lines, &cache_hit,
                             &read_error)) {
            return Fail(read_error);
        }
        if (cache_hit) {
            ++cache_hits;
        }
        std::ostringstream content;
        int end = start - 1;
        std::size_t body_bytes = 0;
        const std::size_t file_limit =
                std::min<std::size_t>(65536, max_bytes - std::min(used, max_bytes));
        bool truncated_by_limit = false;
        bool truncated_by_bytes = false;
        for (std::size_t i = static_cast<std::size_t>(start - 1); i < lines.size(); ++i) {
            const int line_number = static_cast<int>(i) + 1;
            if (line_number >= start + limit) {
                truncated_by_limit = true;
                break;
            }
            std::string rendered = lines[i];
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
        const bool full_range =
                start == 1 &&
                (lines.empty() ||
                 (end >= static_cast<int>(lines.size()) && !truncated_by_limit &&
                  !truncated_by_bytes));
        payload.push_back({{"path", raw_path},
                           {"start", start},
                           {"end", end},
                           {"lines_read", end >= start ? end - start + 1 : 0},
                           {"truncated", truncated_by_limit || truncated_by_bytes},
                           {"cache_hit", cache_hit},
                           {"covered", full_range},
                           {"content", body}});
        if (full_range && coverage_ready) {
            covered_paths.push_back(raw_path);
        }
    }
    if (coverage_ready && !covered_paths.empty()) {
        std::string save_error;
        if (!UpdateReviewState(
                    options_.config, coverage_pack.repo_root, coverage_pack.review_id,
                    coverage_pack_relative,
                    [&context, &covered_paths](ReviewState* latest) {
                        for (const auto& path : covered_paths) {
                            AddCoverageReceipt(latest, context.review_slice_id,
                                               "review_read:full", path);
                        }
                    },
                    &save_error)) {
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kWarning, "review",
                    "event=coverage_save_failed review_id=" +
                            coverage_pack.review_id + " error=" + save_error);
        }
    }
    nlohmann::json result = {{"files", payload},
                             {"max_bytes", max_bytes},
                             {"truncated", overall_truncated || used >= max_bytes}};
    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
    aiagent::DefaultLogger().Log(
            aiagent::LogLevel::kInfo, "review",
            "event=review_read files=" + std::to_string(files.size()) +
                    " cache_hits=" + std::to_string(cache_hits) +
                    " output_bytes=" + std::to_string(used) +
                    " duration_ms=" + std::to_string(duration_ms));
    return Ok(result);
}

tools::ToolExecutionResult ReviewTools::Git(const nlohmann::json& arguments,
                                            const tools::ToolContext& context) {
    if (!context.policy.allow_read) {
        return Fail("permission denied: read is disabled");
    }
    if (DeadlineExceeded()) {
        return Fail("review_timeout_s exceeded; stop reading and write a partial blocked "
                    "report now");
    }
    static const std::set<std::string> kAllowed = {
            "diff", "log", "show", "blame", "rev-parse", "ls-files", "status"};
    const std::string subcommand = arguments.value("subcommand", std::string());
    if (kAllowed.count(subcommand) == 0) {
        return Fail("review_git subcommand is not allowed: " + subcommand);
    }
    std::vector<std::string> extra;
    if (arguments.contains("args")) {
        if (!arguments["args"].is_array()) {
            return Fail("review_git args must be an array");
        }
        if (arguments["args"].size() > 32) {
            return Fail("review_git accepts at most 32 args");
        }
        for (const auto& item : arguments["args"]) {
            if (!item.is_string()) {
                return Fail("review_git args must contain strings");
            }
            const std::string arg = item.get<std::string>();
            if (arg.empty() || arg.find('\0') != std::string::npos ||
                arg.find('\n') != std::string::npos ||
                arg.find('\r') != std::string::npos) {
                return Fail("review_git rejected an invalid argument");
            }
            extra.push_back(arg);
        }
    }
    static const std::set<std::string> kForbiddenExact = {
            "--output", "--ext-diff", "--textconv", "--no-index", "--git-dir",
            "--work-tree", "--exec-path", "--config", "--config-env",
            "--pathspec-from-file", "--file", "--all", "--branches", "--remotes",
            "--tags", "--reflog", "-g", "-C", "-c"};
    static const std::set<std::string> kForbiddenPrefix = {
            "--output=", "--git-dir=", "--work-tree=", "--exec-path=",
            "--config=", "--config-env=", "--pathspec-from-file="};
    for (const auto& arg : extra) {
        if (kForbiddenExact.count(arg) > 0) {
            return Fail("review_git argument is not allowed: " + arg);
        }
        for (const auto& prefix : kForbiddenPrefix) {
            if (arg.rfind(prefix, 0) == 0) {
                return Fail("review_git argument is not allowed: " + arg);
            }
        }
    }

    std::vector<std::string> argv = {"git", "-c", "core.quotePath=false", subcommand};
    argv.insert(argv.end(), extra.begin(), extra.end());
    std::size_t max_bytes = arguments.value("max_bytes", 262144);
    max_bytes = std::min<std::size_t>(std::max<std::size_t>(max_bytes, 1), 1048576);
    const ArgvRunResult run =
            RunArgvCapture(argv, RepoRootFor(context), 30, max_bytes, false);
    if (!run.started) {
        return Fail("cannot start git: " + run.error);
    }
    if (run.timed_out) {
        return Fail("review_git timed out after 30s");
    }
    if (run.cancelled) {
        return Fail("review_git cancelled");
    }
    if (run.exit_code != 0) {
        return Fail("review_git failed (exit " + std::to_string(run.exit_code) +
                    "): " + run.output);
    }
    return Ok({{"subcommand", subcommand},
               {"args", extra},
               {"exit_code", run.exit_code},
               {"duration_ms", run.duration_ms},
               {"truncated", run.truncated},
               {"output", run.output}});
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
    const bool copy_mode = options_.config.verify_mode == "copy";
    fs::path run_root = repo_path;
    fs::path cwd;
    std::error_code ec;
    if (copy_mode) {
        std::string copy_path;
        std::string copy_error;
        if (!EnsureCopyWorktree(pack, &copy_path, &copy_error)) {
            return Fail(copy_error);
        }
        run_root = aiagent::platform::FromUtf8(copy_path);
        cwd = aiagent::platform::ResolveCanonicalPath(run_root, selected->cwd);
        if (!aiagent::platform::IsPathWithin(cwd, run_root)) {
            return Fail("verification cwd escapes the verification copy");
        }
        if (!fs::is_directory(cwd, ec) && !ec) {
            fs::create_directories(cwd, ec);
        }
    } else {
        cwd = aiagent::platform::ResolveCanonicalPath(repo_path, selected->cwd);
    }
    if (!fs::is_directory(cwd, ec) || ec ||
        !aiagent::platform::IsPathWithin(cwd, run_root)) {
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
            {"verify_mode", copy_mode ? "copy" : "in_place"},
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

    // 任何写盘动作之前先检查总时限与 stale；避免超时/过期后仍留下测试文件。
    ReviewState state;
    LoadState(pack, rel_path, options_.config, &state);
    EnsureClock(pack, rel_path, &state);
    if (DeadlineExceeded()) {
        return Fail("review_timeout_s exceeded; write a partial blocked report instead of "
                    "adding more tests");
    }
    const auto owned_before_write = ReviewerOwnedTestPaths(state, pack.repo_root);
    const ReviewStaleResult stale_before_write =
            CheckReviewStale(pack, options_.config, pack.repo_root, owned_before_write);
    if (stale_before_write.stale) {
        return Fail("review pack is stale; write a blocked report instead of adding tests");
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

std::string ReviewTools::CurrentPackPath() const {
    std::lock_guard<std::mutex> lock(last_pack_mutex_);
    return last_pack_path_;
}

void ReviewTools::RecordRunUsage(const ReviewUsage& usage) {
    const std::string pack_path = CurrentPackPath();
    if (pack_path.empty()) {
        return;
    }
    ReviewPack pack;
    std::string error;
    if (!LoadReviewPack(pack_path, &pack, &error)) {
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kWarning, "review",
                "event=review_usage_save_failed review_id=- error=" + error);
        return;
    }
    const std::string rel_path = PackPathRelative(pack, pack_path);
    if (!UpdateReviewState(
                options_.config, pack.repo_root, pack.review_id, rel_path,
                [&usage](ReviewState* latest) {
                    latest->has_run_usage = true;
                    latest->run_usage = usage;
                },
                &error)) {
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kWarning, "review",
                "event=review_usage_save_failed review_id=" + pack.review_id +
                        " error=" + error);
        return;
    }
    aiagent::DefaultLogger().Log(
            aiagent::LogLevel::kInfo, "review",
            "event=review_run_usage review_id=" + pack.review_id +
                    " prompt_tokens=" + std::to_string(usage.prompt_tokens) +
                    " completion_tokens=" + std::to_string(usage.completion_tokens) +
                    " cache_hit_tokens=" + std::to_string(usage.cache_hit_tokens) +
                    " requests=" + std::to_string(usage.requests) +
                    " duration_ms=" + std::to_string(usage.duration_ms));
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
            finding.origin = item.value("origin", std::string("parent"));
            finding.parent_verified =
                    item.value("parent_verified", finding.origin == "parent");
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

    // 分片 finding 只能以“父级核验过”的形态进入正式报告；未核验/未知 slice/重复
    // 一律移入 unverified，不能让模型自报直接污染报告。
    {
        std::vector<ReviewFinding> kept;
        std::set<std::string> seen;
        for (auto& finding : input.findings) {
            if (finding.origin.empty()) {
                finding.origin = "parent";
            }
            if (finding.origin == "parent") {
                finding.parent_verified = true;
            } else if (finding.origin.rfind("scout:", 0) == 0) {
                const std::string slice_id = finding.origin.substr(6);
                if (FindCoverageSlice(state.coverage_ledger, slice_id) == nullptr) {
                    input.unverified.push_back(
                            "finding " + finding.id + " references unknown scout slice " +
                            slice_id + "; excluded");
                    continue;
                }
                if (!finding.parent_verified) {
                    input.unverified.push_back(
                            "finding " + finding.id + " from " + finding.origin +
                            " is not parent-verified; excluded");
                    continue;
                }
            } else {
                input.unverified.push_back(
                        "finding " + finding.id + " has invalid origin " +
                        finding.origin + "; excluded");
                continue;
            }
            const std::string key =
                    NormalizeReviewPath(finding.file) + ":" +
                    std::to_string(finding.line) + "|" +
                    LowerAscii(finding.title);
            if (!seen.insert(key).second) {
                input.unverified.push_back(
                        "duplicate finding " + finding.id + " at " + finding.file +
                        ":" + std::to_string(finding.line) + "; excluded");
                continue;
            }
            kept.push_back(std::move(finding));
        }
        input.findings = std::move(kept);
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

    const ReviewCoverageSummary coverage = BuildReviewCoverage(pack, state);
    if (input.status == "final") {
        for (const auto& failed : coverage.failed_slices) {
            std::ostringstream note;
            note << "review_scout slice " << failed.slice_id << " status="
                 << failed.status;
            if (!failed.error.empty()) {
                note << " error=" << failed.error;
            }
            if (!failed.last_receipt_at.empty()) {
                note << " last_receipt_at=" << failed.last_receipt_at;
            }
            if (!failed.files.empty()) {
                note << " missing_assigned=";
                for (std::size_t i = 0; i < failed.files.size(); ++i) {
                    if (i > 0) {
                        note << ",";
                    }
                    note << failed.files[i];
                }
            }
            input.unverified.push_back(note.str());
        }
        if (input.verdict == "blocked") {
            const std::size_t shown =
                    std::min<std::size_t>(coverage.uncovered.size(), 10);
            for (std::size_t i = 0; i < shown; ++i) {
                input.unverified.push_back("uncovered: " + coverage.uncovered[i]);
            }
            input.unverified.push_back(
                    "recovery: read uncovered files with review_read/"
                    "review_context or rerun review_scout, then write a new final "
                    "report");
        }
    }
    if (input.status == "final" && input.verdict != "blocked") {
        if (!coverage.uncovered.empty()) {
            std::ostringstream message;
            message << "review coverage incomplete: " << coverage.uncovered.size()
                    << " file(s) not covered; read them with review_read or "
                       "review_context(diff/untracked), or write verdict=blocked. First: ";
            const std::size_t shown =
                    std::min<std::size_t>(coverage.uncovered.size(), 10);
            for (std::size_t i = 0; i < shown; ++i) {
                if (i > 0) {
                    message << ", ";
                }
                message << coverage.uncovered[i];
            }
            for (const auto& failed : coverage.failed_slices) {
                if (!failed.files.empty()) {
                    message << " [failed slice " << failed.slice_id
                            << " missing assigned files]";
                    break;
                }
            }
            return Fail(message.str());
        }
        if (!coverage.failed_slices.empty()) {
            std::ostringstream message;
            message << "review_scout failed or incomplete slice(s) require "
                       "verdict=blocked: ";
            const std::size_t shown =
                    std::min<std::size_t>(coverage.failed_slices.size(), 10);
            for (std::size_t i = 0; i < shown; ++i) {
                if (i > 0) {
                    message << ", ";
                }
                message << coverage.failed_slices[i].slice_id << "("
                        << coverage.failed_slices[i].status << ")";
            }
            message << "; cover missing assigned files or write a blocked report.";
            return Fail(message.str());
        }
    }
    input.coverage.required_files = static_cast<int>(coverage.required.size());
    input.coverage.covered_files = static_cast<int>(coverage.covered.size());
    input.coverage.scout_covered_files =
            static_cast<int>(coverage.scout_covered.size());
    input.coverage.parent_verified_files =
            static_cast<int>(coverage.parent_verified.size());
    input.coverage.omitted_files = static_cast<int>(coverage.omitted.size());
    input.coverage.failed_slices =
            static_cast<int>(coverage.failed_slices.size());
    for (const auto& finding : input.findings) {
        if (finding.parent_verified) {
            ++input.coverage.parent_verified_findings;
        }
    }
    const ReviewUsage total_usage = TotalUsage(state);
    input.coverage.has_usage =
            state.has_run_usage || !ScoutUsage(state.coverage_ledger).Empty();
    input.coverage.prompt_tokens = total_usage.prompt_tokens;
    input.coverage.completion_tokens = total_usage.completion_tokens;
    input.coverage.cache_hit_tokens = total_usage.cache_hit_tokens;
    if (const auto cost = EstimateReviewCostUsd(state, options_.config);
        cost.has_value()) {
        input.coverage.has_cost = true;
        input.coverage.estimated_cost_usd = *cost;
    }

    const std::string pack_sha = aiagent::util::Sha256HexFile(abs_path, &error);
    if (pack_sha.empty()) {
        return Fail("cannot hash review pack: " + error);
    }
    ReviewReportWriter writer(options_.config, options_.data_dir);
    const ReviewReportResult written =
            writer.Write(pack.repo_root, pack, rel_path, pack_sha, stale, state, input);
    if (!written.ok) {
        return Fail(written.error);
    }
    state.status = input.status;
    state.updated_at = LocalTimestampIso();
    std::string state_error;
    UpdateReviewState(
            options_.config, pack.repo_root, pack.review_id, rel_path,
            [&](ReviewState* latest) {
                latest->status = input.status;
                latest->timed_out = latest->timed_out || state.timed_out;
            },
            &state_error);
    if (input.status == "final") {
        CleanupCopyWorktree();
    }
    const ReviewUsage scout_usage = ScoutUsage(state.coverage_ledger);
    const ReviewUsage parent_usage = ParentUsage(state);
    std::ostringstream end_log;
    end_log << "event=review_end review_id=" << pack.review_id
            << " status=" << input.status << " verdict=" << input.verdict
            << " findings=" << written.findings
            << " required=" << coverage.required.size()
            << " covered=" << coverage.covered.size()
            << " scout_covered=" << coverage.scout_covered.size()
            << " parent_verified=" << coverage.parent_verified.size()
            << " uncovered=" << coverage.uncovered.size()
            << " omitted=" << coverage.omitted.size()
            << " failed_slices=" << coverage.failed_slices.size()
            << " parent_verified_findings="
            << input.coverage.parent_verified_findings
            << " prompt_tokens=" << total_usage.prompt_tokens
            << " completion_tokens=" << total_usage.completion_tokens
            << " cache_hit_tokens=" << total_usage.cache_hit_tokens
            << " parent_prompt_tokens=" << parent_usage.prompt_tokens
            << " scout_prompt_tokens=" << scout_usage.prompt_tokens;
    if (input.coverage.has_cost) {
        end_log << " estimated_cost_usd=" << input.coverage.estimated_cost_usd;
        if (!coverage.required.empty()) {
            end_log << " cost_per_required_file="
                    << (input.coverage.estimated_cost_usd /
                        static_cast<double>(coverage.required.size()));
        }
        if (!coverage.covered.empty()) {
            end_log << " cost_per_covered_file="
                    << (input.coverage.estimated_cost_usd /
                        static_cast<double>(coverage.covered.size()));
        }
    }
    aiagent::DefaultLogger().Log(aiagent::LogLevel::kInfo, "review",
                                 end_log.str());

    return Ok({{"path", written.path},
               {"bytes", written.bytes},
               {"verdict", written.verdict},
               {"findings", written.findings},
               {"status", input.status},
               {"stale", stale.stale},
               {"coverage",
                {{"required_files", coverage.required.size()},
                 {"scout_covered_files", coverage.scout_covered.size()},
                 {"parent_verified_files", coverage.parent_verified.size()},
                 {"covered_files", coverage.covered.size()},
                 {"uncovered_files", coverage.uncovered.size()},
                 {"omitted_files", coverage.omitted.size()},
                 {"failed_slices", coverage.failed_slices.size()},
                 {"parent_verified_findings",
                  input.coverage.parent_verified_findings}}},
               {"usage",
                {{"prompt_tokens", total_usage.prompt_tokens},
                 {"completion_tokens", total_usage.completion_tokens},
                 {"cache_hit_tokens", total_usage.cache_hit_tokens},
                 {"estimated_cost_usd",
                  input.coverage.has_cost
                          ? nlohmann::json(input.coverage.estimated_cost_usd)
                          : nlohmann::json(nullptr)}}}});
}

}  // namespace aiagent::review
