#include "aiagent/log.hpp"

#include "aiagent/platform/paths.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace aiagent {
namespace {

// 注意：这里刻意用扁平字符数组而不是 `thread_local std::string`。
// MinGW-w64 16.2 + UCRT 上，非平凡 thread_local 对象在短生命周期线程退出时
// 会在 TLS 回调里触发堆损坏（可复现：8 线程 × 100 轮反复赋值后 join，
// 进程以 0xc0000374 退出）。日志是每个子 Agent worker 都会走的热路径，
// 所以把 TLS 状态保持为平凡可析构类型。
constexpr std::size_t kMaxRequestIdBytes = 255;
thread_local char g_request_id[kMaxRequestIdBytes + 1] = {};

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::kTrace:
            return "TRACE";
        case LogLevel::kDebug:
            return "DEBUG";
        case LogLevel::kInfo:
            return "INFO";
        case LogLevel::kWarning:
            return "WARN";
        case LogLevel::kError:
            return "ERROR";
        case LogLevel::kFatal:
            return "FATAL";
    }
    return "INFO";
}

std::string Lower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

std::string FormatTime() {
    const auto now = std::chrono::system_clock::now();
    const auto millis =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                    .count() %
            1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char buffer[40] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc);
    return std::string(buffer) + "." + (millis < 100 ? "0" : "") +
           (millis < 10 ? "0" : "") + std::to_string(millis) + "Z";
}

std::string RedactSecrets(std::string text) {
    constexpr const char* kApiPrefix = "sk-";
    std::size_t pos = 0;
    while ((pos = text.find(kApiPrefix, pos)) != std::string::npos) {
        std::size_t end = pos + 3;
        while (end < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) {
            ++end;
        }
        if (end > pos + 3) {
            text.replace(pos + 3, end - pos - 3, "***");
            pos = pos + 6;
        } else {
            pos += 3;
        }
    }
    return text;
}

}  // namespace

class Logger::Impl {
public:
    void Configure(const std::string& level, const std::string& file_path,
                   bool console, std::size_t max_size_bytes, int max_files) {
        std::lock_guard<std::mutex> lock(mutex_);
        level_ = Logger::ParseLevel(level);
        console_ = console;
        max_size_bytes_ = max_size_bytes;
        max_files_ = max_files > 0 ? max_files : 1;
        if (file_stream_.is_open()) {
            file_stream_.close();
        }
        file_path_ = file_path;
        if (!file_path_.empty()) {
            const std::filesystem::path path = aiagent::platform::FromUtf8(file_path_);
            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path());
            }
            file_stream_.open(path, std::ios::app | std::ios::binary);
        }
    }

    void Log(LogLevel level, const std::string& module,
             const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (level < level_) {
            return;
        }

        RotateIfNeeded();
        const std::string request_suffix =
                g_request_id[0] == '\0'
                        ? std::string()
                        : " req=" + std::string(g_request_id) + " ";
        std::ostringstream line;
        line << FormatTime() << ' ' << LevelName(level) << " [" << module << "] "
             << request_suffix << RedactSecrets(message) << '\n';
        if (file_stream_.is_open()) {
            file_stream_ << line.str();
            file_stream_.flush();
        }
        if (console_) {
            std::cout << line.str() << std::flush;
        }
    }

    bool IsEnabled(LogLevel level) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return level >= level_;
    }

private:
    void RotateIfNeeded() {
        if (!file_stream_.is_open() || file_path_.empty()) {
            return;
        }
        file_stream_.seekp(0, std::ios::end);
        const std::streampos size = file_stream_.tellp();
        if (size >= 0 && static_cast<std::size_t>(size) < max_size_bytes_) {
            return;
        }
        file_stream_.close();

        const std::filesystem::path path = aiagent::platform::FromUtf8(file_path_);
        for (int i = max_files_ - 1; i >= 1; --i) {
            const auto from = aiagent::platform::FromUtf8(file_path_ + "." + std::to_string(i));
            const auto to = aiagent::platform::FromUtf8(file_path_ + "." + std::to_string(i + 1));
            std::error_code error;
            std::filesystem::remove(to, error);
            if (std::filesystem::exists(from)) {
                std::filesystem::rename(from, to, error);
            }
        }
        const auto rotated = aiagent::platform::FromUtf8(file_path_ + ".1");
        std::error_code error;
        std::filesystem::remove(rotated, error);
        std::filesystem::rename(path, rotated, error);
        file_stream_.open(path, std::ios::trunc | std::ios::binary);
    }

    mutable std::mutex mutex_;
    LogLevel level_ = LogLevel::kInfo;
    std::string file_path_;
    std::ofstream file_stream_;
    bool console_ = false;
    std::size_t max_size_bytes_ = 10 * 1024 * 1024;
    int max_files_ = 5;
};

Logger::Logger() : impl_(std::make_unique<Impl>()) {}
Logger::~Logger() = default;

void Logger::Configure(const std::string& level, const std::string& file_path,
                       bool console, std::size_t max_size_bytes, int max_files) {
    impl_->Configure(level, file_path, console, max_size_bytes, max_files);
}

void Logger::Log(LogLevel level, const std::string& module,
                 const std::string& message) {
    impl_->Log(level, module, message);
}

bool Logger::IsEnabled(LogLevel level) const { return impl_->IsEnabled(level); }

LogLevel Logger::ParseLevel(const std::string& text) {
    const std::string value = Lower(text);
    if (value == "trace") {
        return LogLevel::kTrace;
    }
    if (value == "debug") {
        return LogLevel::kDebug;
    }
    if (value == "warning" || value == "warn") {
        return LogLevel::kWarning;
    }
    if (value == "error") {
        return LogLevel::kError;
    }
    if (value == "fatal") {
        return LogLevel::kFatal;
    }
    return LogLevel::kInfo;
}

Logger& DefaultLogger() {
    static Logger logger;
    return logger;
}

void ConfigureDefaultLogger(const std::string& level, const std::string& file_path,
                            bool console, std::size_t max_size_bytes,
                            int max_files) {
    DefaultLogger().Configure(level, file_path, console, max_size_bytes, max_files);
}

void SetThreadRequestId(const std::string& request_id) {
    const std::size_t count =
            std::min(request_id.size(), kMaxRequestIdBytes);
    std::memcpy(g_request_id, request_id.data(), count);
    g_request_id[count] = '\0';
}

void ClearThreadRequestId() { g_request_id[0] = '\0'; }

}  // namespace aiagent
