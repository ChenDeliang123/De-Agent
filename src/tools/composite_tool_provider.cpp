#include "aiagent/tools/composite_tool_provider.hpp"

#include <set>
#include <string>
#include <utility>

namespace aiagent::tools {

void CompositeToolProvider::Add(std::shared_ptr<IToolProvider> provider) {
    if (provider) {
        providers_.push_back(std::move(provider));
    }
}

std::vector<llm::ToolSpec> CompositeToolProvider::ListTools() {
    std::vector<llm::ToolSpec> merged;
    std::set<std::string> seen;
    for (const auto& provider : providers_) {
        for (llm::ToolSpec spec : provider->ListTools()) {
            if (!spec.name.empty() && seen.insert(spec.name).second) {
                merged.push_back(std::move(spec));
            }
        }
    }
    return merged;
}

ToolExecutionResult CompositeToolProvider::Execute(const std::string& name,
                                                   const nlohmann::json& arguments,
                                                   const ToolContext& context) {
    for (const auto& provider : providers_) {
        const auto tools = provider->ListTools();
        for (const auto& tool : tools) {
            if (tool.name == name) {
                return provider->Execute(name, arguments, context);
            }
        }
    }
    ToolExecutionResult result;
    result.ok = false;
    result.error = "unknown tool: " + name;
    return result;
}

}  // namespace aiagent::tools
