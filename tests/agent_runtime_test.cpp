#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "a2a/core/version.hpp"
#include "a2a/models/agent_card.hpp"
#include "a2a/models/message.hpp"
#include "a2a/models/requests.hpp"
#include "a2a/models/task.hpp"
#include "a2a/server/agent_server.hpp"

#include "aiagent/agent/agent_runtime.hpp"
#include "aiagent/llm/deepseek_client.hpp"

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
    std::string last_user_content;
    int call_count = 0;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>& messages,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        ++call_count;
        for (const auto& message : messages) {
            if (message.role == "user") {
                last_user_content = message.content;
            }
        }

        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "fake-reply-to-" + last_user_content;
        choice.finish_reason = "stop";

        aiagent::llm::ChatResult result;
        result.id = "fake-chat-id";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        aiagent::llm::StreamEvent event;
        event.content = "fake-stream";
        if (on_event) {
            on_event(event);
        }
    }
};

class StreamingFakeModel final : public aiagent::llm::IModelClient {
public:
    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "unused";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        aiagent::llm::StreamEvent first;
        first.content = "hel";
        on_event(first);
        aiagent::llm::StreamEvent second;
        second.content = "lo";
        on_event(second);
    }
};

class AlwaysToolModel final : public aiagent::llm::IModelClient {
public:
    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.tool_calls.push_back(
                aiagent::llm::ToolCall{"call-tool", "ok_tool", "{}"});
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        aiagent::llm::StreamEvent event;
        event.finish_reason = "tool_calls";
        event.tool_calls.push_back(
                aiagent::llm::ToolCall{"call-tool", "ok_tool", "{}"});
        if (on_event) {
            on_event(event);
        }
    }
};

class FiniteToolModel final : public aiagent::llm::IModelClient {
public:
    int calls = 0;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "finished-after-tools";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        ++calls;
        if (calls <= 5) {
            aiagent::llm::StreamEvent event;
            event.finish_reason = "tool_calls";
            event.tool_calls.push_back(
                    aiagent::llm::ToolCall{"call-tool", "ok_tool", "{}"});
            on_event(event);
            return;
        }
        aiagent::llm::StreamEvent event;
        event.content = "finished-after-tools";
        event.finish_reason = "stop";
        on_event(event);
    }
};

class OkToolProvider final : public aiagent::tools::IToolProvider {
public:
    std::vector<aiagent::llm::ToolSpec> ListTools() override {
        aiagent::llm::ToolSpec tool;
        tool.name = "ok_tool";
        return {tool};
    }

    aiagent::tools::ToolExecutionResult Execute(
            const std::string&,
            const nlohmann::json&,
            const std::string&) override {
        aiagent::tools::ToolExecutionResult result;
        result.ok = true;
        result.text = "ok";
        return result;
    }
};

a2a::models::AgentCard BuildCard() {
    using a2a::models::AgentCard;

    AgentCard card("aiagent-runtime-test",
                   "Test agent wired to a fake model.",
                   "0.1.0");
    card.WithDefaultInputModes({"text/plain"})
            .WithDefaultOutputModes({"text/plain"});
    return card;
}

std::string FirstText(const a2a::models::Message& message) {
    for (const auto& part : message.parts) {
        if (part.text.has_value()) {
            return *part.text;
        }
    }
    return {};
}

a2a::models::SendMessageRequest MakeRequest(const std::string& text,
                                            const std::string& task_id,
                                            const std::string& context_id) {
    a2a::models::SendMessageRequest request;
    request.message = a2a::models::MakeTextMessage(
            a2a::models::Role::kUser, text, "text/plain");
    request.message.task_id = task_id;
    request.message.context_id = context_id;
    return request;
}

std::vector<a2a::models::Message> HistoryByContext(
        const std::shared_ptr<a2a::server::ITaskStore>& store,
        const std::string& context_id) {
    std::vector<a2a::models::Message> history;
    if (context_id.empty()) {
        return history;
    }
    std::size_t latest_history_size = 0;
    for (const auto& task : store->ListAll()) {
        if (task.context_id == context_id &&
            task.history.size() > latest_history_size) {
            history = task.history;
            latest_history_size = task.history.size();
        }
    }
    return history;
}

}  // namespace

int main() {
    try {
        auto model = std::make_shared<FakeModel>();
        aiagent::agent::AgentRuntime runtime(model);
        a2a::server::AgentServer agent_server(BuildCard());

        agent_server.task_manager().SetMessageHandler(
                [&](const a2a::models::SendMessageRequest& request) {
                    const std::string context_id = request.message.context_id;
                    auto history =
                            HistoryByContext(agent_server.task_manager().task_store(),
                                             context_id);
                    history.push_back(request.message);
                    return runtime.HandleMessage(request.message, history, context_id);
                });

        const auto first_response =
                agent_server.task_manager().SendMessage(MakeRequest("hello", "", "ctx-1"));
        Expect(first_response.IsTask(), "first response should contain a task");
        const std::string task_id = first_response.GetTask().id;
        Expect(!task_id.empty(), "first task should have an id");
        Expect(first_response.GetTask().history.size() == 2,
               "first task history should include user and assistant messages");

        const auto second_response =
                agent_server.task_manager().SendMessage(MakeRequest("again", "", "ctx-1"));
        Expect(second_response.IsTask(), "second response should contain a task");
        Expect(model->call_count == 2, "fake model should be called twice");
        Expect(model->last_user_content == "again",
               "second model call should see the latest user message");

        auto history = HistoryByContext(agent_server.task_manager().task_store(), "ctx-1");
        Expect(history.size() == 4, "persisted history should contain two turns");
        Expect(FirstText(history.back()) == "fake-reply-to-again",
               "last history message should be the fake assistant reply");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime test failed: " << e.what() << '\n';
        return 1;
    }

    try {
        auto model = std::make_shared<StreamingFakeModel>();
        aiagent::agent::AgentRuntime runtime(model);

        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "stream", "text/plain");
        std::string streamed_tokens;
        const std::string reply = runtime.HandleMessageStreaming(
                user, {user}, "ctx-stream", [&](const std::string& token) {
                    streamed_tokens += token;
                });
        Expect(streamed_tokens == "hello", "streaming callback should receive tokens");
        Expect(reply == "hello", "streaming method should return the assembled reply");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime streaming test failed: " << e.what() << '\n';
        return 1;
    }

    try {
        auto model = std::make_shared<AlwaysToolModel>();
        auto tools = std::make_shared<OkToolProvider>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetToolProvider(tools);
        runtime.SetMaxToolRounds(3);

        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "long task", "text/plain");
        bool reached_limit = false;
        try {
            runtime.HandleMessageStreaming(user, {user}, "ctx-limit", nullptr);
        } catch (const std::exception& e) {
            reached_limit = std::string(e.what()).find("max tool rounds") !=
                            std::string::npos;
        }
        Expect(reached_limit,
               "runtime should report reaching max tool rounds instead of silent stop");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime max round test failed: " << e.what() << '\n';
        return 1;
    }

    try {
        auto model = std::make_shared<FiniteToolModel>();
        auto tools = std::make_shared<OkToolProvider>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetToolProvider(tools);

        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "finite long task", "text/plain");
        const std::string reply =
                runtime.HandleMessageStreaming(user, {user}, "ctx-finite", nullptr);
        Expect(reply == "finished-after-tools",
               "default runtime should continue past the old 8-round limit");
        Expect(model->calls == 6, "runtime should finish when the model naturally stops");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime unlimited rounds test failed: " << e.what() << '\n';
        return 1;
    }

    if (g_failures != 0) {
        std::cerr << "agent runtime test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "agent runtime test passed\n";
    return 0;
}
