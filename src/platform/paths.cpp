#include "aiagent/platform/paths.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace aiagent::platform {
namespace {

std::vector<std::string> Split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(separator, start);
        if (end == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

#ifdef _WIN32

std::wstring Utf8ToWide(std::string_view text, UINT code_page, bool strict) {
    if (text.empty()) {
        return {};
    }
    const DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    const int length = MultiByteToWideChar(code_page, flags, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    const int written = MultiByteToWideChar(code_page, flags, text.data(),
                                            static_cast<int>(text.size()), result.data(),
                                            length);
    if (written <= 0) {
        return {};
    }
    result.resize(static_cast<std::size_t>(written));
    return result;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()), result.data(),
                                            length, nullptr, nullptr);
    if (written <= 0) {
        return {};
    }
    result.resize(static_cast<std::size_t>(written));
    return result;
}

bool FileExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

bool HasPathSeparator(std::string_view text) {
    return text.find('/') != std::string_view::npos ||
           text.find('\\') != std::string_view::npos;
}

#endif  // _WIN32

}  // namespace

std::filesystem::path FromUtf8(std::string_view text) {
#ifdef _WIN32
    std::wstring wide = Utf8ToWide(text, CP_UTF8, true);
    if (wide.empty() && !text.empty()) {
        // 兼容少量历史 ACP 路径；正常新代码不应走到这里。
        wide = Utf8ToWide(text, CP_ACP, false);
    }
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(std::string(text));
#endif
}

std::string ToUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::string converted = WideToUtf8(path.wstring());
    if (!converted.empty() || path.empty()) {
        return converted;
    }
    return path.string();
#else
    return path.string();
#endif
}

std::string ToGenericUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::u8string utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
#else
    return path.generic_string();
#endif
}

std::string NormalizePathKey(std::string path) {
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '/', '\\');
    for (char& c : path) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
#endif
    return path;
}

bool IsHiddenPath(const std::filesystem::path& path) {
    const std::filesystem::path name = path.filename();
    if (name.empty()) {
        return false;
    }
#ifdef _WIN32
    if (name.native().front() == L'.') {
        return true;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
#else
    return name.native().front() == '.';
#endif
}

std::string ExecutablePath() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            return WideToUtf8(std::wstring_view(buffer.data(), length));
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    std::vector<char> buffer(4096);
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return {};
    }
    return std::string(buffer.data(), static_cast<std::size_t>(length));
#endif
}

std::string NullDevice() {
#ifdef _WIN32
    return "NUL";
#else
    return "/dev/null";
#endif
}

std::string PathListSeparator() {
#ifdef _WIN32
    return ";";
#else
    return ":";
#endif
}

std::optional<std::string> GetEnvUtf8(std::string_view name) {
#ifdef _WIN32
    std::wstring wide_name = Utf8ToWide(name, CP_UTF8, true);
    if (wide_name.empty() && !name.empty()) {
        wide_name = Utf8ToWide(name, CP_ACP, false);
    }
    if (wide_name.empty()) {
        return std::nullopt;
    }
    const DWORD needed =
            GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (needed == 0) {
        return GetLastError() == ERROR_ENVVAR_NOT_FOUND
                       ? std::nullopt
                       : std::optional<std::string>(std::string());
    }
    std::wstring value(static_cast<std::size_t>(needed), L'\0');
    const DWORD written =
            GetEnvironmentVariableW(wide_name.c_str(), value.data(), needed);
    if (written == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        return std::nullopt;
    }
    value.resize(static_cast<std::size_t>(written));
    return WideToUtf8(value);
#else
    const std::string key(name);
    const char* value = std::getenv(key.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

bool SetEnvUtf8(std::string_view name, std::string_view value) {
#ifdef _WIN32
    const std::wstring wide_name = Utf8ToWide(name, CP_UTF8, true);
    const std::wstring wide_value = Utf8ToWide(value, CP_UTF8, true);
    if (wide_name.empty()) {
        return false;
    }
    return SetEnvironmentVariableW(wide_name.c_str(), wide_value.c_str()) != 0;
#else
    const std::string key(name);
    const std::string text(value);
    return ::setenv(key.c_str(), text.c_str(), 1) == 0;
#endif
}

std::filesystem::path TempDir() {
    std::error_code ec;
    const std::filesystem::path path = std::filesystem::temp_directory_path(ec);
    if (ec || path.empty()) {
        return std::filesystem::path(".");
    }
    return path;
}

std::string ResolveExecutable(std::string_view name, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (name.empty()) {
        if (error != nullptr) {
            *error = "empty executable name";
        }
        return {};
    }

#ifdef _WIN32
    const std::filesystem::path requested = FromUtf8(name);
    auto try_file = [](const std::filesystem::path& candidate) -> std::string {
        if (FileExists(candidate)) {
            return ToUtf8(candidate);
        }
        if (!candidate.has_extension()) {
            std::filesystem::path with_exe = candidate;
            with_exe += L".exe";
            if (FileExists(with_exe)) {
                return ToUtf8(with_exe);
            }
        }
        return {};
    };

    if (HasPathSeparator(name) || requested.has_parent_path()) {
        const std::string found = try_file(requested);
        if (found.empty() && error != nullptr) {
            *error = "executable not found: " + std::string(name);
        }
        return found;
    }

    std::vector<std::wstring> extensions;
    const std::optional<std::string> path_ext = GetEnvUtf8("PATHEXT");
    const std::string ext_text = path_ext.value_or(".COM;.EXE;.BAT;.CMD");
    for (const std::string& item : Split(ext_text, ';')) {
        if (!item.empty()) {
            extensions.push_back(Utf8ToWide(item, CP_UTF8, true));
        }
    }
    if (extensions.empty()) {
        extensions.push_back(L".EXE");
    }

    const std::optional<std::string> path_value = GetEnvUtf8("PATH");
    for (const std::string& directory : Split(path_value.value_or(""), ';')) {
        if (directory.empty()) {
            continue;
        }
        const std::filesystem::path base = FromUtf8(directory) / requested;
        if (FileExists(base)) {
            return ToUtf8(base);
        }
        for (const std::wstring& extension : extensions) {
            std::filesystem::path candidate = base;
            candidate += extension;
            if (FileExists(candidate)) {
                return ToUtf8(candidate);
            }
        }
    }
    if (error != nullptr) {
        *error = "executable not found in PATH: " + std::string(name);
    }
    return {};
#else
    if (name.find('/') != std::string_view::npos) {
        const std::string candidate(name);
        if (::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
        if (error != nullptr) {
            *error = "executable not found: " + candidate;
        }
        return {};
    }
    const std::optional<std::string> path_value = GetEnvUtf8("PATH");
    for (std::string directory : Split(path_value.value_or(""), ':')) {
        if (directory.empty()) {
            directory = ".";
        }
        const std::string candidate = directory + "/" + std::string(name);
        if (::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    if (error != nullptr) {
        *error = "executable not found in PATH: " + std::string(name);
    }
    return {};
#endif
}

std::string DecodeConsoleOutput(std::string_view bytes) {
#ifdef _WIN32
    if (bytes.empty()) {
        return {};
    }
    // 先按严格 UTF-8 判断：ASCII/现代 UTF-8 输出直接保留。
    const std::wstring utf8 = Utf8ToWide(bytes, CP_UTF8, true);
    if (!utf8.empty()) {
        return std::string(bytes);
    }
    // cmd.exe 重定向输出通常是 OEM 代码页（如 936/437）。
    const std::wstring wide = Utf8ToWide(bytes, GetOEMCP(), false);
    if (wide.empty()) {
        return std::string(bytes);
    }
    const std::string converted = WideToUtf8(wide);
    return converted.empty() ? std::string(bytes) : converted;
#else
    return std::string(bytes);
#endif
}

}  // namespace aiagent::platform
