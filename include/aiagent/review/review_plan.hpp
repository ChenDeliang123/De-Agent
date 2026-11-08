#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "aiagent/review/review_pack.hpp"

namespace aiagent::review {

// pack 的必审/省略口径。required 与 BuildReviewCoverage 共用，避免工具间漂移。
struct ReviewScope {
    std::vector<std::string> required;
    std::vector<std::string> omitted;
};

struct ReviewPlanGroup {
    std::string group_id;
    std::vector<std::string> files;
    std::vector<std::string> focus;
};

ReviewScope BuildReviewScope(const ReviewPack& pack);

// 按目录/locality 分组，单组最多 20 个文件；组间互不重叠。
std::vector<ReviewPlanGroup> BuildReviewPlanGroups(
        const std::vector<std::string>& required);

// P2 自适应分片数：profile fast=0；<20 文件=0；20–40=2；40–80=4；
// >80 时 deep 取 cap，balanced 仍取 4。结果同时受 cap 与 group_count 限制。
std::size_t RecommendedSliceCount(std::size_t required_files,
                                  std::size_t group_count,
                                  const std::string& profile, int slice_total_max);

}  // namespace aiagent::review
