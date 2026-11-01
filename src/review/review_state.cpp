#include "aiagent/review/review_state.hpp"

#include <json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "aiagent/platform/paths.hpp"
#include "aiagent/review/review_pack.hpp"
#include "aiagent/util/sha256.hpp"

namespace aiagent::review {
namespace {

namespace fs = std::filesystem;

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

nlohmann::json ToJson(const ReviewState& state) {
    nlohmann::json json;
    json["review_id"] = state.review_id;
    json["pack_path"] = state.pack_path;
    json["pack_sha256"] = state.pack_sha256;
    json["status"] = state.status;
    json["reviewed_files"] = state.reviewed_files;
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
    return json;
}

}  // namespace

std::string ReviewStatePath(const ReviewConfig& config, const std::string& repo_root,
                            const std::string& review_id) {
    return aiagent::platform::ToUtf8(
            aiagent::platform::FromUtf8(ReviewStateDir(config, repo_root)) /
            aiagent::platform::FromUtf8(review_id + ".json"));
}

bool LoadReviewState(const ReviewConfig& config, const std::string& repo_root,
                     const std::string& review_id, ReviewState* state,
                     std::string* error) {
    if (state == nullptr) {
        if (error != nullptr) {
            *error = "state output is null";
        }
        return false;
    }
    const std::string path = ReviewStatePath(config, repo_root, review_id);
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
        *state = std::move(parsed);
        return true;
    } catch (const nlohmann::json::exception& e) {
        if (error != nullptr) {
            *error = std::string("invalid review state: ") + e.what();
        }
        return false;
    }
}

bool SaveReviewState(const ReviewConfig& config, const std::string& repo_root,
                     const ReviewState& state, std::string* error) {
    const std::string path = ReviewStatePath(config, repo_root, state.review_id);
    return WriteAtomic(aiagent::platform::FromUtf8(path), ToJson(state).dump(2), error);
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
