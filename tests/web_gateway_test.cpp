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

#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
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

// 解析 SSE 响应体为 (event, data) 帧，便于按帧断言（不依赖 JSON 键序）。
struct SseFrame {
    std::string event;
    nlohmann::json data = nlohmann::json::object();
};

std::vector<SseFrame> ParseSseFrames(const std::string& body) {
    std::vector<SseFrame> frames;
    std::size_t pos = 0;
    while (pos < body.size()) {
        const std::size_t end = body.find("\n\n", pos);
        const std::string block =
                body.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = (end == std::string::npos) ? body.size() : end + 2;
        SseFrame frame;
        std::istringstream lines(block);
        std::string line;
        while (std::getline(lines, line)) {
            constexpr const char* kEventPrefix = "event: ";
            constexpr const char* kDataPrefix = "data: ";
            if (line.rfind(kEventPrefix, 0) == 0) {
                frame.event = line.substr(std::strlen(kEventPrefix));
            } else if (line.rfind(kDataPrefix, 0) == 0) {
                try {
                    frame.data = nlohmann::json::parse(line.substr(std::strlen(kDataPrefix)));
                } catch (const nlohmann::json::exception&) {
                    // 非 JSON data（如 done 帧的意外内容）保持空对象。
                }
            }
        }
        if (!frame.event.empty()) {
            frames.push_back(std::move(frame));
        }
    }
    return frames;
}

const SseFrame* FindFrame(const std::vector<SseFrame>& frames, const std::string& event,
                          const std::string& id = {}) {
    for (const auto& frame : frames) {
        if (frame.event != event) {
            continue;
        }
        if (!id.empty() && frame.data.value("id", std::string()) != id) {
            continue;
        }
        return &frame;
    }
    return nullptr;
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

        // 结构化事件：approval_request 帧应带工具名与原因。
        gateway.SetStreamEventHandler(
                [](const std::string&, const std::string&, const std::string&,
                   const aiagent::agent::AgentEventCallback& on_event,
                   const aiagent::web::WebGateway::CancelCallback&) {
                    aiagent::agent::AgentEvent approval;
                    approval.kind = aiagent::agent::AgentEventKind::kApprovalRequest;
                    approval.tool_call_id = "call-1";
                    approval.tool_name = "bash";
                    approval.arguments = R"({"command":"ls"})";
                    approval.approval_reason = "command execution needs approval";
                    on_event(approval);
                    aiagent::agent::AgentEvent token;
                    token.kind = aiagent::agent::AgentEventKind::kToken;
                    token.content = "after-approval";
                    on_event(token);
                });
        std::string approval_body;
        const auto approval_stream = http.PostStream(
                base + "/api/v1/chat/stream",
                R"({"session_id":null,"message":"need-approval"})",
                "application/json",
                [&approval_body](std::string_view chunk) {
                    approval_body.append(chunk.data(), chunk.size());
                });
        Expect(approval_stream.IsSuccess(), "approval stream should succeed");
        Expect(approval_body.find("event: approval_request") != std::string::npos,
               "stream should emit approval_request event");
        Expect(approval_body.find("command execution needs approval") !=
                       std::string::npos,
               "approval request should carry the reason");

        // 子 Agent 归属：任何帧都带 agent_id / parent_tool_call_id，且新增
        // subagent_start / subagent_end 帧；根 Agent 的事件不带这两个字段。
        gateway.SetStreamEventHandler(
                [](const std::string&, const std::string&, const std::string&,
                   const aiagent::agent::AgentEventCallback& on_event,
                   const aiagent::web::WebGateway::CancelCallback&) {
                    aiagent::agent::AgentEvent root;
                    root.kind = aiagent::agent::AgentEventKind::kToolStart;
                    root.tool_call_id = "call_parent";
                    root.tool_name = "task";
                    root.arguments = R"({"prompt":"explore"})";
                    on_event(root);

                    aiagent::agent::AgentEvent start;
                    start.kind = aiagent::agent::AgentEventKind::kSubAgentStart;
                    start.agent_id = "explorer#1";
                    start.parent_tool_call_id = "call_parent";
                    on_event(start);

                    aiagent::agent::AgentEvent child_tool;
                    child_tool.kind = aiagent::agent::AgentEventKind::kToolStart;
                    child_tool.tool_call_id = "sub1:c1";
                    child_tool.tool_name = "grep";
                    child_tool.arguments = R"({"pattern":"x"})";
                    child_tool.agent_id = "explorer#1";
                    child_tool.parent_tool_call_id = "call_parent";
                    on_event(child_tool);

                    aiagent::agent::AgentEvent end;
                    end.kind = aiagent::agent::AgentEventKind::kSubAgentEnd;
                    end.agent_id = "explorer#1";
                    end.parent_tool_call_id = "call_parent";
                    end.ok = true;
                    end.duration_ms = 1234;
                    end.tool_calls = 3;
                    end.prompt_tokens = 4000;
                    end.completion_tokens = 400;
                    end.cache_hit_tokens = 1000;
                    on_event(end);
                });
        std::string agents_body;
        const auto agents_stream = http.PostStream(
                base + "/api/v1/chat/stream",
                R"({"session_id":null,"message":"delegate"})",
                "application/json",
                [&agents_body](std::string_view chunk) {
                    agents_body.append(chunk.data(), chunk.size());
                });
        Expect(agents_stream.IsSuccess(), "sub-agent stream should succeed");
        const std::vector<SseFrame> agent_frames = ParseSseFrames(agents_body);
        const SseFrame* start_frame = FindFrame(agent_frames, "subagent_start");
        const SseFrame* end_frame = FindFrame(agent_frames, "subagent_end");
        const SseFrame* child_tool_frame = FindFrame(agent_frames, "tool_start", "sub1:c1");
        const SseFrame* root_tool_frame = FindFrame(agent_frames, "tool_start", "call_parent");
        Expect(start_frame != nullptr, "stream should emit subagent_start");
        Expect(end_frame != nullptr, "stream should emit subagent_end");
        Expect(child_tool_frame != nullptr, "stream should forward the child tool_start");
        Expect(root_tool_frame != nullptr, "stream should forward the root tool_start");
        if (start_frame != nullptr) {
            Expect(start_frame->data.value("agent_id", std::string()) == "explorer#1" &&
                           start_frame->data.value("parent_tool_call_id", std::string()) ==
                                   "call_parent",
                   "subagent_start should carry agent id and parent tool call id");
        }
        if (end_frame != nullptr) {
            Expect(end_frame->data.value("ok", false) == true &&
                           end_frame->data.value("duration_ms", 0) == 1234 &&
                           end_frame->data.value("tool_calls", 0) == 3 &&
                           end_frame->data.value("agent_id", std::string()) == "explorer#1",
                   "subagent_end should carry ok, duration, tool calls and the agent id");
            Expect(end_frame->data.value("prompt_tokens", 0LL) == 4000 &&
                           end_frame->data.value("completion_tokens", 0LL) == 400 &&
                           end_frame->data.value("cache_hit_tokens", 0LL) == 1000,
                   "subagent_end should carry the child token usage");
        }
        if (child_tool_frame != nullptr) {
            Expect(child_tool_frame->data.value("name", std::string()) == "grep" &&
                           child_tool_frame->data.value("agent_id", std::string()) ==
                                   "explorer#1" &&
                           child_tool_frame->data.value("parent_tool_call_id",
                                                        std::string()) == "call_parent",
                   "child tool frames should be attributed to the sub-agent");
        }
        if (root_tool_frame != nullptr) {
            Expect(!root_tool_frame->data.contains("agent_id") &&
                           !root_tool_frame->data.contains("parent_tool_call_id"),
                   "root frames should not carry attribution fields");
        }

        // /agents：未注入 handler 时 enabled=false；注入后返回定义与统计。
        const auto agents_off = http.Get(base + "/api/v1/agents");
        Expect(agents_off.IsSuccess(), "agents endpoint should succeed when disabled");
        Expect(nlohmann::json::parse(agents_off.body).value("enabled", true) == false,
               "agents endpoint should report enabled=false without a handler");

        std::vector<std::string> agents_sessions;
        gateway.SetAgentsInfoHandler([&agents_sessions](const std::string& session_id) {
            agents_sessions.push_back(session_id);
            aiagent::web::SubAgentSnapshot snapshot;
            snapshot.enabled = true;
            snapshot.max_depth = 1;
            snapshot.max_concurrent = 3;
            snapshot.max_background = 2;
            aiagent::web::BackgroundTaskInfo running;
            running.task_id = "bg-3";
            running.agent_id = "explorer#7";
            running.agent_type = "explorer";
            running.session_id = "sess-bg";
            running.state = "running";
            running.elapsed_ms = 42100;
            running.prompt_excerpt = "investigate the widget";
            snapshot.running.push_back(running);
            aiagent::web::BackgroundTaskInfo pending;
            pending.task_id = "bg-2";
            pending.agent_id = "general#1";
            pending.session_id = "sess-bg";
            pending.state = "done";
            pending.ok = true;
            pending.duration_ms = 3900;
            pending.tool_calls = 2;
            pending.text = "REPORT";
            snapshot.pending_results.push_back(pending);
            snapshot.runs = 2;
            snapshot.failures = 1;
            snapshot.cancelled = 0;
            snapshot.total_ms = 4200;
            snapshot.prompt_tokens = 30000;
            snapshot.completion_tokens = 3000;
            snapshot.cache_hit_tokens = 12000;
            snapshot.definitions.push_back(
                    aiagent::web::SubAgentInfo{"explorer", "read-only exploration", "",
                                               {"read", "grep", "glob", "ls"}});
            return snapshot;
        });
        const auto agents_on = http.Get(base + "/api/v1/agents");
        Expect(agents_on.IsSuccess(), "agents endpoint should succeed when enabled");
        const auto agents_json = nlohmann::json::parse(agents_on.body);
        Expect(agents_json.value("enabled", false) == true,
               "agents endpoint should report enabled=true");
        Expect(agents_json.value("max_depth", 0) == 1 &&
                       agents_json.value("max_concurrent", 0) == 3,
               "agents endpoint should report the depth and concurrency limits");
        Expect(agents_json["stats"].value("runs", 0) == 2 &&
                       agents_json["stats"].value("failures", 0) == 1 &&
                       agents_json["stats"].value("total_ms", 0) == 4200,
               "agents endpoint should report the run statistics");
        Expect(agents_json["stats"].value("prompt_tokens", 0LL) == 30000 &&
                       agents_json["stats"].value("completion_tokens", 0LL) == 3000 &&
                       agents_json["stats"].value("cache_hit_tokens", 0LL) == 12000,
               "agents endpoint should report the aggregated token usage");
        Expect(agents_json["definitions"].size() == 1 &&
                       agents_json["definitions"][0].value("name", std::string()) ==
                               "explorer",
               "agents endpoint should list the definitions");
        Expect(agents_json.value("max_background", -1) == 2,
               "agents endpoint should report the background limit");
        Expect(agents_json["running"].size() == 1 &&
                       agents_json["running"][0].value("task_id", std::string()) == "bg-3" &&
                       agents_json["running"][0].value("state", std::string()) == "running" &&
                       agents_json["running"][0].value("elapsed_ms", 0L) == 42100 &&
                       agents_json["running"][0].value("prompt_excerpt", std::string()) ==
                               "investigate the widget",
               "agents endpoint should list running background tasks");
        Expect(agents_json["pending_results"].size() == 1 &&
                       agents_json["pending_results"][0].value("task_id", std::string()) == "bg-2" &&
                       agents_json["pending_results"][0].value("text", std::string()) == "REPORT" &&
                       agents_json["pending_results"][0].value("ok", false),
               "agents endpoint should list unprinted background results");
        // session_id 查询参数原样透传给装配方（过滤在 main 侧做）。
        Expect(http.Get(base + "/api/v1/agents?session_id=sess-bg").IsSuccess() &&
                       !agents_sessions.empty() && agents_sessions.back() == "sess-bg",
               "agents endpoint should pass session_id through");

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

        // ---- 工具审批接口 ----
        bool approve_called = false;
        gateway.SetApproveHandler([&](const std::string& session_id,
                                      const std::string& tool_call_id,
                                      const std::string& decision, std::string*) {
            approve_called = session_id == "s" && tool_call_id == "c" &&
                             decision == "allow_once";
            return true;
        });
        const auto approve = perm.Post(
                "/api/v1/chat/approve",
                R"({"session_id":"s","tool_call_id":"c","decision":"allow_once"})",
                "application/json");
        Expect(approve && approve->status == 200 && approve_called,
               "approve endpoint should resolve the pending request");
        const auto approve_bad = perm.Post(
                "/api/v1/chat/approve",
                R"({"session_id":"","tool_call_id":"","decision":""})",
                "application/json");
        Expect(approve_bad && approve_bad->status == 400,
               "approve endpoint should require all fields");

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

        // 后台任务控制接口：未注入 handler → 501；注入后按契约返回。
        const auto stop_unset =
                http.Post(base + "/api/v1/agents/bg-1/stop", "{}", "application/json");
        Expect(stop_unset.status_code == 501, "stop should be 501 without a handler");
        const auto ack_unset = http.Post(base + "/api/v1/agents/ack",
                                        R"({"task_ids":["bg-1"]})", "application/json");
        Expect(ack_unset.status_code == 501, "ack should be 501 without a handler");

        std::vector<std::string> stopped_ids;
        std::vector<std::vector<std::string>> acked_batches;
        gateway.SetBackgroundStopHandler([&stopped_ids](const std::string& id, bool* requested,
                                                        std::string* message) {
            if (id == "bg-9") {
                if (message != nullptr) {
                    *message = "unknown background task: bg-9";
                }
                return false;
            }
            stopped_ids.push_back(id);
            const bool finished = id == "bg-2";
            if (requested != nullptr) {
                *requested = !finished;
            }
            if (message != nullptr) {
                *message = finished ? "bg-2 already finished (done)"
                                    : "stopping " + id + " (explorer#7)";
            }
            return true;
        });
        gateway.SetBackgroundAckHandler(
                [&acked_batches](const std::vector<std::string>& ids) -> std::size_t {
                    acked_batches.push_back(ids);
                    return ids.size();
                });

        const auto stop_requested =
                http.Post(base + "/api/v1/agents/bg-1/stop", "{}", "application/json");
        Expect(stop_requested.IsSuccess() &&
                       nlohmann::json::parse(stop_requested.body).value("result", std::string()) ==
                               "requested",
               "stop should report requested for a running task, got: " + stop_requested.body);
        const auto stop_finished =
                http.Post(base + "/api/v1/agents/bg-2/stop", "{}", "application/json");
        Expect(stop_finished.IsSuccess() &&
                       nlohmann::json::parse(stop_finished.body).value("result", std::string()) ==
                               "finished",
               "stop should report an already finished task, got: " + stop_finished.body);
        const auto stop_by_agent = http.Post(
                base + "/api/v1/agents/explorer%237/stop", "{}", "application/json");
        Expect(stop_by_agent.IsSuccess() && stopped_ids.size() == 3 &&
                       stopped_ids.back() == "explorer#7",
               "stop should accept the agent id form, got: " + stop_by_agent.body);
        const auto stop_unknown =
                http.Post(base + "/api/v1/agents/bg-9/stop", "{}", "application/json");
        Expect(stop_unknown.status_code == 404 &&
                       nlohmann::json::parse(stop_unknown.body)
                                       .value("error", std::string())
                                       .find("bg-9") != std::string::npos,
               "unknown task should return 404, got: " + stop_unknown.body);

        const auto ack = http.Post(base + "/api/v1/agents/ack",
                                   R"({"task_ids":["bg-1","bg-2"]})", "application/json");
        Expect(ack.IsSuccess() && nlohmann::json::parse(ack.body).value("acked", 0) == 2 &&
                       acked_batches.size() == 1 && acked_batches[0].size() == 2,
               "ack should pass the ids through and report the count, got: " + ack.body);
        const auto ack_bad =
                http.Post(base + "/api/v1/agents/ack", "{not json", "application/json");
        Expect(ack_bad.status_code == 400, "invalid ack body should be rejected");

        // 后台纠偏接口：未注入 handler → 501；注入后按状态映射到 200/404/409/400。
        const auto steer_unset = http.Post(base + "/api/v1/agents/bg-1/steer",
                                          R"({"message":"x"})", "application/json");
        Expect(steer_unset.status_code == 501, "steer should be 501 without a handler");

        std::vector<std::string> steered_ids;
        gateway.SetBackgroundSteerHandler(
                [&steered_ids](const std::string& id, const std::string& text,
                               const std::string& session_id)
                        -> aiagent::web::WebGateway::BackgroundSteerResult {
                    using Status = aiagent::web::WebGateway::BackgroundSteerStatus;
                    aiagent::web::WebGateway::BackgroundSteerResult result;
                    steered_ids.push_back(id);
                    result.message =
                            "steering " + id + " text=" + text + " session=" + session_id;
                    if (id == "bg-9") {
                        result.status = Status::kNotFound;
                        result.message = "unknown background task: bg-9";
                    } else if (id == "bg-2") {
                        result.status = Status::kFinished;
                        result.message = "bg-2 already finished (done)";
                    } else if (id == "bg-3") {
                        result.status = Status::kFull;
                        result.message = "background task inbox is full (8)";
                    } else if (id == "bg-4") {
                        result.status = Status::kTooLarge;
                        result.message = "steer message is too long (4096 bytes max)";
                    } else {
                        result.status = Status::kAccepted;
                    }
                    return result;
                });

        const auto steer_ok = http.Post(
                base + "/api/v1/agents/bg-1/steer",
                R"({"message":"只看 include/","session_id":"sess-bg"})", "application/json");
        Expect(steer_ok.IsSuccess() &&
                       nlohmann::json::parse(steer_ok.body).value("result", std::string()) ==
                               "accepted",
               "steer should accept a running task, got: " + steer_ok.body);
        Expect(steer_ok.body.find("text=只看 include/") != std::string::npos &&
                       steer_ok.body.find("session=sess-bg") != std::string::npos,
               "steer should pass message and session_id through, got: " + steer_ok.body);
        const auto steer_missing = http.Post(base + "/api/v1/agents/bg-9/steer",
                                            R"({"message":"x"})", "application/json");
        Expect(steer_missing.status_code == 404 &&
                       nlohmann::json::parse(steer_missing.body)
                                       .value("error", std::string())
                                       .find("bg-9") != std::string::npos,
               "unknown task should return 404, got: " + steer_missing.body);
        const auto steer_finished = http.Post(base + "/api/v1/agents/bg-2/steer",
                                             R"({"message":"x"})", "application/json");
        Expect(steer_finished.status_code == 409 &&
                       nlohmann::json::parse(steer_finished.body)
                                       .value("error", std::string())
                                       .find("already finished") != std::string::npos,
               "finished task should return 409, got: " + steer_finished.body);
        Expect(http.Post(base + "/api/v1/agents/bg-3/steer", R"({"message":"x"})",
                        "application/json")
                               .status_code == 409,
               "full inbox should return 409");
        const auto steer_large = http.Post(base + "/api/v1/agents/bg-4/steer",
                                          R"({"message":"x"})", "application/json");
        Expect(steer_large.status_code == 400 &&
                       nlohmann::json::parse(steer_large.body)
                                       .value("error", std::string())
                                       .find("too long") != std::string::npos,
               "oversized message should return 400, got: " + steer_large.body);
        const auto steer_bad_body = http.Post(base + "/api/v1/agents/bg-1/steer",
                                             "{not json", "application/json");
        Expect(steer_bad_body.status_code == 400, "invalid steer body should be rejected");
        Expect(steered_ids.size() == 5, "every steer call should reach the handler");

        // 回合外事件推送：活跃流内到达的帧按序补发；无活跃流时静默丢弃。
        gateway.SetStreamEventHandler(
                [&gateway](const std::string& session_id, const std::string&,
                           const std::string&,
                           const aiagent::agent::AgentEventCallback& on_event,
                           const aiagent::web::WebGateway::CancelCallback&) {
                    aiagent::agent::AgentEvent token;
                    token.kind = aiagent::agent::AgentEventKind::kToken;
                    token.content = "先";
                    on_event(token);
                    gateway.PublishSessionEvent(session_id, "subagent_result",
                                                {{"task_id", "bg-3"},
                                                 {"agent_id", "explorer#7"},
                                                 {"ok", true}});
                });
        std::string queued_body;
        const auto queued = http.PostStream(
                base + "/api/v1/chat/stream",
                R"({"session_id":"sess-queue","message":"queue-me"})", "application/json",
                [&queued_body](std::string_view chunk) {
                    queued_body.append(chunk.data(), chunk.size());
                });
        Expect(queued.IsSuccess(), "stream with a published background frame should succeed");
        const auto queued_frames = ParseSseFrames(queued_body);
        const auto* result_frame = FindFrame(queued_frames, "subagent_result");
        Expect(result_frame != nullptr &&
                       result_frame->data.value("task_id", std::string()) == "bg-3" &&
                       result_frame->data.value("ok", false),
               "published frame should reach the active stream, body: " + queued_body);
        const std::size_t result_pos = queued_body.find("event: subagent_result");
        const std::size_t done_pos = queued_body.find("event: done");
        Expect(result_pos != std::string::npos && done_pos != std::string::npos &&
                       result_pos < done_pos,
               "published frame should arrive before done");
        // 无活跃流：丢弃（CLI 轮询兜底），不影响后续请求。
        gateway.PublishSessionEvent("sess-absent", "subagent_result", {{"task_id", "bg-404"}});
        Expect(http.Get(base + "/health").IsSuccess(),
               "publishing without an active stream should not break the gateway");

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
