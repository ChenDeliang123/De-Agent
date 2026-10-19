#include "aiagent/platform/process.hpp"

#include "aiagent/platform/paths.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>

namespace aiagent::platform {
namespace {

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                            static_cast<int>(text.size()), result.data(),
                                            length);
    if (written <= 0) {
        return {};
    }
    result.resize(static_cast<std::size_t>(written));
    return result;
}

std::string WinErrorText(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return "Windows error " + std::to_string(code);
    }
    std::wstring text(buffer, length);
    LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
        text.pop_back();
    }
    const std::string utf8 = ToUtf8(std::filesystem::path(text));
    return utf8.empty() ? "Windows error " + std::to_string(code) : utf8;
}

std::wstring QuoteArg(const std::wstring& arg) {
    if (arg.empty()) {
        return L"\"\"";
    }
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }

    std::wstring quoted = L"\"";
    for (std::size_t i = 0; i < arg.size(); ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(arg[i]);
        }
    }
    quoted.push_back(L'"');
    return quoted;
}

struct WideCaseInsensitiveLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    }
};

bool BuildEnvironmentBlock(const std::vector<std::pair<std::string, std::string>>& overrides,
                           std::vector<wchar_t>* block, std::string* error) {
    std::map<std::wstring, std::wstring, WideCaseInsensitiveLess> variables;
    LPWCH current = GetEnvironmentStringsW();
    if (current == nullptr) {
        if (error != nullptr) {
            *error = "failed to read parent environment: " +
                     WinErrorText(GetLastError());
        }
        return false;
    }
    for (LPWCH item = current; *item != L'\0'; item += std::wcslen(item) + 1) {
        const std::wstring entry(item);
        if (entry.empty() || entry.front() == L'=') {
            continue;
        }
        const std::size_t equals = entry.find(L'=');
        if (equals == std::wstring::npos) {
            continue;
        }
        variables[entry.substr(0, equals)] = entry.substr(equals + 1);
    }
    FreeEnvironmentStringsW(current);

    for (const auto& [name, value] : overrides) {
        if (name.empty() || name.find('=') != std::string::npos) {
            if (error != nullptr) {
                *error = "invalid environment variable name: " + name;
            }
            return false;
        }
        const std::wstring wide_name = Utf8ToWide(name);
        const std::wstring wide_value = Utf8ToWide(value);
        if (wide_name.empty()) {
            if (error != nullptr) {
                *error = "invalid environment variable name: " + name;
            }
            return false;
        }
        variables[wide_name] = wide_value;
    }

    block->clear();
    for (const auto& [name, value] : variables) {
        block->insert(block->end(), name.begin(), name.end());
        block->push_back(L'=');
        block->insert(block->end(), value.begin(), value.end());
        block->push_back(L'\0');
    }
    block->push_back(L'\0');
    return true;
}

bool CreateInheritablePipe(HANDLE* read_end, HANDLE* write_end, std::string* error) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    if (!CreatePipe(read_end, write_end, &attributes, 0)) {
        if (error != nullptr) {
            *error = "failed to create pipe: " + WinErrorText(GetLastError());
        }
        return false;
    }
    return true;
}

void CloseIfValid(HANDLE* handle) {
    if (handle != nullptr && *handle != nullptr && *handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
    }
    if (handle != nullptr) {
        *handle = nullptr;
    }
}

}  // namespace

struct ChildProcess::Impl {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
    DWORD pid = 0;
    std::thread reader;
    std::mutex mutex;
    std::condition_variable condition;
    std::string buffer;
    bool eof = false;
    std::string read_error;
    bool running = false;
    int last_exit_code = -1;
};

ChildProcess::ChildProcess() : impl_(std::make_unique<Impl>()) {}

ChildProcess::~ChildProcess() {
    if (impl_->process != nullptr) {
        Terminate();
        int code = -1;
        std::string error;
        if (!WaitForExit(1000, &code, &error)) {
            Terminate();
            (void)WaitForExit(-1, &code, &error);
        }
    }
    if (impl_->reader.joinable()) {
        impl_->reader.join();
    }
    CloseIfValid(&impl_->stdin_write);
    CloseIfValid(&impl_->stdout_read);
    CloseIfValid(&impl_->job);
    CloseIfValid(&impl_->process);
}

std::unique_ptr<ChildProcess> ChildProcess::Start(const ProcessOptions& options,
                                                  std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (options.argv.empty() && options.raw_command_line.empty()) {
        if (error != nullptr) {
            *error = "empty process argv";
        }
        return nullptr;
    }

    const std::string executable_name =
            options.argv.empty() ? "cmd.exe" : options.argv.front();
    std::string resolve_error;
    const std::string executable = ResolveExecutable(executable_name, &resolve_error);
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

    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_write = nullptr;
    HANDLE nul = nullptr;

    auto cleanup = [&] {
        CloseIfValid(&stdin_read);
        CloseIfValid(&stdin_write);
        CloseIfValid(&stdout_read);
        CloseIfValid(&stdout_write);
        if (nul != nullptr) {
            CloseIfValid(&nul);
        }
    };

    if (!CreateInheritablePipe(&stdin_read, &stdin_write, error)) {
        cleanup();
        return nullptr;
    }
    if (!CreateInheritablePipe(&stdout_read, &stdout_write, error)) {
        cleanup();
        return nullptr;
    }

    // 父进程持有的两端不能被继承。
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    switch (options.stderr_mode) {
        case StderrMode::kMergeToStdout:
            stderr_write = stdout_write;
            break;
        case StderrMode::kDiscard: {
            SECURITY_ATTRIBUTES attributes{};
            attributes.nLength = sizeof(attributes);
            attributes.bInheritHandle = TRUE;
            nul = CreateFileW(L"NUL", GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (nul == INVALID_HANDLE_VALUE || nul == nullptr) {
                if (error != nullptr) {
                    *error = "failed to open NUL: " + WinErrorText(GetLastError());
                }
                nul = nullptr;
                cleanup();
                return nullptr;
            }
            stderr_write = nul;
            break;
        }
        case StderrMode::kInherit:
            stderr_write = GetStdHandle(STD_ERROR_HANDLE);
            if (stderr_write == nullptr || stderr_write == INVALID_HANDLE_VALUE) {
                SECURITY_ATTRIBUTES attributes{};
                attributes.nLength = sizeof(attributes);
                attributes.bInheritHandle = TRUE;
                nul = CreateFileW(L"NUL", GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                stderr_write = nul;
            }
            break;
    }

    std::wstring command_line;
    if (!options.raw_command_line.empty()) {
        command_line = Utf8ToWide(options.raw_command_line);
    } else {
        for (std::size_t i = 0; i < options.argv.size(); ++i) {
            if (i != 0) {
                command_line.push_back(L' ');
            }
            command_line += QuoteArg(Utf8ToWide(options.argv[i]));
        }
    }

    std::vector<wchar_t> environment_block;
    LPVOID environment = nullptr;
    if (!options.env.empty()) {
        if (!BuildEnvironmentBlock(options.env, &environment_block, error)) {
            cleanup();
            return nullptr;
        }
        environment = environment_block.data();
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;

    const std::wstring wide_executable = Utf8ToWide(executable);
    const std::wstring wide_cwd =
            options.cwd.empty() ? std::wstring() : Utf8ToWide(options.cwd);
    std::wstring mutable_command_line = command_line;

    PROCESS_INFORMATION process_info{};
    const DWORD creation_flags =
            CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    const BOOL created = CreateProcessW(
            wide_executable.c_str(), mutable_command_line.data(), nullptr, nullptr, TRUE,
            creation_flags, environment, options.cwd.empty() ? nullptr : wide_cwd.c_str(),
            &startup, &process_info);
    if (!created) {
        if (error != nullptr) {
            *error = "failed to start " + executable + ": " +
                     WinErrorText(GetLastError());
        }
        cleanup();
        return nullptr;
    }

    // 父进程不再需要子进程端。
    CloseIfValid(&stdin_read);
    CloseIfValid(&stdout_write);
    if (nul != nullptr) {
        CloseIfValid(&nul);
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION information{};
        information.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        const BOOL configured = SetInformationJobObject(
                job, JobObjectExtendedLimitInformation, &information,
                sizeof(information));
        const BOOL assigned = configured && AssignProcessToJobObject(job, process_info.hProcess);
        if (!assigned) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    if (ResumeThread(process_info.hThread) == static_cast<DWORD>(-1)) {
        if (error != nullptr) {
            *error = "failed to resume child process: " +
                     WinErrorText(GetLastError());
        }
        TerminateProcess(process_info.hProcess, 1);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        if (job != nullptr) {
            CloseHandle(job);
        }
        CloseIfValid(&stdin_write);
        CloseIfValid(&stdout_read);
        return nullptr;
    }
    CloseHandle(process_info.hThread);

    auto process = std::unique_ptr<ChildProcess>(new ChildProcess());
    process->impl_->process = process_info.hProcess;
    process->impl_->job = job;
    process->impl_->stdin_write = stdin_write;
    process->impl_->stdout_read = stdout_read;
    process->impl_->pid = process_info.dwProcessId;
    process->impl_->running = true;

    process->impl_->reader = std::thread([impl = process->impl_.get()] {
        std::vector<char> buffer(64 * 1024);
        while (true) {
            DWORD count = 0;
            const BOOL ok = ReadFile(impl->stdout_read, buffer.data(),
                                     static_cast<DWORD>(buffer.size()), &count, nullptr);
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (!ok || count == 0) {
                if (!ok) {
                    const DWORD code = GetLastError();
                    if (code != ERROR_BROKEN_PIPE && code != ERROR_PIPE_NOT_CONNECTED &&
                        code != ERROR_OPERATION_ABORTED && code != ERROR_INVALID_HANDLE) {
                        impl->read_error = WinErrorText(code);
                    }
                }
                impl->eof = true;
                impl->condition.notify_all();
                return;
            }
            impl->buffer.append(buffer.data(), count);
            impl->condition.notify_all();
        }
    });

    return process;
}

bool ChildProcess::Write(std::string_view data, std::string* error) {
    if (impl_->stdin_write == nullptr) {
        if (error != nullptr) {
            *error = "stdin is closed";
        }
        return false;
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const DWORD chunk = static_cast<DWORD>(
                std::min<std::size_t>(data.size() - offset, 64 * 1024));
        DWORD written = 0;
        if (!WriteFile(impl_->stdin_write, data.data() + offset, chunk, &written,
                       nullptr)) {
            if (error != nullptr) {
                *error = "failed to write to child stdin: " +
                         WinErrorText(GetLastError());
            }
            return false;
        }
        if (written == 0) {
            if (error != nullptr) {
                *error = "failed to write to child stdin: zero-byte write";
            }
            return false;
        }
        offset += written;
    }
    return true;
}

bool ChildProcess::CloseStdin() {
    CloseIfValid(&impl_->stdin_write);
    return true;
}

ReadStatus ChildProcess::Read(char* buffer, std::size_t size, int timeout_ms,
                              std::size_t* bytes_read, std::string* error) {
    if (bytes_read != nullptr) {
        *bytes_read = 0;
    }
    if (buffer == nullptr || size == 0) {
        if (error != nullptr) {
            *error = "empty read buffer";
        }
        return ReadStatus::kError;
    }

    std::unique_lock<std::mutex> lock(impl_->mutex);
    auto ready = [this] {
        return !impl_->buffer.empty() || impl_->eof || !impl_->read_error.empty();
    };
    if (timeout_ms < 0) {
        impl_->condition.wait(lock, ready);
    } else if (!impl_->condition.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                          ready)) {
        if (error != nullptr) {
            *error = "timeout";
        }
        return ReadStatus::kTimeout;
    }

    if (!impl_->read_error.empty()) {
        if (error != nullptr) {
            *error = impl_->read_error;
        }
        return ReadStatus::kError;
    }
    if (impl_->buffer.empty()) {
        return ReadStatus::kEof;
    }

    const std::size_t count = std::min(size, impl_->buffer.size());
    std::memcpy(buffer, impl_->buffer.data(), count);
    impl_->buffer.erase(0, count);
    if (bytes_read != nullptr) {
        *bytes_read = count;
    }
    return ReadStatus::kData;
}

bool ChildProcess::WaitForExit(int timeout_ms, int* exit_code, std::string* error) {
    if (exit_code != nullptr) {
        *exit_code = impl_->last_exit_code;
    }
    if (impl_->process == nullptr) {
        return true;
    }

    const DWORD wait_ms =
            timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
    const DWORD result = WaitForSingleObject(impl_->process, wait_ms);
    if (result == WAIT_TIMEOUT) {
        return false;
    }
    if (result != WAIT_OBJECT_0) {
        if (error != nullptr) {
            *error = "wait failed: " + WinErrorText(GetLastError());
        }
        return false;
    }

    DWORD code = 0;
    if (!GetExitCodeProcess(impl_->process, &code)) {
        if (error != nullptr) {
            *error = "failed to read exit code: " + WinErrorText(GetLastError());
        }
        return false;
    }
    impl_->last_exit_code = static_cast<int>(code);
    impl_->running = false;
    if (exit_code != nullptr) {
        *exit_code = impl_->last_exit_code;
    }
    return true;
}

void ChildProcess::Terminate() {
    if (impl_->job != nullptr) {
        (void)TerminateJobObject(impl_->job, 1);
    } else if (impl_->process != nullptr) {
        (void)TerminateProcess(impl_->process, 1);
    }
}

bool ChildProcess::IsRunning() const {
    if (impl_->process == nullptr) {
        return false;
    }
    return WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT;
}

long ChildProcess::Pid() const {
    return static_cast<long>(impl_->pid);
}

}  // namespace aiagent::platform
