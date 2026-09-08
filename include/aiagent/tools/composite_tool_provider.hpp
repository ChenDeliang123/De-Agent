#pragma once

#include <memory>
#include <vector>

#include "aiagent/tools/tool_provider.hpp"

namespace aiagent::tools {

// 聚合多个工具提供者：ListTools 合并（重名时以先加入者为准），
// Execute 按工具名路由到第一个能处理的 provider。
class CompositeToolProvider : public IToolProvider {
public:
    void Add(std::shared_ptr<IToolProvider> provider);

    std::vector<llm::ToolSpec> ListTools() override;
    ToolExecutionResult Execute(const std::string& name,
                                const nlohmann::json& arguments,
                                const std::string& working_directory = {}) override;

private:
    std::vector<std::shared_ptr<IToolProvider>> providers_;
};

}  // namespace aiagent::tools
