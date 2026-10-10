#include "aiagent/mcp/mcp_client_bridge.hpp"
#include "aiagent/log.hpp"
#include "aiagent/version.hpp"

#include <json.hpp>

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace aiagent::mcp {
namespace {

constexpr const char* kMcpProtocolVersion = "2025-03-26";

void WriteAll(int fd, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = write(fd, data.data() + offset, data.size() - offset);
        if (written <= 0) {
            if (written < 0 && errno == EINTR) {
                continue;
            }
            throw std::runtime_error("failed to write to MCP server: " +
                                     std::string(std::strerror(errno)));
        }
        offset += static_cast<std::size_t>(written);
    }
}

std::string ContentText(const nlohmann::json& result) {
    std::string text;
    if (result.contains("content") && result["content"].is_array()) {
        for (const auto& item : result["content"]) {
            if (item.contains("type") && item["type"] == "text" &&
                item.contains("text") && item["text"].is_string()) {
                if (!text.empty()) {
                    text.push_back('\n');
                }
                text += item["text"].get<std::string>();
            }
        }
    }
    return text;
}

}  // namespace

class MCPClientBridge::Impl {
public:
    ~Impl() { Stop(); }

    bool Start(const MCPConfig& config) {
        if (running_) {
            return false;
        }
        static const bool ignore_sigpipe = [] {
            std::signal(SIGPIPE, SIG_IGN);
            return true;
        }();
        (void)ignore_sigpipe;
        if (config.binary.empty() || access(config.binary.c_str(), X_OK) != 0) {
            throw std::runtime_error("MCP server binary not found or not executable: " +
                                     config.binary);
        }

        config_ = config;
        // 复用实例重新启动：清空上一次会话的读缓冲与工具缓存。
        read_buffer_.clear();
        tools_cached_ = false;
        tool_cache_.clear();

        int child_stdin[2];
        int child_stdout[2];
        if (pipe(child_stdin) != 0 || pipe(child_stdout) != 0) {
            throw std::runtime_error("failed to create MCP server pipes");
        }

        pid_ = fork();
        if (pid_ < 0) {
            close(child_stdin[0]);
            close(child_stdin[1]);
            close(child_stdout[0]);
            close(child_stdout[1]);
            throw std::runtime_error("failed to fork MCP server");
        }

        if (pid_ == 0) {
            // 子进程：独立进程组，便于清理 Python 子进程。
            setpgid(0, 0);
            dup2(child_stdin[0], STDIN_FILENO);
            dup2(child_stdout[1], STDOUT_FILENO);
            close(child_stdin[0]);
            close(child_stdin[1]);
            close(child_stdout[0]);
            close(child_stdout[1]);

            const std::string python_lib_dir = config_.plugins_dir + "/python";
            if (!config_.plugins_dir.empty() && access(python_lib_dir.c_str(), F_OK) == 0) {
                const char* old_path = std::getenv("PYTHONPATH");
                const std::string combined = old_path == nullptr || *old_path == '\0'
                                                     ? python_lib_dir
                                                     : std::string(old_path) + ":" + python_lib_dir;
                setenv("PYTHONPATH", combined.c_str(), 1);
            }

            std::vector<std::string> args{config_.binary, "-p", config_.plugins_dir};
            if (!config_.log_dir.empty()) {
                args.push_back("-l");
                args.push_back(config_.log_dir);
            }
            args.push_back("--python-path");
            args.push_back(config_.python_path);

            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (auto& arg : args) {
                argv.push_back(arg.data());
            }
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(127);
        }

        close(child_stdin[0]);
        close(child_stdout[1]);
        stdin_fd_ = child_stdin[1];
        stdout_fd_ = child_stdout[0];
        running_ = true;
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kInfo, "mcp",
                "event=server_start pid=" + std::to_string(pid_) +
                        " binary=" + config_.binary);

        try {
            const nlohmann::json params = {
                    {"protocolVersion", kMcpProtocolVersion},
                    {"capabilities", nlohmann::json::object()},
                    {"clientInfo", {{ "name", aiagent::kAgentName },
                                      { "version", aiagent::kVersion }}}};
            Call("initialize", params);
            SendNotification("notifications/initialized", nlohmann::json::object());
        } catch (...) {
            Stop();
            throw;
        }
        return true;
    }

    void Stop() {
        if (stdin_fd_ >= 0) {
            close(stdin_fd_);
            stdin_fd_ = -1;
        }
        if (stdout_fd_ >= 0) {
            close(stdout_fd_);
            stdout_fd_ = -1;
        }
        if (pid_ > 0) {
            kill(-pid_, SIGTERM);
            for (int i = 0; i < 20; ++i) {
                int status = 0;
                if (waitpid(pid_, &status, WNOHANG) == pid_) {
                    pid_ = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (pid_ > 0) {
                kill(-pid_, SIGKILL);
                waitpid(pid_, nullptr, 0);
                pid_ = -1;
            }
        }
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kInfo, "mcp", "event=server_stop");
        running_ = false;
        read_buffer_.clear();
        {
            std::lock_guard<std::mutex> lock(tool_cache_mutex_);
            tools_cached_ = false;
            tool_cache_.clear();
        }
    }

    bool IsRunning() const { return running_; }

    std::vector<llm::ToolSpec> ListTools() {
        {
            std::lock_guard<std::mutex> lock(tool_cache_mutex_);
            if (tools_cached_) {
                aiagent::DefaultLogger().Log(
                        aiagent::LogLevel::kDebug, "mcp",
                        "event=tools_list cache_hit=true count=" +
                                std::to_string(tool_cache_.size()));
                return tool_cache_;
            }
        }

        const nlohmann::json response = Call("tools/list", nlohmann::json::object());
        std::vector<llm::ToolSpec> tools;
        if (!response.contains("tools") || !response["tools"].is_array()) {
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kWarning, "mcp",
                    "event=tools_list cache_hit=false count=0");
            return tools;
        }
        for (const auto& item : response["tools"]) {
            llm::ToolSpec spec;
            spec.name = item.value("name", std::string());
            spec.description = item.value("description", std::string());
            if (item.contains("inputSchema")) {
                spec.parameters = item["inputSchema"];
            }
            if (!spec.name.empty()) {
                tools.push_back(std::move(spec));
            }
        }
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kInfo, "mcp",
                "event=tools_list cache_hit=false count=" +
                        std::to_string(tools.size()));
        {
            std::lock_guard<std::mutex> lock(tool_cache_mutex_);
            if (!tools_cached_) {
                tool_cache_ = std::move(tools);
                tools_cached_ = true;
            }
            return tool_cache_;
        }
    }

    tools::ToolExecutionResult Execute(const std::string& name,
                                       const nlohmann::json& arguments,
                                       const tools::ToolContext&) {
        const auto started = std::chrono::steady_clock::now();
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kDebug, "mcp",
                "event=tool_call_start name=" + name +
                        " args_bytes=" + std::to_string(arguments.dump().size()));
        const nlohmann::json response =
                Call("tools/call", {{"name", name}, {"arguments", arguments}});
        tools::ToolExecutionResult result;
        if (response.contains("isError") && response["isError"] == true) {
            result.ok = false;
            result.error = ContentText(response);
            LogToolCallEnd(name, started, false, result.error.size());
            return result;
        }
        result.ok = true;
        result.text = ContentText(response);
        if (result.text.empty()) {
            result.ok = false;
            result.error = "tool returned no text content";
            LogToolCallEnd(name, started, false, result.error.size());
        } else {
            LogToolCallEnd(name, started, true, result.text.size());
        }
        return result;
    }

private:
    void LogToolCallEnd(const std::string& name,
                        const std::chrono::steady_clock::time_point& started,
                        bool ok, std::size_t output_bytes) {
        const auto duration_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
        aiagent::DefaultLogger().Log(
                ok ? aiagent::LogLevel::kInfo : aiagent::LogLevel::kWarning,
                "mcp",
                "event=tool_call_end name=" + name + " ok=" +
                        (ok ? "true" : "false") +
                        " duration_ms=" + std::to_string(duration_ms) +
                        " output_bytes=" + std::to_string(output_bytes));
    }

    // 缓冲式读取 MCP stdout 的一行（\n 结尾，去除 \r）。
    // 大块 read 进 read_buffer_ 后按行切分，避免旧实现每字节一次 poll+read
    // 造成上万次系统调用（大工具结果时尤其明显）。超时仍以整行读取为粒度。
    std::string ReadLine() {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(config_.request_timeout_seconds);
        while (true) {
            const std::size_t newline = read_buffer_.find('\n');
            if (newline != std::string::npos) {
                std::string line = read_buffer_.substr(0, newline);
                read_buffer_.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return line;
            }

            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining.count() <= 0) {
                throw std::runtime_error("timeout waiting for MCP server response");
            }
            const auto remaining_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
            pollfd descriptor{stdout_fd_, POLLIN, 0};
            const int result = poll(&descriptor, 1, static_cast<int>(remaining_ms));
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("poll failed while reading MCP server");
            }
            if (result == 0) {
                throw std::runtime_error("timeout waiting for MCP server response");
            }

            char buffer[64 * 1024];
            const ssize_t count = read(stdout_fd_, buffer, sizeof(buffer));
            if (count == 0) {
                throw std::runtime_error("MCP server closed stdout");
            }
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("failed to read MCP server response");
            }
            read_buffer_.append(buffer, static_cast<std::size_t>(count));
        }
    }

    nlohmann::json Call(const std::string& method, const nlohmann::json& params) {
        if (!running_) {
            throw std::runtime_error("MCP server is not running");
        }
        // 子进程是单 fd 的请求/响应协议：并发调用会串响应，必须串行化。
        std::lock_guard<std::mutex> lock(call_mutex_);

        const std::string request_id = std::to_string(next_id_++);
        const nlohmann::json request = {{"jsonrpc", "2.0"},
                                        {"id", request_id},
                                        {"method", method},
                                        {"params", params}};
        WriteAll(stdin_fd_, request.dump() + "\n");

        while (true) {
            const std::string line = ReadLine();
            const nlohmann::json response = nlohmann::json::parse(line);
            const std::string response_id =
                    response.contains("id") && !response["id"].is_null()
                            ? response["id"].dump()
                            : std::string();
            if (response_id != "\"" + request_id + "\"" && response_id != request_id) {
                continue;  // 通知或其他请求的响应
            }
            if (response.contains("error")) {
                const std::string message =
                        response["error"].value("message", "unknown MCP error");
                throw std::runtime_error("MCP " + method + " failed: " + message);
            }
            return response.value("result", nlohmann::json::object());
        }
    }

    void SendNotification(const std::string& method, const nlohmann::json& params) {
        const nlohmann::json request = {{"jsonrpc", "2.0"},
                                        {"method", method},
                                        {"params", params}};
        WriteAll(stdin_fd_, request.dump() + "\n");
    }

    MCPConfig config_;
    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    bool running_ = false;
    int next_id_ = 1;
    std::string read_buffer_;
    std::mutex call_mutex_;
    mutable std::mutex tool_cache_mutex_;
    std::vector<llm::ToolSpec> tool_cache_;
    bool tools_cached_ = false;
};

MCPClientBridge::MCPClientBridge() : impl_(std::make_unique<Impl>()) {}
MCPClientBridge::~MCPClientBridge() = default;

bool MCPClientBridge::Start(const MCPConfig& config) { return impl_->Start(config); }
void MCPClientBridge::Stop() { impl_->Stop(); }
bool MCPClientBridge::IsRunning() const { return impl_->IsRunning(); }

std::vector<llm::ToolSpec> MCPClientBridge::ListTools() { return impl_->ListTools(); }
tools::ToolExecutionResult MCPClientBridge::Execute(const std::string& name,
                                                    const nlohmann::json& arguments,
                                                    const tools::ToolContext& context) {
    return impl_->Execute(name, arguments, context);
}

}  // namespace aiagent::mcp
