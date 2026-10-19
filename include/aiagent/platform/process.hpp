#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aiagent::platform {

enum class StderrMode {
    kInherit,       // 继承父进程 stderr
    kMergeToStdout, // 合并到 stdout 管道
    kDiscard,       // 丢弃
};

struct ProcessOptions {
    // argv[0] 是可执行文件；Windows 下支持自动补 .exe/PATHEXT。
    std::vector<std::string> argv;
    // UTF-8；空表示继承父进程工作目录。
    std::string cwd;
    // 追加/覆盖子进程环境变量。
    std::vector<std::pair<std::string, std::string>> env;
    StderrMode stderr_mode = StderrMode::kInherit;
    // Windows 专用：非空时直接作为 CreateProcessW 的命令行（UTF-8），
    // 不再按 argv 逐项转义。用于 cmd.exe /c 这类命令本身带引号的场景。
    std::string raw_command_line;
};

enum class ReadStatus {
    kData,
    kEof,
    kTimeout,
    kError,
};

// 一个带 stdin/stdout 管道的子进程。stdout/stderr 的消费方式由
// ProcessOptions::stderr_mode 决定。
class ChildProcess {
public:
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // 启动失败返回 nullptr，error 写具体原因。
    static std::unique_ptr<ChildProcess> Start(const ProcessOptions& options,
                                               std::string* error);

    bool Write(std::string_view data, std::string* error);
    bool CloseStdin();

    // timeout_ms < 0 表示一直等到有数据/EOF。
    ReadStatus Read(char* buffer, std::size_t size, int timeout_ms,
                    std::size_t* bytes_read, std::string* error);

    // timeout_ms < 0 表示一直等。返回 true 表示已退出（exit_code 有效）；
    // false 表示超时。
    bool WaitForExit(int timeout_ms, int* exit_code, std::string* error);

    // 尽力终止整棵进程树；之后仍需 WaitForExit 回收。
    void Terminate();

    bool IsRunning() const;
    long Pid() const;

private:
    ChildProcess();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aiagent::platform
