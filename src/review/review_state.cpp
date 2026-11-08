#include "aiagent/review/review_state.hpp"

#include <json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <system_error>

#include "aiagent/platform/paths.hpp"
#include "aiagent/review/project_verify_config.hpp"
#include "aiagent/review/review_pack.hpp"
#include "aiagent/util/sha256.hpp"

namespace aiagent::review {
namespace {

namespace fs = std::filesystem;

std::mutex& StateMutex() {
    static std::mutex mutex;
    return mutex;
}

bool WriteAtomic(const fs::path& target, const std::string& content, std::string* error) {
    std::error_code ec;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
    }
    const fs::path temp = target.string() + ".tmp";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error != nullptr) {
                *error = "cannot create " + aiagent::platform::ToUtf8(target);
            }
            return false;
        }
        output << content;
        if (!output.good()) {
            if (error != nullptr) {
                *error = "cannot write " + aiagent::platform::ToUtf8(target);
            }
            return false;
        }
    }
    fs::rename(temp, target, ec);
    if (ec) {
        fs::remove(target, ec);
        fs::rename(temp, target, ec);
    }
    if (ec) {
        if (error != nullptr) {
            *error = "cannot replace " + aiagent::platform::ToUtf8(target);
        }
        return false;
    }
    return true;
}

nlohmann::json UsageJson(const ReviewUsage& usage) {
    return {{"prompt_tokens", usage.prompt_tokens},
            {"completion_tokens", usage.completion_tokens},
            {"cache_hit_tokens", usage.cache_hit_tokens},
            {"tool_calls", usage.tool_calls},
            {"requests", usage.requests},
            {"duration_ms", usage.duration_ms}};
}

nlohmann::json SliceToJson(const ReviewCoverageSlice& slice) {
    nlohmann::json methods = nlohmann::json::object();
    for (const auto& [path, method] : slice.methods) {
        methods[path] = method;
    }
    return {{"slice_id", slice.slice_id},
            {"name", slice.name},
            {"status", slice.status},
            {"assigned_files", slice.assigned_files},
            {"covered_files", slice.covered_files},
            {"methods", std::move(methods)},
            {"started_at", slice.started_at},
            {"finished_at", slice.finished_at},
            {"error", slice.error},
            {"truncated", slice.truncated},
            {"usage", UsageJson(slice.usage)}};
}

nlohmann::json ToJson(const ReviewState& state, const ReviewConfig& config) {
    nlohmann::json json;
    json["review_id"] = state.review_id;
    json["pack_path"] = state.pack_path;
    json["pack_sha256"] = state.pack_sha256;
    json["status"] = state.status;
    // reviewed_files 兼容字段：始终与 parent ledger 合并后的结果保持一致。
    const std::vector<std::string> parent_files = ParentVerifiedFiles(state);
    json["reviewed_files"] = parent_files;
    json["reviewer_tests"] = nlohmann::json::array();
    for (const auto& test : state.reviewer_tests) {
        json["reviewer_tests"].push_back({{"path", test.path},
                                          {"sha256", test.sha256},
                                          {"mode", test.mode},
                                          {"reason", test.reason}});
    }
    json["verification"] = nlohmann::json::array();
    for (const auto& record : state.verification) {
        json["verification"].push_back(
                {{"command_id", record.command_id},
                 {"source", record.source},
                 {"exit_code", record.exit_code},
                 {"duration_ms", record.duration_ms},
                 {"timed_out", record.timed_out},
                 {"source_modified", record.source_modified},
                 {"invalid", record.invalid},
                 {"output_excerpt", record.output_excerpt}});
    }
    json["next_chunk"] = state.next_chunk;
    json["updated_at"] = state.updated_at;
    json["started_at_epoch_s"] = state.started_at_epoch_s;
    json["timed_out"] = state.timed_out;

    nlohmann::json ledger;
    ledger["version"] = state.coverage_ledger.version;
    ledger["required_files"] = state.coverage_ledger.required_files;
    ledger["omitted_files"] = state.coverage_ledger.omitted_files;
    ledger["parent_verified_files"] = parent_files;
    ledger["slices"] = nlohmann::json::array();
    for (const auto& slice : state.coverage_ledger.slices) {
        ledger["slices"].push_back(SliceToJson(slice));
    }
    ledger["failed_slices"] = state.coverage_ledger.failed_slices;
    ledger["updated_at"] = state.coverage_ledger.updated_at.empty()
                                   ? state.updated_at
                                   : state.coverage_ledger.updated_at;
    json["coverage_ledger"] = std::move(ledger);

    const ReviewUsage scout = ScoutUsage(state.coverage_ledger);
    nlohmann::json usage;
    usage["scout"] = UsageJson(scout);
    if (state.has_run_usage) {
        usage["run"] = UsageJson(state.run_usage);
        usage["run"]["available"] = true;
        usage["parent"] = UsageJson(ParentUsage(state));
    }
    if (const auto cost = EstimateReviewCostUsd(state, config); cost.has_value()) {
        usage["estimated_cost_usd"] = *cost;
    }
    json["usage"] = std::move(usage);
    return json;
}

bool LoadReviewStateLocked(const ReviewConfig& config, const std::string& repo_root,
                           const std::string& review_id, ReviewState* state,
                           std::string* error) {
    if (state == nullptr) {
        if (error != nullptr) {
            *error = "state output is null";
        }
        return false;
    }
    const std::string path = ReviewStatePath(config, repo_root, review_id);
    if (path.empty()) {
        if (error != nullptr) {
            *error = "invalid review_id for review state";
        }
        return false;
    }
    std::ifstream input(aiagent::platform::FromUtf8(path));
    if (!input) {
        if (error != nullptr) {
            error->clear();
        }
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    if (text.size() > 16 * 1024 * 1024) {
        if (error != nullptr) {
            *error = "review state is too large";
        }
        return false;
    }
    try {
        const auto json = nlohmann::json::parse(text);
        ReviewState parsed;
        parsed.review_id = json.value("review_id", review_id);
        parsed.pack_path = json.value("pack_path", std::string());
        parsed.pack_sha256 = json.value("pack_sha256", std::string());
        parsed.status = json.value("status", std::string("in_progress"));
        parsed.next_chunk = json.value("next_chunk", 0);
        parsed.updated_at = json.value("updated_at", std::string());
        parsed.started_at_epoch_s = json.value("started_at_epoch_s", 0LL);
        parsed.timed_out = json.value("timed_out", false);
        if (json.contains("reviewed_files") && json["reviewed_files"].is_array()) {
            parsed.reviewed_files =
                    json["reviewed_files"].get<std::vector<std::string>>();
        }
        if (json.contains("reviewer_tests") && json["reviewer_tests"].is_array()) {
            for (const auto& item : json["reviewer_tests"]) {
                parsed.reviewer_tests.push_back(
                        {item.value("path", std::string()),
                         item.value("sha256", std::string()),
                         item.value("mode", std::string()),
                         item.value("reason", std::string())});
            }
        }
        if (json.contains("verification") && json["verification"].is_array()) {
            for (const auto& item : json["verification"]) {
                ReviewVerificationRecord record;
                record.command_id = item.value("command_id", std::string());
                record.source = item.value("source", std::string());
                record.exit_code = item.value("exit_code", -1);
                record.duration_ms = item.value("duration_ms", 0L);
                record.timed_out = item.value("timed_out", false);
                record.source_modified = item.value("source_modified", false);
                record.invalid = item.value("invalid", false);
                record.output_excerpt = item.value("output_excerpt", std::string());
                parsed.verification.push_back(std::move(record));
            }
        }
        auto parse_usage = [](const nlohmann::json& item, ReviewUsage* usage) {
            if (!item.is_object()) {
                return;
            }
            usage->prompt_tokens = item.value("prompt_tokens", 0LL);
            usage->completion_tokens = item.value("completion_tokens", 0LL);
            usage->cache_hit_tokens = item.value("cache_hit_tokens", 0LL);
            usage->tool_calls = item.value("tool_calls", 0LL);
            usage->requests = item.value("requests", 0LL);
            usage->duration_ms = item.value("duration_ms", 0LL);
        };
        if (json.contains("coverage_ledger") && json["coverage_ledger"].is_object()) {
            const auto& ledger = json["coverage_ledger"];
            parsed.coverage_ledger.version = ledger.value("version", 1);
            parsed.coverage_ledger.required_files = ledger.value("required_files", 0);
            parsed.coverage_ledger.omitted_files = ledger.value("omitted_files", 0);
            if (ledger.contains("parent_verified_files") &&
                ledger["parent_verified_files"].is_array()) {
                parsed.coverage_ledger.parent_verified_files =
                        ledger["parent_verified_files"].get<std::vector<std::string>>();
            }
            if (ledger.contains("slices") && ledger["slices"].is_array()) {
                for (const auto& item : ledger["slices"]) {
                    ReviewCoverageSlice slice;
                    slice.slice_id = item.value("slice_id", std::string());
                    slice.name = item.value("name", std::string());
                    slice.status = item.value("status", std::string("running"));
                    slice.started_at = item.value("started_at", std::string());
                    slice.finished_at = item.value("finished_at", std::string());
                    slice.error = item.value("error", std::string());
                    slice.truncated = item.value("truncated", false);
                    if (item.contains("assigned_files") && item["assigned_files"].is_array()) {
                        slice.assigned_files =
                                item["assigned_files"].get<std::vector<std::string>>();
                    }
                    if (item.contains("covered_files") && item["covered_files"].is_array()) {
                        slice.covered_files =
                                item["covered_files"].get<std::vector<std::string>>();
                    }
                    if (item.contains("methods") && item["methods"].is_object()) {
                        for (auto it = item["methods"].begin();
                             it != item["methods"].end(); ++it) {
                            if (it.value().is_string()) {
                                slice.methods[it.key()] = it.value().get<std::string>();
                            }
                        }
                    }
                    if (item.contains("usage")) {
                        parse_usage(item["usage"], &slice.usage);
                    }
                    parsed.coverage_ledger.slices.push_back(std::move(slice));
                }
            }
            if (ledger.contains("failed_slices") && ledger["failed_slices"].is_array()) {
                parsed.coverage_ledger.failed_slices =
                        ledger["failed_slices"].get<std::vector<std::string>>();
            }
            parsed.coverage_ledger.updated_at =
                    ledger.value("updated_at", std::string());
            RefreshFailedSlices(&parsed.coverage_ledger);
        } else if (!parsed.reviewed_files.empty()) {
            // 旧 state 懒迁移：reviewed_files 即父级完整读覆盖。
            parsed.coverage_ledger.version = 1;
            parsed.coverage_ledger.parent_verified_files = parsed.reviewed_files;
            parsed.coverage_ledger.updated_at = parsed.updated_at;
        }
        if (json.contains("usage") && json["usage"].is_object()) {
            const auto& usage = json["usage"];
            if (usage.contains("run") && usage["run"].is_object()) {
                parse_usage(usage["run"], &parsed.run_usage);
                parsed.has_run_usage = true;
            }
        }
        *state = std::move(parsed);
        return true;
    } catch (const nlohmann::json::exception& e) {
        if (error != nullptr) {
            *error = std::string("invalid review state: ") + e.what();
        }
        return false;
    }
}

bool SaveReviewStateLocked(const ReviewConfig& config, const std::string& repo_root,
                           const ReviewState& state, std::string* error) {
    const std::string path = ReviewStatePath(config, repo_root, state.review_id);
    if (path.empty()) {
        if (error != nullptr) {
            *error = "invalid review_id for review state";
        }
        return false;
    }
    return WriteAtomic(aiagent::platform::FromUtf8(path), ToJson(state, config).dump(2),
                       error);
}

}  // namespace

void ReviewUsage::Add(const ReviewUsage& other) {
    prompt_tokens += other.prompt_tokens;
    completion_tokens += other.completion_tokens;
    cache_hit_tokens += other.cache_hit_tokens;
    tool_calls += other.tool_calls;
    requests += other.requests;
    duration_ms += other.duration_ms;
}

bool ReviewUsage::Empty() const {
    return prompt_tokens == 0 && completion_tokens == 0 && cache_hit_tokens == 0 &&
           tool_calls == 0 && requests == 0 && duration_ms == 0;
}

std::string ReviewStatePath(const ReviewConfig& config, const std::string& repo_root,
                            const std::string& review_id) {
    if (!IsValidReviewId(review_id)) {
        return {};
    }
    return aiagent::platform::ToUtf8(
            aiagent::platform::FromUtf8(ReviewStateDir(config, repo_root)) /
            aiagent::platform::FromUtf8(review_id + ".json"));
}

bool LoadReviewState(const ReviewConfig& config, const std::string& repo_root,
                     const std::string& review_id, ReviewState* state,
                     std::string* error) {
    std::lock_guard<std::mutex> lock(StateMutex());
    return LoadReviewStateLocked(config, repo_root, review_id, state, error);
}

bool SaveReviewState(const ReviewConfig& config, const std::string& repo_root,
                     const ReviewState& state, std::string* error) {
    std::lock_guard<std::mutex> lock(StateMutex());
    return SaveReviewStateLocked(config, repo_root, state, error);
}

bool UpdateReviewState(const ReviewConfig& config, const std::string& repo_root,
                       const std::string& review_id, const std::string& pack_path,
                       const std::function<void(ReviewState*)>& mutate,
                       std::string* error) {
    if (mutate == nullptr) {
        if (error != nullptr) {
            *error = "review state mutation is null";
        }
        return false;
    }
    std::lock_guard<std::mutex> lock(StateMutex());
    ReviewState state;
    std::string load_error;
    if (!LoadReviewStateLocked(config, repo_root, review_id, &state, &load_error)) {
        if (!load_error.empty()) {
            if (error != nullptr) {
                *error = load_error;
            }
            return false;
        }
        state.review_id = review_id;
    }
    if (state.pack_path.empty()) {
        state.pack_path = pack_path;
    }
    mutate(&state);
    state.review_id = review_id;
    state.updated_at = LocalTimestampIso();
    state.coverage_ledger.updated_at = state.updated_at;
    RefreshFailedSlices(&state.coverage_ledger);
    return SaveReviewStateLocked(config, repo_root, state, error);
}

bool AddCoverageReceipt(ReviewState* state, const std::string& slice_id,
                        const std::string& method, const std::string& path) {
    if (state == nullptr) {
        return false;
    }
    const std::string normalized = NormalizeRelativePath(path);
    if (normalized.empty()) {
        return false;
    }
    bool changed = false;
    if (slice_id.empty()) {
        auto& verified = state->coverage_ledger.parent_verified_files;
        if (std::find(verified.begin(), verified.end(), normalized) == verified.end()) {
            verified.push_back(normalized);
            changed = true;
        }
        // 兼容字段与 ledger 同步。
        if (std::find(state->reviewed_files.begin(), state->reviewed_files.end(),
                      normalized) == state->reviewed_files.end()) {
            state->reviewed_files.push_back(normalized);
            changed = true;
        }
        return changed;
    }
    auto* slice = FindCoverageSlice(&state->coverage_ledger, slice_id);
    if (slice == nullptr) {
        ReviewCoverageSlice created;
        created.slice_id = slice_id;
        created.name = slice_id;
        created.status = "running";
        state->coverage_ledger.slices.push_back(std::move(created));
        slice = &state->coverage_ledger.slices.back();
        changed = true;
    }
    if (std::find(slice->covered_files.begin(), slice->covered_files.end(), normalized) ==
        slice->covered_files.end()) {
        slice->covered_files.push_back(normalized);
        changed = true;
    }
    if (!method.empty()) {
        auto [it, inserted] = slice->methods.emplace(normalized, method);
        if (!inserted && it->second != method) {
            it->second = method;
            changed = true;
        } else if (inserted) {
            changed = true;
        }
    }
    return changed;
}

std::vector<std::string> ParentVerifiedFiles(const ReviewState& state) {
    std::vector<std::string> files;
    std::set<std::string> seen;
    auto add = [&files, &seen](const std::string& path) {
        const std::string normalized = NormalizeRelativePath(path);
        if (!normalized.empty() && seen.insert(normalized).second) {
            files.push_back(normalized);
        }
    };
    for (const auto& path : state.coverage_ledger.parent_verified_files) {
        add(path);
    }
    for (const auto& path : state.reviewed_files) {
        add(path);
    }
    return files;
}

ReviewCoverageSlice* FindCoverageSlice(ReviewCoverageLedger* ledger,
                                       const std::string& slice_id) {
    if (ledger == nullptr) {
        return nullptr;
    }
    for (auto& slice : ledger->slices) {
        if (slice.slice_id == slice_id) {
            return &slice;
        }
    }
    return nullptr;
}

const ReviewCoverageSlice* FindCoverageSlice(const ReviewCoverageLedger& ledger,
                                             const std::string& slice_id) {
    for (const auto& slice : ledger.slices) {
        if (slice.slice_id == slice_id) {
            return &slice;
        }
    }
    return nullptr;
}

void RefreshFailedSlices(ReviewCoverageLedger* ledger) {
    if (ledger == nullptr) {
        return;
    }
    ledger->failed_slices.clear();
    for (const auto& slice : ledger->slices) {
        if (slice.status == "done" || slice.status == "running") {
            continue;
        }
        ledger->failed_slices.push_back(slice.slice_id);
    }
}

ReviewUsage ScoutUsage(const ReviewCoverageLedger& ledger) {
    ReviewUsage usage;
    for (const auto& slice : ledger.slices) {
        usage.Add(slice.usage);
    }
    return usage;
}

ReviewUsage ParentUsage(const ReviewState& state) {
    if (!state.has_run_usage) {
        return {};
    }
    const ReviewUsage scout = ScoutUsage(state.coverage_ledger);
    ReviewUsage parent = state.run_usage;
    parent.prompt_tokens = std::max<long long>(0, parent.prompt_tokens - scout.prompt_tokens);
    parent.completion_tokens =
            std::max<long long>(0, parent.completion_tokens - scout.completion_tokens);
    parent.cache_hit_tokens =
            std::max<long long>(0, parent.cache_hit_tokens - scout.cache_hit_tokens);
    parent.tool_calls = std::max<long long>(0, parent.tool_calls - scout.tool_calls);
    parent.requests = std::max<long long>(0, parent.requests - scout.requests);
    // duration_ms 是父级整段墙钟（含等待 scout），不做减法。
    return parent;
}

ReviewUsage TotalUsage(const ReviewState& state) {
    return state.has_run_usage ? state.run_usage : ScoutUsage(state.coverage_ledger);
}

nlohmann::json ReviewUsageToJson(const ReviewUsage& usage) {
    return UsageJson(usage);
}

std::optional<double> EstimateReviewCostUsd(const ReviewState& state,
                                            const ReviewConfig& config) {
    if (config.price_input_per_million_usd <= 0 &&
        config.price_cached_input_per_million_usd <= 0 &&
        config.price_output_per_million_usd <= 0) {
        return std::nullopt;
    }
    const ReviewUsage usage = TotalUsage(state);
    const long long cached = std::min(usage.cache_hit_tokens, usage.prompt_tokens);
    const long long uncached = std::max<long long>(0, usage.prompt_tokens - cached);
    const double cost =
            (static_cast<double>(uncached) / 1000000.0) *
                    config.price_input_per_million_usd +
            (static_cast<double>(cached) / 1000000.0) *
                    config.price_cached_input_per_million_usd +
            (static_cast<double>(usage.completion_tokens) / 1000000.0) *
                    config.price_output_per_million_usd;
    return cost;
}

std::vector<std::string> ReviewerOwnedTestPaths(const ReviewState& state,
                                                const std::string& repo_root) {
    std::vector<std::string> paths;
    for (const auto& test : state.reviewer_tests) {
        if (test.path.empty() || test.sha256.empty()) {
            continue;
        }
        std::string error;
        const std::string current =
                aiagent::util::Sha256HexFile(
                        aiagent::platform::FromUtf8(repo_root) /
                                aiagent::platform::FromUtf8(test.path),
                        &error);
        if (!current.empty() && current == test.sha256) {
            paths.push_back(test.path);
        }
    }
    return paths;
}

const ReviewVerificationRecord* FindVerification(const ReviewState& state,
                                                 const std::string& command_id,
                                                 const std::string& source) {
    for (const auto& record : state.verification) {
        if (record.command_id == command_id &&
            (source.empty() || record.source == source)) {
            return &record;
        }
    }
    return nullptr;
}

}  // namespace aiagent::review
