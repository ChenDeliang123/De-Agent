#include <json.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

#include "aiagent/config.hpp"
#include "aiagent/llm/deepseek_types.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

void TestConfigParsing() {
    const std::string ini = R"INI(
[deepseek]
api_key = sk-test-key
base_url = https://api.deepseek.com/
model = deepseek-v4-flash
reasoning_model = deepseek-v4-pro
timeout_seconds = 45
max_retries = 5
)INI";

    std::string error;
    const auto config = aiagent::ParseDeepSeekConfig(ini, &error);
    Expect(config.has_value(), "config should parse successfully");
    if (config.has_value()) {
        Expect(config->api_key == "sk-test-key", "api_key should be parsed");
        Expect(config->base_url == "https://api.deepseek.com/", "base_url should be parsed");
        Expect(config->model == "deepseek-v4-flash", "model should be parsed");
        Expect(config->reasoning_model == "deepseek-v4-pro", "reasoning_model should be parsed");
        Expect(config->timeout_seconds == 45, "timeout_seconds should be parsed");
        Expect(config->max_retries == 5, "max_retries should be parsed");
        Expect(aiagent::DeepSeekChatEndpoint(*config) ==
                       "https://api.deepseek.com/chat/completions",
               "endpoint should be derived from base_url");
    }

    const auto missing_key = aiagent::ParseDeepSeekConfig("[deepseek]\nmodel = x\n", &error);
    Expect(!missing_key.has_value(), "config without api_key should fail");
}

void TestRequestBody() {
    using namespace aiagent::llm;

    ChatMessage system;
    system.role = "system";
    system.content = "You are helpful.";

    ChatMessage user;
    user.role = "user";
    user.content = "北京天气怎么样？";

    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.reasoning_content = "需要查询天气";
    assistant.content = "";
    assistant.tool_calls.push_back(
            ToolCall{"call_1", "get_weather", R"({"city":"北京"})", 0, false});

    ChatMessage tool_result;
    tool_result.role = "tool";
    tool_result.tool_call_id = "call_1";
    tool_result.content = "晴，25 度";

    ToolSpec tool;
    tool.name = "get_weather";
    tool.description = "查询城市天气";
    tool.parameters = {{"type", "object"},
                       {"properties", {{"city", {{"type", "string"}}}}}};

    const std::string body = BuildChatRequestBody(
            "deepseek-v4-flash",
            {system, user, assistant, tool_result},
            {tool},
            /*stream=*/false,
            "auto");

    const auto root = nlohmann::json::parse(body);
    Expect(root["model"] == "deepseek-v4-flash", "request model should be set");
    Expect(root["stream"] == false, "request stream should be false");
    Expect(root["messages"].size() == 4, "request should contain all messages");
    Expect(root["messages"][2]["tool_calls"][0]["function"]["name"] == "get_weather",
           "assistant tool_calls should be serialized");
    Expect(root["messages"][2]["reasoning_content"] == "需要查询天气",
           "reasoning_content should be serialized");
    Expect(root["messages"][3]["tool_call_id"] == "call_1",
           "tool message should keep tool_call_id");
    Expect(root["tools"][0]["function"]["name"] == "get_weather",
           "tools should be serialized");
    Expect(root["tool_choice"] == "auto", "tool_choice should be serialized");
}

void TestResponseParsing() {
    using namespace aiagent::llm;

    const std::string body = R"JSON({
  "id": "chatcmpl-test",
  "object": "chat.completion",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "1 + 1 = 2",
        "reasoning_content": "先计算",
        "tool_calls": [
          {
            "id": "call_abc",
            "type": "function",
            "function": {
              "name": "calculate",
              "arguments": "{\"expr\":\"1+1\"}"
            }
          }
        ]
      },
      "finish_reason": "tool_calls"
    }
  ],
  "usage": {
    "prompt_tokens": 12,
    "completion_tokens": 8,
    "total_tokens": 20
  }
})JSON";

    const ChatResult result = ParseChatResponse(body);
    Expect(result.id == "chatcmpl-test", "chat result id should be parsed");
    Expect(result.choices.size() == 1, "chat result should contain one choice");
    if (!result.choices.empty()) {
        const auto& choice = result.choices.front();
        Expect(choice.content == "1 + 1 = 2", "assistant content should be parsed");
        Expect(choice.reasoning_content == "先计算", "reasoning_content should be parsed");
        Expect(choice.finish_reason == "tool_calls", "finish_reason should be parsed");
        Expect(choice.tool_calls.size() == 1, "tool_calls should be parsed");
        if (!choice.tool_calls.empty()) {
            Expect(choice.tool_calls.front().id == "call_abc", "tool call id should be parsed");
            Expect(choice.tool_calls.front().name == "calculate",
                   "tool call name should be parsed");
            Expect(choice.tool_calls.front().arguments == R"({"expr":"1+1"})",
                   "tool call arguments should be kept raw");
        }
    }
    Expect(result.usage.prompt_tokens == 12, "prompt tokens should be parsed");
    Expect(result.usage.completion_tokens == 8, "completion tokens should be parsed");
    Expect(result.usage.total_tokens == 20, "total tokens should be parsed");

    const std::string stream_frame = R"JSON({
  "id": "chatcmpl-stream",
  "choices": [
    {
      "index": 0,
      "delta": {
        "role": "assistant",
        "tool_calls": [
          {
            "index": 0,
            "id": "call_weather",
            "type": "function",
            "function": {
              "name": "get_weather",
              "arguments": "{\"city\":\""
            }
          }
        ]
      },
      "finish_reason": null
    }
  ]
})JSON";
    const StreamChunk chunk = ParseStreamChunk(stream_frame);
    Expect(chunk.role == "assistant", "stream role should be parsed");
    Expect(chunk.tool_calls.size() == 1, "stream tool_calls should be parsed");
    if (!chunk.tool_calls.empty()) {
        Expect(chunk.tool_calls.front().index == 0, "stream tool index should be parsed");
        Expect(chunk.tool_calls.front().has_index, "stream tool index presence should be parsed");
        Expect(chunk.tool_calls.front().name == "get_weather",
               "stream tool name should be parsed");
        Expect(chunk.tool_calls.front().arguments == R"({"city":")",
               "partial stream arguments should be preserved");
    }

    Expect(DeepSeekErrorMessage(R"({"error":{"message":"bad request"}})") == "bad request",
           "error message should be extracted");
}

}  // namespace

int main() {
    TestConfigParsing();
    TestRequestBody();
    TestResponseParsing();

    if (g_failures != 0) {
        std::cerr << "llm client test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "llm client test passed\n";
    return 0;
}
