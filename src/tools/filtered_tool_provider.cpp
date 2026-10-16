#include "aiagent/tools/filtered_tool_provider.hpp"

#include <utility>

namespace aiagent::tools {

FilteredToolProvider::FilteredToolProvider(std::shared_ptr<IToolProvider> inner,
                                           std::vector<std::string> allow,
                                           std::vector<std::string> deny)
    : inner_(std::move(inner)),
      allow_(allow.begin(), allow.end()),
      deny_(deny.begin(), deny.end()),
      inherit_all_(allow.empty()) {}

bool FilteredToolProvider::Allows(const std::string& name) const {
    if (name.empty() || deny_.count(name) > 0) {
        return false;
    }
    return inherit_all_ || allow_.count(name) > 0;
}

std::vector<llm::ToolSpec> FilteredToolProvider::ListTools() {
    if (!inner_) {
        return {};
    }
    std::vector<llm::ToolSpec> tools;
    for (auto& spec : inner_->ListTools()) {
        if (Allows(spec.name)) {
            tools.push_back(std::move(spec));
        }
    }
    return tools;
}

ToolExecutionResult FilteredToolProvider::Execute(const std::string& name,
                                                  const nlohmann::json& arguments,
                                                  const ToolContext& context) {
    if (!Allows(name)) {
        ToolExecutionResult result;
        result.error = "tool not available to this agent: " + name;
        return result;
    }
    if (!inner_) {
        ToolExecutionResult result;
        result.error = "no tool provider configured";
        return result;
    }
    return inner_->Execute(name, arguments, context);
}

}  // namespace aiagent::tools
