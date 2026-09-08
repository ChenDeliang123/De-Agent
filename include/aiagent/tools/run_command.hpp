#pragma once

#include <string>

namespace aiagent::tools {

struct CommandResult {
    bool ok = false;      // 成功执行且退出码为 0
    int exit_code = 0;
    std::string output;   // stdout + stderr 合并（尾部截断后）
    std::string error;    // 失败描述（超时 / 无法启动等）
};

// 在指定工作目录下用 /bin/sh -c 执行命令，合并 stdout/stderr，尾部截断。
// timeout_seconds <= 0 表示不设超时。
CommandResult RunCommand(const std::string& command, const std::string& cwd,
                         long timeout_seconds);

}  // namespace aiagent::tools
