// 真实 MCP 服务端集成冒烟：启动 mcp_server -> tools/list -> tools/call -> Stop。
// 通过环境变量启用，未设置时跳过（避免默认 ctest 依赖外部构建产物）：
//   AIAGENT_MCP_TEST_SERVER  mcp_server(.exe) 路径
//   AIAGENT_MCP_TEST_PLUGINS 插件目录（含 native/ 与 python/）
//   AIAGENT_MCP_TEST_PYTHON  Python 解释器（可选，默认 python）
#include <cstdlib>
#include <iostream>
#include <string>

#include "aiagent/config.hpp"
#include "aiagent/mcp/mcp_client_bridge.hpp"

int main() {
    const char* server = std::getenv("AIAGENT_MCP_TEST_SERVER");
    const char* plugins = std::getenv("AIAGENT_MCP_TEST_PLUGINS");
    if (server == nullptr || *server == '\0' || plugins == nullptr ||
        *plugins == '\0') {
        std::cout << "mcp real bridge test skipped (AIAGENT_MCP_TEST_SERVER / "
                     "AIAGENT_MCP_TEST_PLUGINS not set)\n";
        return 0;
    }

    aiagent::MCPConfig config;
    config.enabled = true;
    config.binary = server;
    config.plugins_dir = plugins;
    if (const char* python = std::getenv("AIAGENT_MCP_TEST_PYTHON");
        python != nullptr && *python != '\0') {
        config.python_path = python;
    }

    int failures = 0;
    auto expect = [&failures](bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    };

    aiagent::mcp::MCPClientBridge bridge;
    try {
        expect(bridge.Start(config), "bridge should start MCP server");
        const auto tools = bridge.ListTools();
        bool has_shell = false;
        bool has_python = false;
        for (const auto& tool : tools) {
            if (tool.name == "shell_exec") {
                has_shell = true;
            }
            if (tool.name == "calculate") {
                has_python = true;
            }
        }
        expect(has_shell, "tools/list should include native shell_exec");
        expect(has_python, "tools/list should include Python calculate");

        const auto result = bridge.Execute(
                "shell_exec", {{"command", "echo mcp-bridge-ok"}}, {});
        expect(result.ok, "shell_exec should succeed: " + result.error);
        expect(result.text.find("mcp-bridge-ok") != std::string::npos,
               "shell_exec output should contain marker: " + result.text);
        bridge.Stop();
        expect(!bridge.IsRunning(), "bridge should be stopped");
    } catch (const std::exception& e) {
        std::cerr << "FAILED: exception: " << e.what() << '\n';
        ++failures;
        bridge.Stop();
    }

    if (failures != 0) {
        std::cerr << "mcp real bridge test failed with " << failures
                  << " failure(s)\n";
        return 1;
    }
    std::cout << "mcp real bridge test passed\n";
    return 0;
}
