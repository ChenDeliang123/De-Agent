#include <json.hpp>

#include <httplib.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "aiagent/config.hpp"
#include "aiagent/llm/deepseek_client.hpp"
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

    // 流式请求应开启 include_usage，并在非流式请求中不出现该字段。
    const std::string stream_body = BuildChatRequestBody(
            "deepseek-v4-flash", {system, user}, {}, /*stream=*/true, std::nullopt);
    const auto stream_root = nlohmann::json::parse(stream_body);
    Expect(stream_root["stream"] == true, "stream request should set stream=true");
    Expect(stream_root["stream_options"]["include_usage"] == true,
           "stream request should enable include_usage");
    Expect(!root.contains("stream_options"),
           "non-stream request should not send stream_options");
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

    // include_usage 的 usage 帧：choices 为空数组，usage 在顶层。
    const std::string usage_frame = R"JSON({
  "id": "chatcmpl-usage",
  "choices": [],
  "usage": {
    "prompt_tokens": 120,
    "completion_tokens": 30,
    "total_tokens": 150
  }
})JSON";
    const StreamChunk usage_chunk = ParseStreamChunk(usage_frame);
    Expect(usage_chunk.usage.prompt_tokens == 120,
           "stream usage prompt_tokens should be parsed");
    Expect(usage_chunk.usage.completion_tokens == 30,
           "stream usage completion_tokens should be parsed");
    Expect(usage_chunk.usage.total_tokens == 150,
           "stream usage total_tokens should be parsed");
}

// 起一个本地 HTTP server，POST /chat/completions 返回 status（handler 决定），
// 记录命中次数。返回 (server, hits) 的简单封装交给调用方 stop/join。
struct LocalServer {
    httplib::Server server;
    std::atomic<int> hits{0};
    int port = 0;
    std::thread listener;

    bool Start() {
        port = server.bind_to_any_port("127.0.0.1", 0);
        if (port <= 0) {
            return false;
        }
        listener = std::thread([this] { server.listen_after_bind(); });
        return true;
    }

    void Stop() {
        server.stop();
        if (listener.joinable()) {
            listener.join();
        }
    }

    std::string BaseUrl() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

const char* kOkBody =
        R"({"id":"chatcmpl-ok","choices":[{"index":0,"message":{"role":"assistant","content":"ok"},"finish_reason":"stop"}]})";
const char* kErrorBody = R"({"error":{"message":"boom"}})";

aiagent::DeepSeekConfig TestConfig(const std::string& base_url) {
    aiagent::DeepSeekConfig config;
    config.api_key = "sk-test";
    config.base_url = base_url;
    config.model = "test-model";
    config.max_retries = 3;
    config.timeout_seconds = 5;
    return config;
}

// 5xx 前两次失败、第三次成功：应重试并最终成功，server 命中 3 次。
void TestRetryOnTransientServerErrors() {
    LocalServer local;
    local.server.Post("/chat/completions",
                      [&](const httplib::Request&, httplib::Response& response) {
                          if (local.hits.fetch_add(1) < 2) {
                              response.status = 500;
                              response.set_content(kErrorBody, "application/json");
                              return;
                          }
                          response.status = 200;
                          response.set_content(kOkBody, "application/json");
                      });
    if (!local.Start()) {
        Expect(false, "retry server should bind");
        return;
    }

    try {
        aiagent::llm::DeepSeekClient client(TestConfig(local.BaseUrl()));
        const auto result = client.Chat({});
        Expect(result.choices.size() == 1, "retry should eventually succeed");
        Expect(local.hits.load() == 3,
               "server should be hit 3 times (2 failures + 1 success)");
    } catch (const std::exception& e) {
        Expect(false, std::string("retry chat should not throw: ") + e.what());
    }
    local.Stop();
}

// 4xx（客户端错误）不应重试：只命中一次并抛出 401。
void TestNoRetryOnClientError() {
    LocalServer local;
    local.server.Post("/chat/completions",
                      [&](const httplib::Request&, httplib::Response& response) {
                          local.hits.fetch_add(1);
                          response.status = 401;
                          response.set_content(kErrorBody, "application/json");
                      });
    if (!local.Start()) {
        Expect(false, "401 server should bind");
        return;
    }

    try {
        aiagent::llm::DeepSeekClient client(TestConfig(local.BaseUrl()));
        (void)client.Chat({});
        Expect(false, "401 chat should throw");
    } catch (const aiagent::llm::DeepSeekException& e) {
        Expect(e.status_code() == 401, "401 should surface its status code");
        Expect(local.hits.load() == 1, "401 should not be retried");
    } catch (const std::exception& e) {
        Expect(false, std::string("unexpected exception: ") + e.what());
    }
    local.Stop();
}

// max_retries = 0：即使 5xx 也不重试，命中一次后抛出。
void TestNoRetryWhenDisabled() {
    LocalServer local;
    local.server.Post("/chat/completions",
                      [&](const httplib::Request&, httplib::Response& response) {
                          local.hits.fetch_add(1);
                          response.status = 500;
                          response.set_content(kErrorBody, "application/json");
                      });
    if (!local.Start()) {
        Expect(false, "500 server should bind");
        return;
    }

    try {
        auto config = TestConfig(local.BaseUrl());
        config.max_retries = 0;
        aiagent::llm::DeepSeekClient client(config);
        (void)client.Chat({});
        Expect(false, "500 chat with max_retries=0 should throw");
    } catch (const aiagent::llm::DeepSeekException& e) {
        Expect(e.status_code() == 500, "500 should surface its status code");
        Expect(local.hits.load() == 1, "max_retries=0 should disable retry");
    } catch (const std::exception& e) {
        Expect(false, std::string("unexpected exception: ") + e.what());
    }
    local.Stop();
}

// 流式响应中的 include_usage 帧应通过 StreamEvent.usage 向上层传递。
void TestStreamUsageEvent() {
    LocalServer local;
    local.server.Post("/chat/completions",
                      [&](const httplib::Request&, httplib::Response& response) {
                          local.hits.fetch_add(1);
                          response.set_chunked_content_provider(
                                  "text/event-stream",
                                  [](std::size_t offset, httplib::DataSink& sink) {
                                      if (offset != 0) {
                                          sink.done();
                                          return false;
                                      }
                                      const std::string frames =
                                              "data: {\"id\":\"c\",\"choices\":[{\"index\":0,"
                                              "\"delta\":{\"content\":\"hi\"},"
                                              "\"finish_reason\":null}]}\n\n"
                                              "data: {\"id\":\"c\",\"choices\":[],"
                                              "\"usage\":{\"prompt_tokens\":11,"
                                              "\"completion_tokens\":2,\"total_tokens\":13}}\n\n"
                                              "data: [DONE]\n\n";
                                      sink.write(frames.data(), frames.size());
                                      sink.done();
                                      return true;
                                  });
                      });
    if (!local.Start()) {
        Expect(false, "stream usage server should bind");
        return;
    }

    try {
        aiagent::llm::DeepSeekClient client(TestConfig(local.BaseUrl()));
        std::string text;
        aiagent::llm::ChatUsage seen;
        client.ChatStream({}, {}, [&](const aiagent::llm::StreamEvent& event) {
            text += event.content;
            if (event.usage.total_tokens > 0) {
                seen = event.usage;
            }
        });
        Expect(text == "hi", "stream content should still be delivered");
        Expect(seen.total_tokens == 13, "stream usage total_tokens should be surfaced");
        Expect(seen.prompt_tokens == 11, "stream usage prompt_tokens should be surfaced");
    } catch (const std::exception& e) {
        Expect(false, std::string("stream usage chat should not throw: ") + e.what());
    }
    local.Stop();
}

}  // namespace

int main() {
    TestConfigParsing();
    TestRequestBody();
    TestResponseParsing();
    TestRetryOnTransientServerErrors();
    TestNoRetryOnClientError();
    TestNoRetryWhenDisabled();
    TestStreamUsageEvent();

    if (g_failures != 0) {
        std::cerr << "llm client test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "llm client test passed\n";
    return 0;
}
