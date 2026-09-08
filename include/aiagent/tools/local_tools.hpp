#pragma once

#include <string>

#include "aiagent/tools/tool_provider.hpp"

namespace aiagent::tools {

// 后端内置 coding 工具：read / write / edit / bash。
// 直接操作本地文件系统、执行本地命令（以后端进程权限运行），
// 是终端 coding 客户端的安全边界。相对路径以 working_directory 为基准。
class LocalCodingTools : public IToolProvider {
public:
    explicit LocalCodingTools(std::string working_directory);

    std::vector<llm::ToolSpec> ListTools() override;
    ToolExecutionResult Execute(const std::string& name,
                                const nlohmann::json& arguments) override;

private:
    std::string ResolvePath(const std::string& path) const;

    std::string cwd_;
};

}  // namespace aiagent::tools
