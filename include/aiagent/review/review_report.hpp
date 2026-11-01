#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "aiagent/config.hpp"
#include "aiagent/review/review_pack.hpp"
#include "aiagent/review/review_state.hpp"

namespace aiagent::review {

struct ReviewFinding {
    std::string id;
    std::string severity;  // blocker / major / minor / nit
    std::string title;
    std::string file;
    int line = 0;
    std::string evidence;
    std::string impact;
    std::string trigger;
    std::string suggestion;
    std::string confidence;  // high / medium / low
    std::string status = "open";  // open / resolved / still_open / regressed
};

struct ReviewReportVerificationRef {
    std::string command_id;
    std::string source;  // pack / project
};

struct ReviewReportInput {
    std::string status = "final";  // draft / final
    std::string verdict;           // draft 可为空
    std::string summary;
    std::vector<ReviewFinding> findings;
    std::vector<std::string> checked;
    std::vector<ReviewReportVerificationRef> verification;
    std::vector<std::string> test_files;
    std::vector<std::string> unverified;
    std::vector<std::string> open_questions;
    std::string notes;
};

// 上一轮报告的最小摘要：frontmatter 的 pack/review_id/verdict + finding id。
// mode=delta 与第二轮状态校验共用，避免各处重复解析 Markdown。
struct PreviousReviewSummary {
    std::string report_path;
    std::string pack_path;
    std::string review_id;
    std::string verdict;
    std::vector<std::string> finding_ids;
};

bool LoadPreviousReviewSummary(const std::string& repo_root,
                               const std::string& report_relative_path,
                               PreviousReviewSummary* summary,
                               std::string* error = nullptr);

struct ReviewReportResult {
    bool ok = false;
    std::string error;
    std::string path;  // 相对 repo_root
    std::size_t bytes = 0;
    std::string verdict;
    std::size_t findings = 0;
};

// 渲染并原子写 `docs/review/<feature>-<YYYYMMDD-HHMM>.md`。
// 路径完全由 pack 文件名推导，调用方不能指定。
class ReviewReportWriter {
public:
    explicit ReviewReportWriter(ReviewConfig config, std::string data_dir = {});

    ReviewReportResult Write(const std::string& repo_root, const ReviewPack& pack,
                             const std::string& pack_relative_path,
                             const std::string& pack_sha256,
                             const ReviewStaleResult& stale, const ReviewState& state,
                             const ReviewReportInput& input) const;

private:
    ReviewConfig config_;
    std::string data_dir_;
};

std::string ReviewVerdictName(const std::string& verdict);

}  // namespace aiagent::review
