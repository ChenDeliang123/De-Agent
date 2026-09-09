#pragma once

#include <memory>
#include <string>

namespace aiagent {

enum class LogLevel {
    kTrace,
    kDebug,
    kInfo,
    kWarning,
    kError,
    kFatal,
};

// 线程安全、轻量日志器。P0 只支持文件 + 可选 stdout，不引入第三方日志库。
class Logger {
public:
    Logger();
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void Configure(const std::string& level, const std::string& file_path,
                   bool console = false);
    void Log(LogLevel level, const std::string& module,
             const std::string& message);
    bool IsEnabled(LogLevel level) const;

    static LogLevel ParseLevel(const std::string& text);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

Logger& DefaultLogger();
void ConfigureDefaultLogger(const std::string& level,
                            const std::string& file_path,
                            bool console = false);

}  // namespace aiagent
