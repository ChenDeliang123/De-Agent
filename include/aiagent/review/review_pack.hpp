#pragma once

#include <json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "aiagent/config.hpp"

namespace aiagent::review {

struct ReviewProducer {
    std::string kind = "manual";  // de-session / external / manual
    std::string agent;
    std::string request_file;
    std::string request_sha256;
};

struct ReviewSessionExcerpt {
    std::string id;
    std::string title;
    std::vector<std::string> user_messages;
    int message_count = 0;
};

struct ReviewChangedFile {
    std::string path;
    std::string status;
    int added = 0;
    int removed = 0;
    bool binary = false;
    std::size_t diff_bytes = 0;
    bool truncated = false;
};

struct ReviewUntrackedFile {
    std::string path;
    std::string content;
    bool truncated = false;
    bool redacted = false;
    std::string reason;  // redacted / binary / too_large / untracked_limit
};

struct ReviewOmittedFile {
    std::string path;
    std::string reason;
};

struct ReviewContextPath {
    std::string path;
    std::string sha256;
};

struct ReviewCommand {
    std::string id;
    std::string source = "pack";
    std::vector<std::string> argv;
    std::string cwd = ".";
    long timeout_s = 300;
    int declared_exit_code = 0;
    std::string summary;
};

struct ReviewGitInfo {
    std::string base_ref = "HEAD";
    std::string base_commit;
    std::string head_commit;
    std::string branch;
    bool dirty = false;
    std::string diff_sha256;
    std::string untracked_sha256;
};

struct ReviewPack {
    int schema = 1;
    std::string review_id;
    std::string feature;
    std::string created_at;
    std::string cwd;
    std::string repo_root;
    ReviewGitInfo git;
    ReviewProducer producer;
    std::string intent_source = "none";  // request-file / de-session / git-log / none
    std::string request_text;
    std::optional<ReviewSessionExcerpt> session;
    std::string notes;
    std::vector<ReviewContextPath> context_paths;
    std::string project_verify_config_sha256;
    std::vector<ReviewCommand> verification;
    std::vector<ReviewChangedFile> changed_files;
    std::vector<ReviewUntrackedFile> untracked;
    std::vector<ReviewOmittedFile> omitted;
    std::string diff;
    bool diff_truncated = false;
    std::string previous_report;
    bool context_missing = false;
};

struct ReviewPackRequest {
    std::string cwd;
    std::string feature = "review";
    std::string base = "HEAD";
    ReviewProducer producer;
    std::string request_text;
    std::string request_file;
    std::vector<std::string> context_paths;
    std::string notes;
    std::vector<ReviewCommand> verification;
    std::optional<ReviewSessionExcerpt> session;
    std::string previous_report;
};

struct ReviewPackResult {
    bool ok = false;
    std::string error;
    std::string review_id;
    std::string pack_path;         // 相对 repo_root，便于 CLI 展示
    std::string report_path_hint;  // 相对 repo_root
    std::string intent_source;
    bool context_missing = false;
    std::size_t changed_files = 0;
    std::size_t diff_bytes = 0;
    bool truncated = false;
};

struct ReviewPackServiceOptions {
    ReviewConfig config;
    std::string data_dir;
};

// 文件名安全化：保留 UTF-8 字母/数字/中文、-、_、.；空白折叠为 -；
// 拒绝路径分隔符、Windows 保留名、. / ..，最长 80 字节。
// 失败时返回空并写 error。
std::string NormalizeFeature(const std::string& feature, std::string* error = nullptr);

std::string LocalTimestampCompact();
std::string LocalTimestampIso();
std::string NewReviewId();

nlohmann::json ReviewPackToJson(const ReviewPack& pack);
bool LoadReviewPack(const std::string& path, ReviewPack* pack, std::string* error);

// 当前工作区相对 base_commit 的摘要。diff 使用与 pack 相同的采集上限；
// untracked 使用排序后的 path + 内容 sha256 清单。
struct ReviewWorkspaceDigest {
    bool ok = false;
    std::string error;
    std::string diff_sha256;
    std::string untracked_sha256;
    std::string head_commit;
};

ReviewWorkspaceDigest ComputeWorkspaceDigest(
        const std::string& repo_root, const std::string& base_commit,
        const ReviewConfig& config, const std::vector<std::string>& excluded_paths);

struct ReviewStaleResult {
    bool stale = false;
    bool head_changed = false;
    std::vector<std::string> reasons;
    ReviewWorkspaceDigest digest;
};

ReviewStaleResult CheckReviewStale(const ReviewPack& pack, const ReviewConfig& config,
                                   const std::string& repo_root,
                                   const std::vector<std::string>& reviewer_owned_paths);

// pack 构建入口；CLI、HTTP 与 review_handoff 共用。
class ReviewPackService {
public:
    explicit ReviewPackService(ReviewPackServiceOptions options);

    ReviewPackResult CreatePack(const ReviewPackRequest& request) const;

    const ReviewConfig& config() const { return options_.config; }

private:
    ReviewPackServiceOptions options_;
};

std::string ReviewPackDir(const ReviewConfig& config, const std::string& repo_root);
std::string ReviewStateDir(const ReviewConfig& config, const std::string& repo_root);
std::string ReviewInboxDir(const ReviewConfig& config, const std::string& repo_root);

}  // namespace aiagent::review
