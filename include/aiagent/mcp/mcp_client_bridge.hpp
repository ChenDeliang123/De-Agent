#pragma once

#include <memory>
#include <string>

#include "aiagent/config.hpp"
#include "aiagent/tools/tool_provider.hpp"

namespace aiagent::mcp {

// MCP Extension Platform 的 stdio 客户端桥：
// 负责启动 MCP Server 子进程、MCP 握手、tools/list 与 tools/call。
class MCPClientBridge : public tools::IToolProvider {
public:
    MCPClientBridge();
    ~MCPClientBridge() override;

    MCPClientBridge(const MCPClientBridge&) = delete;
    MCPClientBridge& operator=(const MCPClientBridge&) = delete;

    bool Start(const MCPConfig& config);
    void Stop();
    bool IsRunning() const;

    std::vector<llm::ToolSpec> ListTools() override;
    tools::ToolExecutionResult Execute(const std::string& name,
                                       const nlohmann::json& arguments,
                                       const std::string& working_directory = {}) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aiagent::mcp
