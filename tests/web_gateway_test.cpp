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
#include "aiagent/permissions/permission_store.hpp"
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
        gateway.SetSessionDetailsHandler(
                [&](const std::string& id, std::string* cwd,
                    std::vector<aiagent::web::InstructionSourceInfo>* instructions) {
                    if (id != session_id) {
                        return false;
                    }
                    *cwd = "/tmp/stats-project";
                    instructions->push_back(aiagent::web::InstructionSourceInfo{
                            "/tmp/stats-project/AGENTS.md", 32, false});
                    return true;
                });
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

        // ---- 权限模式 REST ----
        aiagent::permissions::PermissionStore permission_store;
        aiagent::agent::PermissionSettings permission_settings;
        gateway.SetPermissionsListHandler([&]() {
            aiagent::web::PermissionsSnapshot snapshot;
            snapshot.default_mode =
                    aiagent::permissions::ModeName(permission_settings.default_mode);
            for (const auto& [path, mode] : permission_store.List()) {
                snapshot.entries.push_back(
                        {path, aiagent::permissions::ModeName(mode)});
            }
            return snapshot;
        });
        gateway.SetPermissionResolveHandler(
                [&](const std::string& path, std::string* mode, std::string* source) {
                    if (const auto entry = permission_store.Resolve(path);
                        entry.has_value()) {
                        *mode = aiagent::permissions::ModeName(entry->mode);
                        *source = entry->inherited ? "inherited" : "exact";
                    } else {
                        *mode = aiagent::permissions::ModeName(
                                permission_settings.default_mode);
                        *source = "default";
                    }
                    return true;
                });
        gateway.SetPermissionSetHandler(
                [&](const std::string& path, const std::string& mode_text,
                    std::string* effective, std::string* error) {
                    const auto mode = aiagent::permissions::ParseMode(mode_text);
                    if (!mode.has_value()) {
                        if (error != nullptr) {
                            *error = "invalid mode: " + mode_text;
                        }
                        return false;
                    }
                    permission_store.Set(path, *mode);
                    if (effective != nullptr) {
                        *effective = aiagent::permissions::ModeName(*mode);
                    }
                    return true;
                });
        gateway.SetPermissionRemoveHandler([&](const std::string& path, std::string*) {
            permission_store.Remove(path);
            return true;
        });

        httplib::Client perm(base);
        const auto perm_put = perm.Put(
                "/api/v1/permissions",
                R"({"path":"/tmp/permission-test","mode":"workspace"})",
                "application/json");
        Expect(perm_put && perm_put->status == 200, "PUT permissions should succeed");

        const auto perm_eff =
                perm.Get("/api/v1/permissions/effective?path=/tmp/permission-test");
        Expect(perm_eff && perm_eff->status == 200,
               "effective endpoint should succeed");
        const auto perm_eff_json = nlohmann::json::parse(perm_eff->body);
        Expect(perm_eff_json.value("mode", std::string()) == "workspace",
               "effective mode should be workspace");
        Expect(perm_eff_json.value("source", std::string()) == "exact",
               "effective source should be exact");

        const auto perm_inherited = perm.Get(
                "/api/v1/permissions/effective?path=/tmp/permission-test/sub");
        Expect(perm_inherited && perm_inherited->status == 200,
               "inherited effective endpoint should succeed");
        Expect(nlohmann::json::parse(perm_inherited->body).value("source",
                                                                 std::string()) ==
                       "inherited",
               "child path should inherit");

        const auto perm_default =
                perm.Get("/api/v1/permissions/effective?path=/tmp/other-perm");
        Expect(nlohmann::json::parse(perm_default->body).value("source", std::string()) ==
                       "default",
               "unset path should report default");

        const auto perm_bad = perm.Put(
                "/api/v1/permissions",
                R"({"path":"/tmp/permission-test","mode":"bogus"})",
                "application/json");
        Expect(perm_bad && perm_bad->status == 400, "invalid mode should return 400");

        const auto perm_list = perm.Get("/api/v1/permissions");
        Expect(perm_list && perm_list->status == 200, "permissions list should succeed");
        const auto perm_list_json = nlohmann::json::parse(perm_list->body);
        Expect(perm_list_json.value("default_mode", std::string()) == "confirm",
               "list should include default mode");

        const auto perm_del =
                perm.Delete("/api/v1/permissions?path=/tmp/permission-test");
        Expect(perm_del && perm_del->status == 200,
               "DELETE permissions should succeed");
        const auto perm_after =
                perm.Get("/api/v1/permissions/effective?path=/tmp/permission-test");
        Expect(nlohmann::json::parse(perm_after->body).value("source", std::string()) ==
                       "default",
               "after delete, path falls back to default");

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
        Expect(stats_json.value("cwd", std::string()) == "/tmp/stats-project",
               "stats should include the session cwd");
        Expect(stats_json.contains("instructions") &&
                       stats_json["instructions"].is_array() &&
                       stats_json["instructions"].size() == 1,
               "stats should include instruction sources");

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
