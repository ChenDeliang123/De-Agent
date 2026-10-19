#include "aiagent/tools/run_command.hpp"

#include "aiagent/log.hpp"
#include "aiagent/platform/paths.hpp"
#include "aiagent/platform/process.hpp"

#include <algorithm>
#include <chrono>
#include <string>

namespace aiagent::tools {
namespace {

constexpr std::size_t kMaxCaptureBytes = 4 * 1024 * 1024;  // 内存累积上限，防无限输出
constexpr std::size_t kMaxTailBytes = 50 * 1024;           // 回传模型的尾部截断上限
constexpr int kMaxTailLines = 2000;

// 只保留尾部若干行；超出再按字节截断（不切半个 UTF-8 序列）。
std::string TruncateTail(const std::string& text) {
    std::size_t line_count = 0;
    std::size_t keep_start = 0;  // 默认保留全部
    for (std::size_t i = text.size(); i > 0; --i) {
        if (text[i - 1] == '\n') {
            ++line_count;
            if (line_count > static_cast<std::size_t>(kMaxTailLines)) {
                keep_start = i;
                break;
            }
        }
    }
    std::string tail = text.substr(keep_start);
    if (tail.size() > kMaxTailBytes) {
        std::size_t start = tail.size() - kMaxTailBytes;
        while (start < tail.size() &&
               (static_cast<unsigned char>(tail[start]) & 0xC0) == 0x80) {
            ++start;
        }
        tail = "..." + tail.substr(start);
    }
    return tail;
}

platform::ProcessOptions ShellOptions(const std::string& command) {
    platform::ProcessOptions options;
#ifdef _WIN32
    // cmd 的 /s + 外层引号规则：命令原文交给 cmd 解析，避免逐参数转义破坏
    // &&、|、>、%VAR% 等语法。
    options.argv = {"cmd.exe"};
    options.raw_command_line = "cmd.exe /d /s /c " + command;
#else
    options.argv = {"/bin/sh", "-c", command};
#endif
    options.stderr_mode = platform::StderrMode::kMergeToStdout;
    return options;
}

}  // namespace

CommandResult RunCommand(const std::string& command, const std::string& cwd,
                         long timeout_seconds) {
    CommandResult result;
    const auto started = std::chrono::steady_clock::now();
    aiagent::DefaultLogger().Log(
            aiagent::LogLevel::kDebug, "tools",
            "tool=bash event=start command_bytes=" + std::to_string(command.size()) +
                    " cwd=" + cwd + " timeout=" + std::to_string(timeout_seconds));

    platform::ProcessOptions options = ShellOptions(command);
    options.cwd = cwd;

    std::string start_error;
    std::unique_ptr<platform::ChildProcess> process =
            platform::ChildProcess::Start(options, &start_error);
    if (!process) {
        result.error = start_error;
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kError, "tools",
                "tool=bash event=end ok=false error=" + start_error);
        return result;
    }

    std::string output;
    bool capture = true;
    bool timed_out = false;
    std::string read_error;
    const bool has_timeout = timeout_seconds > 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(has_timeout ? timeout_seconds : 0);

    while (true) {
        int read_timeout_ms = -1;
        if (has_timeout) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining.count() <= 0) {
                timed_out = true;
                break;
            }
            read_timeout_ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
                            .count());
            if (read_timeout_ms < 1) {
                read_timeout_ms = 1;
            }
        }

        char buffer[65536];
        std::size_t count = 0;
        const platform::ReadStatus status = process->Read(
                buffer, sizeof(buffer), read_timeout_ms, &count, &read_error);
        if (status == platform::ReadStatus::kTimeout) {
            timed_out = true;
            read_error.clear();
            break;
        }
        if (status == platform::ReadStatus::kError) {
            break;
        }
        if (status == platform::ReadStatus::kEof) {
            break;
        }

        if (capture) {
            const std::size_t remaining_capacity = kMaxCaptureBytes - output.size();
            if (count > remaining_capacity) {
                output.append(buffer, remaining_capacity);
                capture = false;  // 已达内存上限，后续只排空管道
            } else {
                output.append(buffer, count);
            }
        }
    }

    if (timed_out) {
        process->Terminate();
    }

    int exit_code = -1;
    std::string wait_error;
    if (!process->WaitForExit(-1, &exit_code, &wait_error) &&
        read_error.empty()) {
        read_error = wait_error;
    }
    result.exit_code = exit_code;

    output = platform::DecodeConsoleOutput(output);

    if (!read_error.empty()) {
        result.ok = false;
        result.error = read_error;
        result.output = TruncateTail(output);
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kError, "tools",
                "tool=bash event=end ok=false error=" + read_error);
        return result;
    }
    if (timed_out) {
        result.ok = false;
        result.error = "command timed out after " + std::to_string(timeout_seconds) + "s";
        result.output = TruncateTail(output);
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kWarning, "tools",
                "tool=bash event=end ok=false reason=timeout timeout=" +
                        std::to_string(timeout_seconds));
        return result;
    }
    if (exit_code != 0) {
        result.ok = false;
        result.error = "command exited with code " + std::to_string(exit_code);
        result.output = TruncateTail(output);
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kWarning, "tools",
                "tool=bash event=end ok=false exit=" + std::to_string(exit_code));
        return result;
    }

    result.ok = true;
    result.output = TruncateTail(output);
    if (result.output.empty()) {
        result.output = "(no output)";
    }
    const auto duration_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count();
    aiagent::DefaultLogger().Log(
            aiagent::LogLevel::kInfo, "tools",
            "tool=bash event=end ok=true exit=0 duration_ms=" +
                    std::to_string(duration_ms) + " output_bytes=" +
                    std::to_string(result.output.size()));
    return result;
}

}  // namespace aiagent::tools
