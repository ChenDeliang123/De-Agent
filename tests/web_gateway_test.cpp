#include <a2a/core/http_client.hpp>
#include <a2a/core/version.hpp>
#include <a2a/models/agent_card.hpp>
#include <a2a/models/message.hpp>
#include <a2a/models/requests.hpp>
#include <a2a/server/agent_server.hpp>
#include <a2a/server/memory_task_store.hpp>

#include <json.hpp>

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "aiagent/agent/agent_runtime.hpp"
#include "aiagent/llm/deepseek_client.hpp"
#include "aiagent/web/web_gateway.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

class FakeModel final : public aiagent::llm::IModelClient {
public:
    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>& messages,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        std::string last_user;
        for (const auto& message : messages) {
            if (message.role == "user") {
                last_user = message.content;
            }
        }
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "web-reply-to-" + last_user;
        choice.finish_reason = "stop";
        aiagent::llm::ChatResult result;
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>&) override {}
};

a2a::models::AgentCard BuildCard() {
    a2a::models::AgentCard card("web-test-agent", "Web gateway test", "0.1.0");
    card.WithDefaultInputModes({"text/plain"}).WithDefaultOutputModes({"text/plain"});
    return card;
}

}  // namespace

int main() {
    try {
        auto model = std::make_shared<FakeModel>();
        aiagent::agent::AgentRuntime runtime(model);
        auto task_store = std::make_shared<a2a::server::MemoryTaskStore>();
        a2a::server::AgentServer agent_server(BuildCard(), task_store);

        agent_server.task_manager().SetMessageHandler(
                [&](const a2a::models::SendMessageRequest& request) {
                    std::vector<a2a::models::Message> history;
                    std::size_t latest_size = 0;
                    for (const auto& task : task_store->ListAll()) {
                        if (task.context_id == request.message.context_id &&
                            task.history.size() > latest_size) {
                            history = task.history;
                            latest_size = task.history.size();
                        }
                    }
                    history.push_back(request.message);
                    return runtime.HandleMessage(request.message, history,
                                                 request.message.context_id);
                });

        aiagent::web::WebGateway gateway(agent_server, *task_store);
        gateway.SetStreamHandler(
                [](const std::string&,
                   const std::string&,
                   const aiagent::web::WebGateway::TokenCallback& on_token) {
                    on_token("你");
                    on_token("好");
                });
        if (!gateway.Start("127.0.0.1", 0)) {
            std::cerr << "web gateway failed to start\n";
            return 1;
        }

        const std::string base =
                "http://127.0.0.1:" + std::to_string(gateway.bound_port());
        a2a::core::HttpClient http;

        const auto health = http.Get(base + "/health");
        Expect(health.IsSuccess(), "health endpoint should succeed");

        const auto chat = http.Post(base + "/api/v1/chat",
                                    R"({"session_id":null,"message":"hello"})",
                                    "application/json");
        Expect(chat.IsSuccess(), "chat endpoint should succeed");
        const auto chat_json = nlohmann::json::parse(chat.body);
        const std::string session_id = chat_json.value("session_id", std::string());
        Expect(!session_id.empty(), "chat response should contain a session id");
        Expect(chat_json.value("content", std::string()) == "web-reply-to-hello",
               "chat response should contain the model reply");

        const auto sessions = http.Get(base + "/api/v1/sessions");
        Expect(sessions.IsSuccess(), "sessions endpoint should succeed");
        const auto session_list = nlohmann::json::parse(sessions.body);
        Expect(session_list.size() == 1, "sessions should contain one session");

        const auto messages = http.Get(base + "/api/v1/sessions/" + session_id + "/messages");
        Expect(messages.IsSuccess(), "messages endpoint should succeed");
        const auto message_list = nlohmann::json::parse(messages.body);
        Expect(message_list.size() == 2, "messages should contain user and assistant messages");

        std::string stream_body;
        const auto stream = http.PostStream(
                base + "/api/v1/chat/stream",
                R"({"session_id":null,"message":"stream-hello"})",
                "application/json",
                [&stream_body](std::string_view chunk) {
                    stream_body.append(chunk.data(), chunk.size());
                });
        Expect(stream.IsSuccess(), "stream endpoint should succeed");
        Expect(stream_body.find("event: done") != std::string::npos,
               "stream body should contain done event");
        Expect(stream_body.find(R"({"content":"你"})") != std::string::npos,
               "stream body should contain the first token");
        Expect(stream_body.find(R"({"content":"好"})") != std::string::npos,
               "stream body should contain the second token");

        gateway.Stop();
    } catch (const std::exception& e) {
        std::cerr << "web gateway test failed: " << e.what() << '\n';
        return 1;
    }

    if (g_failures != 0) {
        std::cerr << "web gateway test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "web gateway test passed\n";
    return 0;
}
