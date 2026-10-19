// ChildProcess 平台层冒烟测试：POSIX 与 Windows 共用。
#include "aiagent/platform/paths.hpp"
#include "aiagent/platform/process.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

std::vector<std::string> ShellArgv(const std::string& command) {
#ifdef _WIN32
    return {"cmd.exe", "/d", "/s", "/c", command};
#else
    return {"/bin/sh", "-c", command};
#endif
}

std::string ReadAll(aiagent::platform::ChildProcess& process, int timeout_ms,
                    bool* timed_out) {
    std::string output;
    if (timed_out != nullptr) {
        *timed_out = false;
    }
    while (true) {
        char buffer[8192];
        std::size_t count = 0;
        std::string error;
        const auto status = process.Read(buffer, sizeof(buffer), timeout_ms, &count,
                                         &error);
        if (status == aiagent::platform::ReadStatus::kData) {
            output.append(buffer, count);
            continue;
        }
        if (status == aiagent::platform::ReadStatus::kEof) {
            return output;
        }
        if (status == aiagent::platform::ReadStatus::kTimeout) {
            if (timed_out != nullptr) {
                *timed_out = true;
            }
            return output;
        }
        Expect(false, "read failed: " + error);
        return output;
    }
}

int RunSimple(const std::string& command, const std::string& cwd,
              const std::vector<std::pair<std::string, std::string>>& env,
              std::string* output) {
    aiagent::platform::ProcessOptions options;
    options.argv = ShellArgv(command);
    options.cwd = cwd;
    options.env = env;
    options.stderr_mode = aiagent::platform::StderrMode::kMergeToStdout;

    std::string error;
    auto process = aiagent::platform::ChildProcess::Start(options, &error);
    if (!process) {
        Expect(false, "start failed: " + error);
        return -1;
    }
    *output = ReadAll(*process, 5000, nullptr);
    int exit_code = -1;
    if (!process->WaitForExit(5000, &exit_code, &error)) {
        Expect(false, "wait failed: " + error);
    }
    return exit_code;
}

}  // namespace

int main() {
    const std::string unique =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path temp_root =
            aiagent::platform::TempDir() / ("aiagent_process_" + unique);
    std::error_code ec;
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        std::cerr << "FAILED: cannot create temp dir: " << temp_root.string() << '\n';
        return 1;
    }

    {
        std::string output;
        const int code = RunSimple("echo hello", "", {}, &output);
        Expect(code == 0, "echo exit code should be 0, got " + std::to_string(code));
        Expect(output.find("hello") != std::string::npos,
               "echo output should contain hello, got: " + output);
    }

    {
        std::string output;
#ifdef _WIN32
        const int code = RunSimple("exit /b 7", "", {}, &output);
#else
        const int code = RunSimple("exit 7", "", {}, &output);
#endif
        Expect(code == 7, "exit code should be 7, got " + std::to_string(code));
    }

    {
        std::string output;
        const int code = RunSimple("echo 123456", "", {{"AIAGENT_PROC_TEST", "ok"}}, &output);
        Expect(code == 0, "env command exit code should be 0");
        (void)output;
#ifdef _WIN32
        const int env_code = RunSimple("echo %AIAGENT_PROC_TEST%", "",
                                       {{"AIAGENT_PROC_TEST", "ok"}}, &output);
#else
        const int env_code = RunSimple("printf '%s' \"$AIAGENT_PROC_TEST\"", "",
                                       {{"AIAGENT_PROC_TEST", "ok"}}, &output);
#endif
        Expect(env_code == 0, "env read exit code should be 0");
        Expect(output.find("ok") != std::string::npos,
               "child env should contain injected value, got: " + output);
    }

    {
        std::string output;
#ifdef _WIN32
        const int cwd_code =
                RunSimple("cd", aiagent::platform::ToUtf8(temp_root), {}, &output);
#else
        const int cwd_code =
                RunSimple("pwd", aiagent::platform::ToUtf8(temp_root), {}, &output);
#endif
        Expect(cwd_code == 0, "cwd command should exit 0");
        Expect(output.find(temp_root.filename().string()) != std::string::npos,
               "child should run in requested cwd, got: " + output);
    }

    {
        aiagent::platform::ProcessOptions options;
#ifdef _WIN32
        options.argv = ShellArgv("ping -n 10 127.0.0.1 >NUL");
#else
        options.argv = ShellArgv("sleep 5");
#endif
        options.stderr_mode = aiagent::platform::StderrMode::kDiscard;
        std::string error;
        auto process = aiagent::platform::ChildProcess::Start(options, &error);
        Expect(process != nullptr, "timeout process should start: " + error);
        if (process) {
            char buffer[256];
            std::size_t count = 0;
            const auto status = process->Read(buffer, sizeof(buffer), 300, &count,
                                              &error);
            Expect(status == aiagent::platform::ReadStatus::kTimeout,
                   "read should time out");
            process->Terminate();
            int exit_code = -1;
            Expect(process->WaitForExit(5000, &exit_code, &error),
                   "terminated process should exit");
        }
    }

    {
        aiagent::platform::ProcessOptions options;
#ifdef _WIN32
        options.argv = ShellArgv("findstr .");
#else
        options.argv = ShellArgv("cat");
#endif
        options.stderr_mode = aiagent::platform::StderrMode::kMergeToStdout;
        std::string error;
        auto process = aiagent::platform::ChildProcess::Start(options, &error);
        Expect(process != nullptr, "stdin process should start: " + error);
        if (process) {
            Expect(process->Write("ping\n", &error), "stdin write should succeed: " + error);
            process->CloseStdin();
            bool timed_out = false;
            const std::string output = ReadAll(*process, 5000, &timed_out);
            Expect(!timed_out, "stdin process should reach EOF");
            Expect(output.find("ping") != std::string::npos,
                   "stdin should be echoed, got: " + output);
            int exit_code = -1;
            Expect(process->WaitForExit(5000, &exit_code, &error),
                   "stdin process should exit");
        }
    }

    std::filesystem::remove_all(temp_root, ec);

    if (g_failures != 0) {
        std::cerr << "process test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "process test passed\n";
    return 0;
}
