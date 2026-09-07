#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "a2a/models/message.hpp"
#include "a2a/models/task.hpp"

#include "aiagent/agent/agent_runtime.hpp"
#include "aiagent/config.hpp"
#include "aiagent/llm/deepseek_client.hpp"
#include "aiagent/mcp/mcp_client_bridge.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

class ToolCallingModel final : public aiagent::llm::IModelClient {
public:
    int call_count = 0;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>& messages,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        ++call_count;

        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        if (call_count == 1) {
            choice.content = "";
            choice.tool_calls.push_back(
                    aiagent::llm::ToolCall{"call_1", "calculate", R"({"expr":"1+1"})"});
            choice.finish_reason = "tool_calls";
        } else {
            choice.content = "计算结果是 4";
            choice.finish_reason = "stop";
        }

        aiagent::llm::ChatResult result;
        result.choices.push_back(std::move(choice));
        (void)messages;
        return result;
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>&) override {}
};

std::string FirstText(const a2a::models::Message& message) {
    for (const auto& part : message.parts) {
        if (part.text.has_value()) {
            return *part.text;
        }
    }
    return {};
}

aiagent::MCPConfig FakeConfig() {
    aiagent::MCPConfig config;
    config.enabled = true;
    config.binary = AIAGENT_FAKE_MCP_BINARY;
    config.plugins_dir = "";
    config.request_timeout_seconds = 5;
    return config;
}

}  // namespace

int main() {
    try {
        auto bridge = std::make_shared<aiagent::mcp::MCPClientBridge>();
        if (!bridge->Start(FakeConfig())) {
            std::cerr << "fake MCP server failed to start\n";
            return 1;
        }

        const auto tools = bridge->ListTools();
        Expect(tools.size() == 1, "tools/list should return one tool");
        Expect(!tools.empty() && tools.front().name == "calculate",
               "tool name should be calculate");

        const auto execution = bridge->Execute("calculate", {{"expr", "1+1"}});
        Expect(execution.ok, "tools/call should succeed");
        Expect(execution.text == "4", "tools/call should return text 4");

        auto model = std::make_shared<ToolCallingModel>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetToolProvider(bridge);

        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "1+1 等于几？", "text/plain");
        const auto response = runtime.HandleMessage(user, {user}, "ctx-tool");
        Expect(response.IsTask(), "tool-loop response should contain a task");
        Expect(model->call_count == 2, "model should be called twice (tool round + final)");
        Expect(FirstText(response.GetTask().history.back()) == "计算结果是 4",
               "final assistant reply should mention the tool result");

        bridge->Stop();
    } catch (const std::exception& e) {
        std::cerr << "mcp bridge test failed: " << e.what() << '\n';
        return 1;
    }

    if (g_failures != 0) {
        std::cerr << "mcp bridge test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "mcp bridge test passed\n";
    return 0;
}
