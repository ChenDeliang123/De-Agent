#pragma once

#include <string>
#include <vector>

#include "aiagent/tools/tool_provider.hpp"

namespace aiagent::tools {

// 后端内置 coding 工具：read / write / edit / bash。
// 直接操作本地文件系统、执行本地命令（以后端进程权限运行），
// 是终端 coding 客户端的安全边界。
//
// 相对路径以 ToolContext.working_directory 为基准；是否允许越出工作目录由
// 该请求的权限策略（policy.allow_outside）决定，extra_roots 是额外放行的根目录
// （来自 [tools] sandbox_roots）。
class LocalCodingTools : public IToolProvider {
public:
    explicit LocalCodingTools(std::string working_directory,
                              std::vector<std::string> extra_roots = {});

    std::vector<llm::ToolSpec> ListTools() override;
    ToolExecutionResult Execute(const std::string& name,
                                const nlohmann::json& arguments,
                                const ToolContext& context) override;

private:
    // 解析 path：相对路径以 working_directory（空则 cwd_）为基准。
    // policy.allow_outside 为真时不限制范围；否则必须落在工作目录或
    // extra_roots 之内。越界/非法时返回 false 并写入 error。
    bool ResolvePath(const std::string& path,
                     const std::string& working_directory,
                     const permissions::PermissionPolicy& policy,
                     std::string* resolved,
                     std::string* error) const;

    std::string cwd_;
    std::vector<std::string> extra_roots_;
};

}  // namespace aiagent::tools
