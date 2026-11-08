#pragma once

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

}  // namespace aiagent::review
