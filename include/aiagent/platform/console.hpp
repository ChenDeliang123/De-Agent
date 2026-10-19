#pragma once

namespace aiagent::platform {

// 安装 Ctrl+C / Ctrl+Break / 关闭事件处理器。
// POSIX 等价于 std::signal(SIGINT/SIGTERM, handler)。
void InstallStopHandler(void (*handler)(int));

// 尝试开启 Windows 控制台的 ANSI/VT 处理；非 Windows 或失败返回 false
// 时调用方应禁用颜色。
bool EnableVirtualTerminal();

// 把控制台输入/输出代码页切到 UTF-8。非 Windows 为空操作。
void ConfigureConsoleUtf8();

}  // namespace aiagent::platform
