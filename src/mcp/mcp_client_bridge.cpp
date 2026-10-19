#include "aiagent/mcp/mcp_client_bridge.hpp"

#include "aiagent/log.hpp"
#include "aiagent/platform/paths.hpp"
#include "aiagent/platform/process.hpp"
#include "aiagent/version.hpp"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace aiagent::mcp {
namespace {

constexpr const char* kMcpProtocolVersion = "2025-03-26";

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
#ifndef _WIN32
        static const bool ignore_sigpipe = [] {
            std::signal(SIGPIPE, SIG_IGN);
            return true;
        }();
        (void)ignore_sigpipe;
#endif

        std::string resolve_error;
        const std::string binary = platform::ResolveExecutable(config.binary,
                                                               &resolve_error);
        if (binary.empty()) {
            throw std::runtime_error("MCP server binary not found: " + resolve_error);
        }

        config_ = config;
        // 复用实例重新启动：清空上一次会话的读缓冲与工具缓存。
        read_buffer_.clear();
        tools_cached_ = false;
        tool_cache_.clear();

        platform::ProcessOptions options;
        options.argv = {binary, "-p", config_.plugins_dir};
        if (!config_.log_dir.empty()) {
            options.argv.push_back("-l");
            options.argv.push_back(config_.log_dir);
        }
        options.argv.push_back("--python-path");
        options.argv.push_back(config_.python_path);
        options.stderr_mode = platform::StderrMode::kDiscard;

        // Python 插件包目录加入 PYTHONPATH；分隔符按平台。
        if (!config_.plugins_dir.empty()) {
            const std::filesystem::path python_dir =
                    platform::FromUtf8(config_.plugins_dir) / "python";
            std::error_code ec;
            if (std::filesystem::exists(python_dir, ec) && !ec) {
                const std::string python_lib_dir = platform::ToUtf8(python_dir);
                const std::optional<std::string> old_path =
                        platform::GetEnvUtf8("PYTHONPATH");
                std::string combined = old_path.value_or("");
                if (!combined.empty()) {
                    combined += platform::PathListSeparator();
                }
                combined += python_lib_dir;
                options.env.emplace_back("PYTHONPATH", combined);
            }
        }

        std::string start_error;
        process_ = platform::ChildProcess::Start(options, &start_error);
        if (!process_) {
            throw std::runtime_error("failed to start MCP server: " + start_error);
        }
        running_ = true;
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kInfo, "mcp",
                "event=server_start pid=" + std::to_string(process_->Pid()) +
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
        if (process_) {
            process_->CloseStdin();
            int exit_code = -1;
            std::string wait_error;
            if (!process_->WaitForExit(1000, &exit_code, &wait_error)) {
                process_->Terminate();
                (void)process_->WaitForExit(-1, &exit_code, &wait_error);
            }
            process_.reset();
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

    bool IsRunning() const {
        return running_ && process_ != nullptr && process_->IsRunning();
    }

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
    // 大块 read 进 read_buffer_ 后按行切分，避免每字节一次 poll+read。
    std::string ReadLine() {
        const auto deadline = std::chrono::steady_clock::now() +
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
            int timeout_ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
                            .count());
            if (timeout_ms < 1) {
                timeout_ms = 1;
            }

            char buffer[64 * 1024];
            std::size_t count = 0;
            std::string read_error;
            const platform::ReadStatus status =
                    process_->Read(buffer, sizeof(buffer), timeout_ms, &count,
                                   &read_error);
            if (status == platform::ReadStatus::kTimeout) {
                throw std::runtime_error("timeout waiting for MCP server response");
            }
            if (status == platform::ReadStatus::kError) {
                throw std::runtime_error("failed to read MCP server response: " +
                                         read_error);
            }
            if (status == platform::ReadStatus::kEof) {
                throw std::runtime_error("MCP server closed stdout");
            }
            read_buffer_.append(buffer, count);
        }
    }

    void WriteLine(const std::string& data) {
        std::string write_error;
        if (!process_->Write(data, &write_error)) {
            throw std::runtime_error("failed to write to MCP server: " +
                                     write_error);
        }
    }

    nlohmann::json Call(const std::string& method, const nlohmann::json& params) {
        if (!running_ || !process_) {
            throw std::runtime_error("MCP server is not running");
        }
        // 子进程是单 fd 的请求/响应协议：并发调用会串响应，必须串行化。
        std::lock_guard<std::mutex> lock(call_mutex_);

        const std::string request_id = std::to_string(next_id_++);
        const nlohmann::json request = {{"jsonrpc", "2.0"},
                                        {"id", request_id},
                                        {"method", method},
                                        {"params", params}};
        WriteLine(request.dump() + "\n");

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
        WriteLine(request.dump() + "\n");
    }

    MCPConfig config_;
    std::unique_ptr<platform::ChildProcess> process_;
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
