#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aiagent/agent/sub_agent.hpp"
#include "aiagent/config.hpp"
#include "aiagent/tools/tool_provider.hpp"

namespace aiagent::tools {

inline constexpr const char* kReviewScoutToolName = "review_scout";

// 受控只读分片：把若干文件/focus 交给固定的 review-scout 子 Agent。
// 子 Agent 的工具白名单由装配方注入；本 provider 只负责参数校验、
// assignment 构造、结果聚合，不写报告、不执行命令。
class ReviewScoutToolProvider final : public IToolProvider {
public:
    using ScoutRunner = std::function<std::vector<agent::SubAgentResult>(
            const std::vector<agent::SubAgentRequest>&, int depth)>;

    // current_pack_path 返回当前审核的 pack 绝对路径；为空时仍可跑分片，
    // 但不会写 CoverageLedger（单测/独立使用）。
    ReviewScoutToolProvider(ScoutRunner runner, int max_tasks,
                            ReviewConfig config = ReviewConfig{},
                            std::function<std::string()> current_pack_path = {});

    std::vector<llm::ToolSpec> ListTools() override;
    ToolExecutionResult Execute(const std::string& name,
                                const nlohmann::json& arguments,
                                const ToolContext& context) override;

private:
    ScoutRunner runner_;
    int max_tasks_ = 0;
    ReviewConfig config_;
    std::function<std::string()> current_pack_path_;
};

std::shared_ptr<IToolProvider> MakeReviewScoutToolProvider(
        ReviewScoutToolProvider::ScoutRunner runner, int max_tasks,
        ReviewConfig config = ReviewConfig{},
        std::function<std::string()> current_pack_path = {});

}  // namespace aiagent::tools
