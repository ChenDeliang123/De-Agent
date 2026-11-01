#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <filesystem>

namespace aiagent::util {

// 小写十六进制 SHA-256。审核 pack/report 的 diff、untracked、config、
// reviewer-owned 测试文件都依赖同一个实现，避免各模块各写一套。
std::string Sha256Hex(const void* data, std::size_t size);
std::string Sha256Hex(std::string_view data);
std::string Sha256HexFile(const std::filesystem::path& path, std::string* error = nullptr);

}  // namespace aiagent::util
