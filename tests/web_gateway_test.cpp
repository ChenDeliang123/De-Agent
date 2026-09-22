#include <a2a/core/http_client.hpp>
#include <a2a/core/id.hpp>
#include <a2a/core/version.hpp>
#include <a2a/models/agent_card.hpp>
#include <a2a/models/message.hpp>
#include <a2a/models/requests.hpp>
#include <a2a/models/task.hpp>
#include <a2a/server/agent_server.hpp>
#include <a2a/server/memory_task_store.hpp>

#include <httplib.h>
#include <json.hpp>

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "aiagent/agent/agent_runtime.hpp"
#include "aiagent/agent/session_meta_store.hpp"
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
        Expect(session_list[0].value("id", std::string()) == session_id,
               "session object should contain its id");
        Expect(!session_list[0].value("title", std::string()).empty(),
               "session object should contain a derived title");

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

        std::string compact_cwd;
        gateway.SetCompactHandler(
                [&](const std::string& id, const std::string& cwd, std::string* error) {
                    compact_cwd = cwd;
                    if (id == "ctx-compact") {
                        return true;
                    }
                    if (error != nullptr) {
                        *error = "session not found";
                    }
                    return false;
                });
        gateway.SetCompactionInfoHandler(
                [](const std::string& id, aiagent::agent::CompactionRecord* record) {
                    if (id != "ctx-compact") {
                        return false;
                    }
                    record->forced = true;
                    record->tokens_before = 100;
                    record->tokens_after = 20;
                    record->messages_before = 8;
                    record->messages_after = 3;
                    return true;
                });

        const auto compact = http.Post(
                base + "/api/v1/sessions/ctx-compact/compact",
                R"({"cwd":"/tmp/compact-project"})", "application/json");
        Expect(compact.IsSuccess(), "compact endpoint should succeed");
        Expect(compact_cwd == "/tmp/compact-project",
               "compact endpoint should forward cwd");
        Expect(nlohmann::json::parse(compact.body).value("compacted", false),
               "compact response should report success");

        const auto compact_missing = http.Post(
                base + "/api/v1/sessions/ctx-missing/compact", "", "application/json");
        Expect(compact_missing.status_code == 409,
               "unknown session compact should fail with 409");

        const auto info = http.Get(base + "/api/v1/sessions/ctx-compact/compaction");
        Expect(info.IsSuccess(), "compaction info endpoint should succeed");
        const auto info_json = nlohmann::json::parse(info.body);
        Expect(info_json.value("found", false), "compaction info should be found");
        Expect(info_json.value("tokens_before", 0) == 100,
               "compaction info tokens_before should be returned");
        Expect(info_json.value("tokens_after", 0) == 20,
               "compaction info tokens_after should be returned");

        const auto info_missing =
                http.Get(base + "/api/v1/sessions/ctx-missing/compaction");
        Expect(info_missing.status_code == 404,
               "missing compaction info should return 404");

        // ---- P1: 会话命名 / 统计 / 复制 / 删除 / 工具清单 ----
        aiagent::agent::SessionMetaStore meta_store;
        gateway.SetSessionMetaStore(&meta_store);
        gateway.SetDeleteHandler([&](const std::string& id, std::string* error) {
            std::vector<std::string> task_ids;
            for (const auto& task : task_store->ListAll()) {
                if (task.context_id == id) {
                    task_ids.push_back(task.id);
                }
            }
            if (task_ids.empty()) {
                if (error != nullptr) {
                    *error = "session not found";
                }
                return false;
            }
            for (const auto& task_id : task_ids) {
                task_store->Delete(task_id);
            }
            meta_store.Remove(id);
            return true;
        });
        gateway.SetCloneHandler(
                [&](const std::string& source, std::string* new_id, std::string* error) {
                    std::vector<a2a::models::Message> history;
                    for (const auto& task : task_store->ListAll()) {
                        if (task.context_id == source &&
                            task.history.size() > history.size()) {
                            history = task.history;
                        }
                    }
                    if (history.empty()) {
                        if (error != nullptr) {
                            *error = "session not found";
                        }
                        return false;
                    }
                    const std::string target = std::string(a2a::core::NewUuid());
                    a2a::models::Task cloned;
                    cloned.id = std::string(a2a::core::NewUuid());
                    cloned.context_id = target;
                    cloned.status = a2a::models::MakeTaskStatus(
                            a2a::models::TaskState::kCompleted);
                    cloned.history = history;
                    task_store->Put(std::move(cloned));
                    meta_store.SetTitle(target, "copy");
                    if (new_id != nullptr) {
                        *new_id = target;
                    }
                    return true;
                });
        gateway.SetToolsHandler([]() {
            return std::vector<aiagent::web::ToolInfo>{{"read", "Read a file"},
                                                      {"bash", "Run a command"}};
        });

        const auto sessions_p1 = http.Get(base + "/api/v1/sessions");
        const auto session_list_p1 = nlohmann::json::parse(sessions_p1.body);
        Expect(!session_list_p1.empty(), "sessions should list the chat session");
        Expect(session_list_p1[0].contains("updated_at"),
               "session should include updated_at for recency ordering");

        httplib::Client raw(base);
        const auto renamed = raw.Patch(
                ("/api/v1/sessions/" + session_id).c_str(),
                R"({"title":"renamed session"})", "application/json");
        Expect(renamed && renamed->status == 200, "rename endpoint should succeed");
        const auto sessions_renamed =
                nlohmann::json::parse(http.Get(base + "/api/v1/sessions").body);
        bool renamed_seen = false;
        for (const auto& item : sessions_renamed) {
            if (item.value("id", std::string()) == session_id) {
                renamed_seen = item.value("title", std::string()) == "renamed session";
            }
        }
        Expect(renamed_seen, "renamed title should override the derived title");

        const auto stats =
                http.Get(base + "/api/v1/sessions/" + session_id + "/stats");
        Expect(stats.IsSuccess(), "stats endpoint should succeed");
        const auto stats_json = nlohmann::json::parse(stats.body);
        Expect(stats_json.value("messages", 0) == 2, "stats messages should be 2");
        Expect(stats_json.value("user_messages", 0) == 1,
               "stats user messages should be 1");
        Expect(stats_json.value("assistant_messages", 0) == 1,
               "stats assistant messages should be 1");
        Expect(stats_json.contains("compaction"), "stats should include compaction");

        const auto cloned = http.Post(
                base + "/api/v1/sessions/" + session_id + "/clone", "",
                "application/json");
        Expect(cloned.IsSuccess(), "clone endpoint should succeed");
        const std::string cloned_id =
                nlohmann::json::parse(cloned.body).value("session_id", std::string());
        Expect(!cloned_id.empty() && cloned_id != session_id,
               "clone should return a new session id");
        Expect(http.Get(base + "/api/v1/sessions/" + cloned_id + "/stats").IsSuccess(),
               "cloned session should have stats");

        const auto tools_result = http.Get(base + "/api/v1/tools");
        Expect(tools_result.IsSuccess(), "tools endpoint should succeed");
        Expect(nlohmann::json::parse(tools_result.body).size() == 2,
               "tools endpoint should return two tools");

        const auto deleted = raw.Delete(("/api/v1/sessions/" + session_id).c_str());
        Expect(deleted && deleted->status == 200, "delete endpoint should succeed");
        const auto delete_missing = raw.Delete("/api/v1/sessions/does-not-exist");
        Expect(delete_missing && delete_missing->status == 404,
               "deleting a missing session should return 404");
        Expect(http.Get(base + "/api/v1/sessions/" + session_id + "/stats").status_code ==
                       404,
               "deleted session stats should return 404");

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
