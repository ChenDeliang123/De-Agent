#include "aiagent/log.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace aiagent {
namespace {

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

}  // namespace

class Logger::Impl {
public:
    void Configure(const std::string& level, const std::string& file_path,
                   bool console) {
        std::lock_guard<std::mutex> lock(mutex_);
        level_ = Logger::ParseLevel(level);
        console_ = console;
        if (file_stream_.is_open()) {
            file_stream_.close();
        }
        file_path_ = file_path;
        if (!file_path_.empty()) {
            const std::filesystem::path path(file_path_);
            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path());
            }
            file_stream_.open(file_path_, std::ios::app);
        }
    }

    void Log(LogLevel level, const std::string& module,
             const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (level < level_) {
            return;
        }

        std::ostringstream line;
        line << FormatTime() << ' ' << LevelName(level) << " [" << module << "] "
             << message << '\n';
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
    mutable std::mutex mutex_;
    LogLevel level_ = LogLevel::kInfo;
    std::string file_path_;
    std::ofstream file_stream_;
    bool console_ = false;
};

Logger::Logger() : impl_(std::make_unique<Impl>()) {}
Logger::~Logger() = default;

void Logger::Configure(const std::string& level, const std::string& file_path,
                       bool console) {
    impl_->Configure(level, file_path, console);
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

void ConfigureDefaultLogger(const std::string& level,
                            const std::string& file_path, bool console) {
    DefaultLogger().Configure(level, file_path, console);
}

}  // namespace aiagent
