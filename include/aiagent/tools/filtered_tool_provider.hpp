#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "aiagent/tools/tool_provider.hpp"

namespace aiagent::tools {

// 按名字过滤另一个 IToolProvider 的工具集，用于子 Agent 的工具白名单。
//
// - allow 非空：只暴露 allow 里的工具（交集：父工具集 ∩ allow）。
// - allow 为空：继承父工具集的全部工具。
// - deny 永远优先生效（当前用于剔除 task，避免无约束的递归派生）。
//
// ListTools 过滤清单；Execute 对被过滤掉的工具直接拒绝（防御模型绕过清单）。
class FilteredToolProvider final : public IToolProvider {
public:
    FilteredToolProvider(std::shared_ptr<IToolProvider> inner, std::vector<std::string> allow = {},
                         std::vector<std::string> deny = {});

    std::vector<llm::ToolSpec> ListTools() override;
    ToolExecutionResult Execute(const std::string& name, const nlohmann::json& arguments,
                                const ToolContext& context) override;

    // 某个工具名是否对本 provider 可见。
    bool Allows(const std::string& name) const;

    // 暴露给上层的清单（测试/诊断用）。
    const std::set<std::string>& AllowedNames() const { return allow_; }

private:
    std::shared_ptr<IToolProvider> inner_;
    std::set<std::string> allow_;
    std::set<std::string> deny_;
    bool inherit_all_ = true;
};

}  // namespace aiagent::tools
