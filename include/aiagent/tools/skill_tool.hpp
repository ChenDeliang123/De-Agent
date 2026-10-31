#pragma once

#include <memory>
#include <string>
#include <vector>

#include "aiagent/skills/skill_loader.hpp"
#include "aiagent/tools/tool_provider.hpp"

namespace aiagent::tools {

// 只读工具名。与内置工具 `read` 同档：不弹审批，也不能执行命令/写文件。
inline constexpr const char* kSkillToolName = "skill";

// 把 SkillLoader 暴露成模型可调用的 `skill` 工具：
//   - name 必填：从当前目录的 Skills 目录中选择技能；
//   - file 可选：读取技能目录内的资源文件。
class SkillToolProvider final : public IToolProvider {
public:
    SkillToolProvider(std::shared_ptr<const skills::SkillLoader> loader,
                      std::string fallback_working_directory = {});

    std::vector<llm::ToolSpec> ListTools() override;
    std::vector<llm::ToolSpec> ListToolsFor(const std::string& working_directory) override;
    ToolExecutionResult Execute(const std::string& name, const nlohmann::json& arguments,
                                const ToolContext& context) override;

private:
    std::shared_ptr<const skills::SkillLoader> loader_;
    std::string fallback_working_directory_;
};

std::shared_ptr<IToolProvider> MakeSkillToolProvider(
        std::shared_ptr<const skills::SkillLoader> loader,
        std::string fallback_working_directory = {});

}  // namespace aiagent::tools
