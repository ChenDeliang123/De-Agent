#pragma once

#include <json.hpp>

#include <functional>
#include <map>
#include <optional>
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

// 一次模型运行/分片的用量。cache_hit_tokens 是 prompt_tokens 的子集。
struct ReviewUsage {
    long long prompt_tokens = 0;
    long long completion_tokens = 0;
    long long cache_hit_tokens = 0;
    long long tool_calls = 0;
    long long requests = 0;
    long long duration_ms = 0;

    void Add(const ReviewUsage& other);
    bool Empty() const;
};

// 一个 review_scout 委派主审分片；覆盖事实由工具级 receipt 写入。
struct ReviewCoverageSlice {
    std::string slice_id;
    std::string name;
    // running / done / partial / failed / timed_out / truncated / invalid / cancelled
    std::string status = "running";
    std::vector<std::string> assigned_files;
    std::vector<std::string> covered_files;
    // path -> review_read:full / review_context:diff / review_context:untracked
    std::map<std::string, std::string> methods;
    std::string started_at;
    std::string finished_at;
    std::string error;
    bool truncated = false;
    ReviewUsage usage;
};

// 覆盖账本：覆盖事实由工具 receipt 写入，模型自报 coverage 只做展示。
// 旧 state 没有该字段时，由 reviewed_files 懒迁移为 parent_verified_files。
struct ReviewCoverageLedger {
    int version = 1;
    int required_files = 0;
    int omitted_files = 0;
    std::vector<std::string> parent_verified_files;
    std::vector<ReviewCoverageSlice> slices;
    std::vector<std::string> failed_slices;  // slice_id
    std::string updated_at;
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
    // 审核起点（epoch 秒）：跨进程 resume 时继续沿用同一个总时限。
    long long started_at_epoch_s = 0;
    bool timed_out = false;
    ReviewCoverageLedger coverage_ledger;
    // 父级 run summary（含本次运行内完成的 scout）。parent = run - scout。
    bool has_run_usage = false;
    ReviewUsage run_usage;
};

std::string ReviewStatePath(const ReviewConfig& config, const std::string& repo_root,
                            const std::string& review_id);

bool LoadReviewState(const ReviewConfig& config, const std::string& repo_root,
                     const std::string& review_id, ReviewState* state,
                     std::string* error = nullptr);

bool SaveReviewState(const ReviewConfig& config, const std::string& repo_root,
                     const ReviewState& state, std::string* error = nullptr);

// 读取最新 state + 在进程内串行化 merge + 原子保存。
// 用于父级与多个 scout 并发写覆盖/用量；旧 state 不存在时创建。
bool UpdateReviewState(const ReviewConfig& config, const std::string& repo_root,
                       const std::string& review_id, const std::string& pack_path,
                       const std::function<void(ReviewState*)>& mutate,
                       std::string* error = nullptr);

// 记录一条工具级覆盖回执。slice_id 为空表示父级；否则写入对应分片。
// 返回 true 表示 state 有变化。父级回执同时同步旧字段 reviewed_files。
bool AddCoverageReceipt(ReviewState* state, const std::string& slice_id,
                        const std::string& method, const std::string& path);

// 旧 state 的 reviewed_files 与 ledger.parent_verified_files 合并后的父级覆盖。
std::vector<std::string> ParentVerifiedFiles(const ReviewState& state);
ReviewCoverageSlice* FindCoverageSlice(ReviewCoverageLedger* ledger,
                                       const std::string& slice_id);
const ReviewCoverageSlice* FindCoverageSlice(const ReviewCoverageLedger& ledger,
                                             const std::string& slice_id);
void RefreshFailedSlices(ReviewCoverageLedger* ledger);
ReviewUsage ScoutUsage(const ReviewCoverageLedger& ledger);
ReviewUsage ParentUsage(const ReviewState& state);
ReviewUsage TotalUsage(const ReviewState& state);
nlohmann::json ReviewUsageToJson(const ReviewUsage& usage);

// 价格全为 0 时返回 nullopt（不伪造 0 成本）。
std::optional<double> EstimateReviewCostUsd(const ReviewState& state,
                                            const ReviewConfig& config);

// 返回当前内容 sha256 与状态记录一致、且仍被分类为测试的路径（相对 repo_root）。
// 这些路径从 stale/验证的源码摘要中排除；内容被外部改动后自动恢复计入。
std::vector<std::string> ReviewerOwnedTestPaths(const ReviewState& state,
                                                const std::string& repo_root);

const ReviewVerificationRecord* FindVerification(const ReviewState& state,
                                                 const std::string& command_id,
                                                 const std::string& source);

}  // namespace aiagent::review
