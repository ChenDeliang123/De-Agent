// 测试用的跨平台小工具：进程号、环境变量、临时路径。
// 只做薄封装，避免在各测试里重复 #ifdef _WIN32。
#pragma once

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "aiagent/platform/paths.hpp"

namespace test_support {

inline long Pid() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

inline void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

inline void UnsetEnv(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

inline std::filesystem::path TempDir() {
    return aiagent::platform::TempDir();
}

// 临时目录下的绝对路径（UTF-8），用于替代测试里写死的 /tmp/...
inline std::string TempPath(const std::string& name) {
    return aiagent::platform::ToUtf8(TempDir() / aiagent::platform::FromUtf8(name));
}

// 同上，但统一用 '/' 分隔，便于直接拼进 JSON / URL 查询串。
inline std::string TempGenericPath(const std::string& name) {
    return aiagent::platform::ToGenericUtf8(
            TempDir() / aiagent::platform::FromUtf8(name));
}

// 带进程号的临时路径，避免并行 ctest 之间互相踩文件。
inline std::string TempPathWithPid(const std::string& prefix,
                                   const std::string& suffix = "") {
    return TempPath(prefix + std::to_string(Pid()) + suffix);
}

}  // namespace test_support
