#pragma once

#include <json.hpp>
#include <string>
#include <vector>

#include "aiagent/llm/deepseek_types.hpp"
#include "aiagent/tools/tool_context.hpp"

namespace aiagent::tools {

struct ToolExecutionResult {
    bool ok = false;
    std::string text;
    std::string error;
};

// 工具提供者抽象：Agent Runtime 通过它发现工具并执行 model 发起的 tool_calls。
class IToolProvider {
public:
    virtual ~IToolProvider() = default;

    virtual std::vector<llm::ToolSpec> ListTools() = 0;

    // 按本次请求的工作目录列出工具。默认忽略 cwd，保持旧行为；
    // 技能等依赖 cwd 的工具覆盖它，实现“无技能时不暴露工具”。
    virtual std::vector<llm::ToolSpec> ListToolsFor(const std::string& working_directory) {
        (void)working_directory;
        return ListTools();
    }

    virtual ToolExecutionResult Execute(const std::string& name, const nlohmann::json& arguments,
                                        const ToolContext& context) = 0;
};

}  // namespace aiagent::tools
