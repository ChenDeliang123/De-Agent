#pragma once

#include <json.hpp>

#include <string>
#include <vector>

#include "aiagent/llm/deepseek_types.hpp"

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
    virtual ToolExecutionResult Execute(const std::string& name,
                                        const nlohmann::json& arguments) = 0;
};

}  // namespace aiagent::tools
