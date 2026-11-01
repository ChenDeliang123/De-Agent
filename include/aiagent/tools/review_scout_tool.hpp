#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aiagent/agent/sub_agent.hpp"
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

    ReviewScoutToolProvider(ScoutRunner runner, int max_tasks);

    std::vector<llm::ToolSpec> ListTools() override;
    ToolExecutionResult Execute(const std::string& name,
                                const nlohmann::json& arguments,
                                const ToolContext& context) override;

private:
    ScoutRunner runner_;
    int max_tasks_ = 0;
};

std::shared_ptr<IToolProvider> MakeReviewScoutToolProvider(
        ReviewScoutToolProvider::ScoutRunner runner, int max_tasks);

}  // namespace aiagent::tools
