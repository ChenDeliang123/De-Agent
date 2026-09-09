#include "aiagent/tools/run_command.hpp"
#include "aiagent/log.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <poll.h>

namespace aiagent::tools {
namespace {

constexpr size_t kMaxCaptureBytes = 4 * 1024 * 1024;  // 内存累积上限，防无限输出
constexpr size_t kMaxTailBytes = 50 * 1024;           // 回传模型的尾部截断上限
constexpr int kMaxTailLines = 2000;

// 只保留尾部若干行；超出再按字节截断（不切半个 UTF-8 序列）。
std::string TruncateTail(const std::string& text) {
    size_t line_count = 0;
    size_t keep_start = 0;  // 默认保留全部
    for (size_t i = text.size(); i > 0; --i) {
        if (text[i - 1] == '\n') {
            ++line_count;
            if (line_count > static_cast<size_t>(kMaxTailLines)) {
                keep_start = i;
                break;
            }
        }
    }
    std::string tail = text.substr(keep_start);
    if (tail.size() > kMaxTailBytes) {
        size_t start = tail.size() - kMaxTailBytes;
        while (start < tail.size() && (static_cast<unsigned char>(tail[start]) & 0xC0) == 0x80) {
            ++start;
        }
        tail = "..." + tail.substr(start);
    }
    return tail;
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

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        result.error = "failed to create pipe: " + std::string(std::strerror(errno));
        return result;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        result.error = "failed to fork: " + std::string(std::strerror(errno));
        return result;
    }

    if (pid == 0) {
        // 子进程：独立进程组（便于超时整组清理），stdout/stderr 都汇入管道。
        setpgid(0, 0);
        if (!cwd.empty()) {
            (void)chdir(cwd.c_str());
        }
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(pipe_fds[1]);
    const int read_fd = pipe_fds[0];

    std::string output;
    bool capture = true;
    bool timed_out = false;
    std::string read_error;
    const bool has_timeout = timeout_seconds > 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(has_timeout ? timeout_seconds : 0);

    while (true) {
        int poll_timeout_ms = -1;
        if (has_timeout) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining.count() <= 0) {
                timed_out = true;
                break;
            }
            poll_timeout_ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count());
        }

        pollfd descriptor{read_fd, POLLIN, 0};
        const int poll_result = poll(&descriptor, 1, poll_timeout_ms);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            read_error = "poll failed: " + std::string(std::strerror(errno));
            break;
        }
        if (poll_result == 0) {
            timed_out = true;
            break;
        }

        char buffer[65536];
        const ssize_t count = read(read_fd, buffer, sizeof(buffer));
        if (count == 0) {
            break;  // EOF：子进程已关闭 stdout
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            read_error = "read failed: " + std::string(std::strerror(errno));
            break;
        }
        if (capture) {
            const size_t remaining_capacity = kMaxCaptureBytes - output.size();
            if (static_cast<size_t>(count) > remaining_capacity) {
                output.append(buffer, remaining_capacity);
                capture = false;  // 已达内存上限，后续只排空管道
            } else {
                output.append(buffer, static_cast<size_t>(count));
            }
        }
    }

    if (timed_out) {
        kill(-pid, SIGKILL);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }

    int exit_code = -1;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    }
    result.exit_code = exit_code;

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
