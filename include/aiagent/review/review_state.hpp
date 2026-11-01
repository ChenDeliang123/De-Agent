#pragma once

#include <string>
#include <vector>

#include "aiagent/config.hpp"

namespace aiagent::review {

struct ReviewerTestRecord {
    std::string path;  // 相对 repo_root
    std::string sha256;
    std::string mode;  // create / append
    std::string reason;
};

struct ReviewVerificationRecord {
    std::string command_id;  // V1 / pack:test / project:test
    std::string source;      // pack / project
    int exit_code = -1;
    long duration_ms = 0;
    bool timed_out = false;
    bool source_modified = false;
    bool invalid = false;
    std::string output_excerpt;
};

// `<review dir>/.state/<review_id>.json`：长任务 checkpoint，也是 reviewer-owned
// 测试文件与验证结果的唯一事实来源。
struct ReviewState {
    std::string review_id;
    std::string pack_path;  // 相对 repo_root
    std::string pack_sha256;
    std::string status = "in_progress";  // in_progress / draft / final / cancelled
    std::vector<std::string> reviewed_files;
    std::vector<ReviewerTestRecord> reviewer_tests;
    std::vector<ReviewVerificationRecord> verification;
    int next_chunk = 0;
    std::string updated_at;
};

std::string ReviewStatePath(const ReviewConfig& config, const std::string& repo_root,
                            const std::string& review_id);

bool LoadReviewState(const ReviewConfig& config, const std::string& repo_root,
                     const std::string& review_id, ReviewState* state,
                     std::string* error = nullptr);

bool SaveReviewState(const ReviewConfig& config, const std::string& repo_root,
                     const ReviewState& state, std::string* error = nullptr);

// 返回当前内容 sha256 与状态记录一致、且仍被分类为测试的路径（相对 repo_root）。
// 这些路径从 stale/验证的源码摘要中排除；内容被外部改动后自动恢复计入。
std::vector<std::string> ReviewerOwnedTestPaths(const ReviewState& state,
                                                const std::string& repo_root);

const ReviewVerificationRecord* FindVerification(const ReviewState& state,
                                                 const std::string& command_id,
                                                 const std::string& source);

}  // namespace aiagent::review
