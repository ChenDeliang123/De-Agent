// 版本单一来源回归测试。
//
// kVersion 来自 CMake configure_file 生成的 <build>/generated/aiagent/version.hpp，
// 其值取 project(VERSION)；这里与编译期传入的 AIAGENT_EXPECTED_VERSION 对比，
// 用来抓住「有人重新引入手写 version.hpp」或生成/包含路径错乱导致的版本漂移。
#include <string>

#include "aiagent/version.hpp"

#ifndef AIAGENT_EXPECTED_VERSION
#error "AIAGENT_EXPECTED_VERSION must be defined by the build system"
#endif

int main() {
    const std::string expected = AIAGENT_EXPECTED_VERSION;
    const std::string actual = aiagent::kVersion;
    if (actual != expected) {
        return 1;
    }
    if (actual.empty()) {
        return 2;
    }
    return 0;
}
