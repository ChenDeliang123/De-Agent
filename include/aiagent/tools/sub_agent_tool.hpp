#pragma once

#include <memory>
#include <string>
#include <vector>

#include "aiagent/tools/tool_provider.hpp"

namespace aiagent::agent {
class SubAgentManager;
}  // namespace aiagent::agent

namespace aiagent::tools {

// `task` 工具名。子 Agent 工具集过滤（递归控制）也用它。
inline constexpr const char* kTaskToolName = "task";

// `task_result` 工具名：取后台任务（task background=true 派发的）状态与报告。
// 与 kTaskToolName 同属一个 provider，子 Agent 过滤时两个都要剔除。
inline constexpr const char* kTaskResultToolName = "task_result";

// 单次批量扇出的任务数上限：防止一次调用打爆并发与成本。
inline constexpr int kMaxTaskBatch = 8;

// `task`（前台阻塞 / background=true 后台派发）与 `task_result`（查询/收取）
// 两个工具：把工具调用派发为子 Agent 运行（见 docs/多Agent协同规划.md、
// docs/子Agent后台化与可干预规划.md §3.2）。
//
// depth = 归属深度：根 Agent 注册的 task 工具 depth=0；某个子 Agent 内嵌的
// task 工具 depth=1，以此类推。深度上限由 SubAgentManager 的 max_depth 强制执行。
class SubAgentToolProvider final : public IToolProvider {
public:
    SubAgentToolProvider(std::weak_ptr<agent::SubAgentManager> manager, int depth);

    std::vector<llm::ToolSpec> ListTools() override;
    ToolExecutionResult Execute(const std::string& name, const nlohmann::json& arguments,
                                const ToolContext& context) override;

private:
    std::weak_ptr<agent::SubAgentManager> manager_;
    int depth_ = 0;
};

// 工厂：weak_ptr 语义，避免 Manager ↔ 父工具集之间的 shared_ptr 引用环。
std::shared_ptr<IToolProvider> MakeSubAgentToolProvider(
        std::weak_ptr<agent::SubAgentManager> manager, int depth);

}  // namespace aiagent::tools
