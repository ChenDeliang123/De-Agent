#include "aiagent/platform/process.hpp"

#include "aiagent/platform/paths.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace aiagent::platform {
namespace {

int NormalizeExitStatus(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

}  // namespace

struct ChildProcess::Impl {
    pid_t pid = -1;
    int stdin_fd = -1;
    int stdout_fd = -1;
    int last_exit_code = -1;
};

ChildProcess::ChildProcess() : impl_(std::make_unique<Impl>()) {}

ChildProcess::~ChildProcess() {
    if (impl_->pid > 0) {
        Terminate();
        int code = -1;
        std::string error;
        WaitForExit(1000, &code, &error);
    }
    if (impl_->stdin_fd >= 0) {
        ::close(impl_->stdin_fd);
        impl_->stdin_fd = -1;
    }
    if (impl_->stdout_fd >= 0) {
        ::close(impl_->stdout_fd);
        impl_->stdout_fd = -1;
    }
}

std::unique_ptr<ChildProcess> ChildProcess::Start(const ProcessOptions& options,
                                                  std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (options.argv.empty() || options.argv.front().empty()) {
        if (error != nullptr) {
            *error = "empty process argv";
        }
        return nullptr;
    }

    std::string resolve_error;
    const std::string executable =
            ResolveExecutable(options.argv.front(), &resolve_error);
    if (executable.empty()) {
        if (error != nullptr) {
            *error = resolve_error;
        }
        return nullptr;
    }

    if (!options.cwd.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(FromUtf8(options.cwd), ec) || ec) {
            if (error != nullptr) {
                *error = "working directory does not exist: " + options.cwd;
            }
            return nullptr;
        }
    }

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (::pipe(stdin_pipe) != 0) {
        if (error != nullptr) {
            *error = "failed to create stdin pipe: " + std::string(std::strerror(errno));
        }
        return nullptr;
    }
    if (::pipe(stdout_pipe) != 0) {
        const int saved = errno;
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        if (error != nullptr) {
            *error = "failed to create stdout pipe: " + std::string(std::strerror(saved));
        }
        return nullptr;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int saved = errno;
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        if (error != nullptr) {
            *error = "failed to fork: " + std::string(std::strerror(saved));
        }
        return nullptr;
    }

    if (pid == 0) {
        // 子进程：独立进程组，便于超时时整组清理。
        (void)::setpgid(0, 0);

        if (!options.cwd.empty() && ::chdir(options.cwd.c_str()) != 0) {
            _exit(127);
        }

        (void)::dup2(stdin_pipe[0], STDIN_FILENO);
        (void)::dup2(stdout_pipe[1], STDOUT_FILENO);
        switch (options.stderr_mode) {
            case StderrMode::kMergeToStdout:
                (void)::dup2(stdout_pipe[1], STDERR_FILENO);
                break;
            case StderrMode::kDiscard: {
                const int dev_null = ::open("/dev/null", O_WRONLY);
                if (dev_null >= 0) {
                    (void)::dup2(dev_null, STDERR_FILENO);
                    ::close(dev_null);
                }
                break;
            }
            case StderrMode::kInherit:
                break;
        }

        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);

        for (const auto& [name, value] : options.env) {
            (void)::setenv(name.c_str(), value.c_str(), 1);
        }

        std::vector<char*> argv;
        argv.reserve(options.argv.size() + 1);
        for (const std::string& arg : options.argv) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        ::execv(executable.c_str(), argv.data());
        _exit(127);
    }

    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);

    auto process = std::unique_ptr<ChildProcess>(new ChildProcess());
    process->impl_->pid = pid;
    process->impl_->stdin_fd = stdin_pipe[1];
    process->impl_->stdout_fd = stdout_pipe[0];
    return process;
}

bool ChildProcess::Write(std::string_view data, std::string* error) {
    if (impl_->stdin_fd < 0) {
        if (error != nullptr) {
            *error = "stdin is closed";
        }
        return false;
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written =
                ::write(impl_->stdin_fd, data.data() + offset, data.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (error != nullptr) {
                *error = "failed to write to child stdin: " +
                         std::string(std::strerror(errno));
            }
            return false;
        }
        if (written == 0) {
            if (error != nullptr) {
                *error = "failed to write to child stdin: zero-byte write";
            }
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool ChildProcess::CloseStdin() {
    if (impl_->stdin_fd >= 0) {
        ::close(impl_->stdin_fd);
        impl_->stdin_fd = -1;
    }
    return true;
}

ReadStatus ChildProcess::Read(char* buffer, std::size_t size, int timeout_ms,
                              std::size_t* bytes_read, std::string* error) {
    if (bytes_read != nullptr) {
        *bytes_read = 0;
    }
    if (impl_->stdout_fd < 0) {
        if (error != nullptr) {
            *error = "stdout is closed";
        }
        return ReadStatus::kError;
    }
    if (buffer == nullptr || size == 0) {
        if (error != nullptr) {
            *error = "empty read buffer";
        }
        return ReadStatus::kError;
    }

    const auto deadline = timeout_ms < 0
                                  ? std::chrono::steady_clock::time_point::max()
                                  : std::chrono::steady_clock::now() +
                                            std::chrono::milliseconds(timeout_ms);
    while (true) {
        int wait_ms = -1;
        if (timeout_ms >= 0) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining.count() <= 0) {
                if (error != nullptr) {
                    *error = "timeout";
                }
                return ReadStatus::kTimeout;
            }
            wait_ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count());
            if (wait_ms < 0) {
                wait_ms = 0;
            }
        }

        pollfd descriptor{impl_->stdout_fd, POLLIN, 0};
        const int poll_result = ::poll(&descriptor, 1, wait_ms);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (error != nullptr) {
                *error = "poll failed: " + std::string(std::strerror(errno));
            }
            return ReadStatus::kError;
        }
        if (poll_result == 0) {
            if (error != nullptr) {
                *error = "timeout";
            }
            return ReadStatus::kTimeout;
        }

        const ssize_t count = ::read(impl_->stdout_fd, buffer, size);
        if (count == 0) {
            return ReadStatus::kEof;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (error != nullptr) {
                *error = "read failed: " + std::string(std::strerror(errno));
            }
            return ReadStatus::kError;
        }
        if (bytes_read != nullptr) {
            *bytes_read = static_cast<std::size_t>(count);
        }
        return ReadStatus::kData;
    }
}

bool ChildProcess::WaitForExit(int timeout_ms, int* exit_code, std::string* error) {
    if (exit_code != nullptr) {
        *exit_code = impl_->last_exit_code;
    }
    if (impl_->pid <= 0) {
        return true;
    }

    const auto deadline = timeout_ms < 0
                                  ? std::chrono::steady_clock::time_point::max()
                                  : std::chrono::steady_clock::now() +
                                            std::chrono::milliseconds(timeout_ms);
    while (true) {
        int status = 0;
        const pid_t result = ::waitpid(impl_->pid, &status, WNOHANG);
        if (result == impl_->pid) {
            impl_->last_exit_code = NormalizeExitStatus(status);
            impl_->pid = -1;
            if (exit_code != nullptr) {
                *exit_code = impl_->last_exit_code;
            }
            return true;
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                // 已被回收（例如 Terminate 后由别处 wait）；返回最后一次已知值。
                impl_->pid = -1;
                return true;
            }
            if (error != nullptr) {
                *error = "waitpid failed: " + std::string(std::strerror(errno));
            }
            return false;
        }
        if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ChildProcess::Terminate() {
    if (impl_->pid <= 0) {
        return;
    }
    if (::kill(-impl_->pid, SIGKILL) != 0) {
        // 进程组可能尚未建立或已消失；退回单进程 kill。
        (void)::kill(impl_->pid, SIGKILL);
    }
}

bool ChildProcess::IsRunning() const {
    return impl_->pid > 0;
}

long ChildProcess::Pid() const {
    return static_cast<long>(impl_->pid);
}

}  // namespace aiagent::platform
