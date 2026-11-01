#include "aiagent/review/review_process.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "aiagent/platform/process.hpp"
#include "aiagent/platform/paths.hpp"

namespace aiagent::review {

ArgvRunResult RunArgvCapture(const std::vector<std::string>& argv,
                             const std::string& cwd,
                             long timeout_seconds,
                             std::size_t max_output_bytes,
                             bool keep_tail,
                             const std::function<bool()>& should_cancel) {
    ArgvRunResult result;
    if (argv.empty()) {
        result.started = false;
        result.error = "argv must not be empty";
        return result;
    }
    if (max_output_bytes == 0) {
        max_output_bytes = 1;
    }

    platform::ProcessOptions options;
    options.argv = argv;
    options.cwd = cwd;
    options.stderr_mode = platform::StderrMode::kMergeToStdout;
    std::string start_error;
    auto process = platform::ChildProcess::Start(options, &start_error);
    if (process == nullptr) {
        result.started = false;
        result.error = start_error.empty() ? "failed to start process" : start_error;
        return result;
    }
    process->CloseStdin();

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(std::max<long>(1, timeout_seconds));
    std::string data;
    char buffer[8192];
    while (true) {
        if (should_cancel && should_cancel()) {
            result.cancelled = true;
            process->Terminate();
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            process->Terminate();
            break;
        }
        std::size_t bytes_read = 0;
        std::string read_error;
        const platform::ReadStatus status = process->Read(
                buffer, sizeof(buffer), 100, &bytes_read, &read_error);
        if (status == platform::ReadStatus::kData) {
            if (keep_tail) {
                data.append(buffer, bytes_read);
                if (data.size() > max_output_bytes) {
                    data.erase(0, data.size() - max_output_bytes);
                    result.truncated = true;
                }
            } else if (data.size() < max_output_bytes) {
                const std::size_t take =
                        std::min(max_output_bytes - data.size(), bytes_read);
                data.append(buffer, take);
                if (take < bytes_read) {
                    result.truncated = true;
                }
            } else {
                result.truncated = true;
            }
            continue;
        }
        if (status == platform::ReadStatus::kEof) {
            break;
        }
        if (status == platform::ReadStatus::kError) {
            if (!read_error.empty()) {
                result.error = read_error;
            }
            process->Terminate();
            break;
        }
        // kTimeout：继续等待数据/截止时间。
    }

    int exit_code = -1;
    std::string wait_error;
    if (result.timed_out || result.cancelled) {
        if (!process->WaitForExit(2000, &exit_code, &wait_error)) {
            result.error = wait_error.empty() ? "process did not exit after termination"
                                              : wait_error;
        }
    } else {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining =
                now >= deadline
                        ? 0
                        : static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   deadline - now)
                                                   .count());
        if (!process->WaitForExit(remaining, &exit_code, &wait_error)) {
            result.timed_out = true;
            process->Terminate();
            process->WaitForExit(2000, &exit_code, &wait_error);
        }
    }
    result.exit_code = exit_code;
    if (!result.timed_out && !result.cancelled && !result.error.empty() && exit_code < 0) {
        // 启动成功但回收失败：保留 error。
    }
    result.duration_ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now() - started)
                                                   .count());
    // 控制台输出按 UTF-8 解码（Windows OEM 代码页）。
    result.output = platform::DecodeConsoleOutput(data);
    return result;
}

}  // namespace aiagent::review
