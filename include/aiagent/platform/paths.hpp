#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace aiagent::platform {

// 内部统一使用 UTF-8 std::string；这些助手负责与 std::filesystem::path 互转。
// Windows 上禁止直接用 path.string() 处理用户路径（会走 ACP，可能丢字符）。
std::filesystem::path FromUtf8(std::string_view text);
std::string ToUtf8(const std::filesystem::path& path);
// 模型可见路径统一用 '/' 分隔的 UTF-8。
std::string ToGenericUtf8(const std::filesystem::path& path);
// 点号前缀或 Windows FILE_ATTRIBUTE_HIDDEN。
bool IsHiddenPath(const std::filesystem::path& path);
// 权限/信任表的路径键：Windows 上统一分隔符并折叠 ASCII 大小写。
std::string NormalizePathKey(std::string path);

// 当前进程可执行文件路径；拿不到时返回空。
std::string ExecutablePath();

// 用于丢弃子进程 stderr 的设备名。
std::string NullDevice();

// PATH/PYTHONPATH 等列表分隔符：POSIX ":"，Windows ";"。
std::string PathListSeparator();

std::optional<std::string> GetEnvUtf8(std::string_view name);
bool SetEnvUtf8(std::string_view name, std::string_view value);

std::filesystem::path TempDir();

// 查找可执行文件：
// - name 含路径分隔符：验证该文件（Windows 会尝试补 .exe）；
// - 否则按 PATH（Windows 还按 PATHEXT）搜索。
// 找不到时返回空字符串并写 error。
std::string ResolveExecutable(std::string_view name, std::string* error);

// 把子进程的控制台输出转成 UTF-8。
// Windows：先按严格 UTF-8 尝试；失败则按 OEM 代码页转换。POSIX 原样返回。
std::string DecodeConsoleOutput(std::string_view bytes);

}  // namespace aiagent::platform
