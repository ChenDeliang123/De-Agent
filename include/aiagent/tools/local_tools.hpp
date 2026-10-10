#pragma once

#include <string>
#include <vector>

#include "aiagent/tools/tool_provider.hpp"

namespace aiagent::tools {

// 文件工具（read/write/edit）的访问沙箱。
//
// enabled=true 时，路径解析并 canonical 化后必须落在「本次请求的工作目录」
// 或 roots 中的某个根目录之内（含相等）；否则直接拒绝，避免 `../`、
// 绝对路径或指向外部的符号链接越权访问。
// roots 中的相对路径以本次请求的工作目录为基准解析。
//
// 说明：bash 工具以工作目录为 cwd 执行，但本身不受该策略限制。
struct SandboxPolicy {
    bool enabled = true;
    std::vector<std::string> roots;
};

// 后端内置 coding 工具：read / write / edit / bash。
// 直接操作本地文件系统、执行本地命令（以后端进程权限运行），
// 是终端 coding 客户端的安全边界。相对路径以 working_directory 为基准。
class LocalCodingTools : public IToolProvider {
public:
    explicit LocalCodingTools(std::string working_directory,
                              SandboxPolicy sandbox = {});

    std::vector<llm::ToolSpec> ListTools() override;
    ToolExecutionResult Execute(const std::string& name,
                                const nlohmann::json& arguments,
                                const std::string& working_directory = {}) override;

    const SandboxPolicy& sandbox() const noexcept { return sandbox_; }

private:
    // 解析 path：相对路径以 working_directory（空则 cwd_）为基准。
    // 命中沙箱时写入 resolved 并返回 true；为空、非法或越界时返回 false
    // 并写入 error。
    bool ResolvePath(const std::string& path,
                     const std::string& working_directory,
                     std::string* resolved,
                     std::string* error) const;

    std::string cwd_;
    SandboxPolicy sandbox_;
};

}  // namespace aiagent::tools
