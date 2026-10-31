#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace aiagent::platform {

// 项目指令与技能加载共用的小工具：二进制嗅探、UTF-8 安全截断、文件读取。
// 放在 header-only 里是为了避免为一个几十行的 helper 增加独立编译单元。
inline constexpr std::size_t kBinaryProbeBytes = 8192;

// 前 probe 字节里出现 NUL 即视为二进制（与 AGENTS.md 的判定保持一致）。
inline bool LooksBinary(const std::string& content) {
    const std::size_t probe = std::min(content.size(), kBinaryProbeBytes);
    return content.substr(0, probe).find('\0') != std::string::npos;
}

// 按字节上限截断，且不劈开 UTF-8 序列。
inline std::string TruncateUtf8(const std::string& content, std::size_t limit) {
    if (limit >= content.size()) {
        return content;
    }
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(content[end]) & 0xC0) == 0x80) {
        --end;
    }
    return content.substr(0, end);
}

// 读取前缀后可能停在一个多字节 UTF-8 字符中间；把不完整的尾字符去掉。
inline void TrimIncompleteUtf8Suffix(std::string* text) {
    if (text == nullptr || text->empty()) {
        return;
    }
    const std::size_t end = text->size();
    std::size_t start = end;
    while (start > 0 && (static_cast<unsigned char>((*text)[start - 1]) & 0xC0) == 0x80) {
        --start;
    }
    if (start == 0) {
        return;
    }

    const unsigned char lead = static_cast<unsigned char>((*text)[start - 1]);
    std::size_t need = 0;
    if ((lead & 0xE0) == 0xC0) {
        need = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        need = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        need = 4;
    }
    if (need != 0 && end - (start - 1) < need) {
        text->resize(start - 1);
    }
}

inline bool ReadFile(const std::filesystem::path& path, std::string* content, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open file";
        }
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        if (error != nullptr) {
            *error = "failed while reading file";
        }
        return false;
    }
    if (content != nullptr) {
        *content = buffer.str();
    }
    return true;
}

// 最多读取 limit 字节；文件更大时 *truncated=true，并保证尾部不残留半个 UTF-8 字符。
inline bool ReadFilePrefix(const std::filesystem::path& path, std::size_t limit,
                           std::string* content, bool* truncated, std::string* error) {
    if (truncated != nullptr) {
        *truncated = false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open file";
        }
        return false;
    }

    std::string buffer(limit, '\0');
    input.read(buffer.data(), static_cast<std::streamsize>(limit));
    if (input.bad()) {
        if (error != nullptr) {
            *error = "failed while reading file";
        }
        return false;
    }
    buffer.resize(static_cast<std::size_t>(input.gcount()));
    if (truncated != nullptr) {
        *truncated = input.peek() != std::char_traits<char>::eof();
    }
    TrimIncompleteUtf8Suffix(&buffer);
    if (content != nullptr) {
        *content = std::move(buffer);
    }
    return true;
}

}  // namespace aiagent::platform
