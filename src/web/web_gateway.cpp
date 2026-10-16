#include "aiagent/web/web_gateway.hpp"

#include <httplib.h>
#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/core/id.hpp"
#include "a2a/core/time.hpp"
#include "a2a/models/message.hpp"
#include "a2a/models/requests.hpp"
#include "a2a/models/task.hpp"
#include "a2a/server/agent_server.hpp"
#include "a2a/server/task_store.hpp"

#include "aiagent/agent/history_cache.hpp"
#include "aiagent/agent/session_history.hpp"
#include "aiagent/agent/session_meta_store.hpp"
#include "aiagent/log.hpp"

namespace aiagent::web {
namespace {

class RequestIdGuard {
public:
    explicit RequestIdGuard(std::string request_id)
        : request_id_(std::move(request_id)) {
        aiagent::SetThreadRequestId(request_id_);
    }

    ~RequestIdGuard() { aiagent::ClearThreadRequestId(); }

private:
    std::string request_id_;
};

std::string FirstText(const a2a::models::Message& message) {
    for (const auto& part : message.parts) {
        if (part.text.has_value()) {
            return *part.text;
        }
    }
    return {};
}

std::vector<a2a::models::Message> LatestHistory(a2a::server::ITaskStore& store,
                                                aiagent::agent::HistoryCache* history_cache,
                                                const std::string& context_id) {
    return aiagent::agent::LoadLatestHistory(history_cache, store, context_id);
}

std::string RoleName(a2a::models::Role role) {
    switch (role) {
        case a2a::models::Role::kUser:
            return "user";
        case a2a::models::Role::kAgent:
            return "assistant";
        default:
            return "unknown";
    }
}

std::string StringValue(const nlohmann::json& object, const char* key) {
    if (!object.contains(key) || !object[key].is_string()) {
        return {};
    }
    return object[key].get<std::string>();
}

std::string ShortTitle(std::string text) {
    constexpr std::size_t kMaxBytes = 48;
    if (text.size() <= kMaxBytes) {
        return text;
    }
    std::size_t end = kMaxBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end) + "…";
}

std::string SessionTitleFromHistory(const std::vector<a2a::models::Message>& history) {
    for (const auto& message : history) {
        if (message.role == a2a::models::Role::kUser) {
            const std::string text = FirstText(message);
            if (!text.empty()) {
                return ShortTitle(text);
            }
        }
    }
    return "新对话";
}

// 后台任务快照 → JSON（GET /api/v1/agents 的 running[]/pending_results[]）。
nlohmann::json BackgroundTasksJson(const std::vector<BackgroundTaskInfo>& tasks) {
    nlohmann::json array = nlohmann::json::array();
    for (const auto& task : tasks) {
        nlohmann::json item = {{"task_id", task.task_id},
                               {"agent_id", task.agent_id},
                               {"agent_type", task.agent_type},
                               {"session_id", task.session_id},
                               {"state", task.state},
                               {"elapsed_ms", task.elapsed_ms},
                               {"prompt_excerpt", task.prompt_excerpt},
                               {"ok", task.ok},
                               {"duration_ms", task.duration_ms},
                               {"tool_calls", task.tool_calls},
                               {"prompt_tokens", task.prompt_tokens},
                               {"completion_tokens", task.completion_tokens},
                               {"cache_hit_tokens", task.cache_hit_tokens},
                               {"error", task.error}};
        if (!task.text.empty()) {
            item["text"] = task.text;
        }
        array.push_back(std::move(item));
    }
    return array;
}

// 将 AgentEvent 序列化为 SSE 帧（event + data）。arguments 原始 JSON 若能
// 解析则作为对象嵌入，否则按字符串透传。
// 归属：agent_id / parent_tool_call_id 非空时附加到任意帧（缺省 = 根 Agent），
// 客户端据此把子 Agent 的输出折叠/加前缀。
void WriteAgentEventFrame(
        const std::function<void(const std::string&, const nlohmann::json&)>& write,
        const aiagent::agent::AgentEvent& event) {
    using aiagent::agent::AgentEventKind;
    const auto emit_frame = [&write, &event](const std::string& name,
                                            nlohmann::json payload) {
        if (!event.agent_id.empty()) {
            payload["agent_id"] = event.agent_id;
        }
        if (!event.parent_tool_call_id.empty()) {
            payload["parent_tool_call_id"] = event.parent_tool_call_id;
        }
        write(name, payload);
    };
    switch (event.kind) {
        case AgentEventKind::kToken:
            emit_frame("content", {{"content", event.content}});
            break;
        case AgentEventKind::kToolStart: {
            nlohmann::json arguments = nlohmann::json::object();
            if (!event.arguments.empty()) {
                try {
                    arguments = nlohmann::json::parse(event.arguments);
                } catch (const nlohmann::json::exception&) {
                    arguments = event.arguments;
                }
            }
            emit_frame("tool_start", {{"id", event.tool_call_id},
                                      {"name", event.tool_name},
                                      {"arguments", arguments}});
            break;
        }
        case AgentEventKind::kToolOutput:
            emit_frame("tool_output", {{"id", event.tool_call_id},
                                       {"content", event.content}});
            break;
        case AgentEventKind::kToolEnd:
            emit_frame("tool_end", {{"id", event.tool_call_id},
                                    {"ok", event.ok},
                                    {"text", event.text}});
            break;
        case AgentEventKind::kCompaction:
            emit_frame("compaction", {{"tokens_before", event.compaction_tokens_before},
                                      {"tokens_after", event.compaction_tokens_after},
                                      {"forced", event.compaction_forced}});
            break;
        case AgentEventKind::kApprovalRequest: {
            nlohmann::json arguments = nlohmann::json::object();
            if (!event.arguments.empty()) {
                try {
                    arguments = nlohmann::json::parse(event.arguments);
                } catch (const nlohmann::json::exception&) {
                    arguments = event.arguments;
                }
            }
            emit_frame("approval_request", {{"id", event.tool_call_id},
                                            {"name", event.tool_name},
                                            {"arguments", arguments},
                                            {"reason", event.approval_reason}});
            break;
        }
        case AgentEventKind::kSubAgentStart:
            emit_frame("subagent_start", nlohmann::json::object());
            break;
        case AgentEventKind::kSubAgentEnd:
            emit_frame("subagent_end", {{"ok", event.ok},
                                        {"duration_ms", event.duration_ms},
                                        {"tool_calls", event.tool_calls},
                                        {"prompt_tokens", event.prompt_tokens},
                                        {"completion_tokens", event.completion_tokens},
                                        {"cache_hit_tokens", event.cache_hit_tokens},
                                        {"text", event.text}});
            break;
    }
}

}  // namespace

class WebGateway::Impl {
public:
    Impl(a2a::server::AgentServer& agent_server, a2a::server::ITaskStore& task_store)
        : agent_server_(agent_server), task_store_(task_store) {
        SetupRoutes();
    }

    ~Impl() { Stop(); }

    bool Start(const std::string& host, int port) {
        if (running_) {
            return false;
        }
        if (port == 0) {
            port_ = server_.bind_to_any_port(host, 0);
            if (port_ <= 0) {
                return false;
            }
        } else {
            if (!server_.bind_to_port(host, port, 0)) {
                return false;
            }
            port_ = port;
        }
        running_ = true;
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        return true;
    }

    void Stop() {
        if (!running_) {
            return;
        }
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
        running_ = false;
    }

    int bound_port() const noexcept { return port_; }

    void SetStreamHandler(WebGateway::StreamChatHandler handler) {
        stream_handler_ = std::move(handler);
    }

    void SetHistoryCache(aiagent::agent::HistoryCache* history_cache) {
        history_cache_ = history_cache;
    }

    void SetStreamEventHandler(WebGateway::StreamEventHandler handler) {
        stream_event_handler_ = std::move(handler);
    }

    void SetCompactHandler(WebGateway::CompactHandler handler) {
        compact_handler_ = std::move(handler);
    }

    void SetSessionDetailsHandler(WebGateway::SessionDetailsHandler handler) {
        session_details_handler_ = std::move(handler);
    }

    void SetCompactionInfoHandler(WebGateway::CompactionInfoHandler handler) {
        compaction_info_handler_ = std::move(handler);
    }

    void SetDeleteHandler(WebGateway::DeleteHandler handler) {
        delete_handler_ = std::move(handler);
    }

    void SetCloneHandler(WebGateway::CloneHandler handler) {
        clone_handler_ = std::move(handler);
    }

    void SetToolsHandler(WebGateway::ToolsHandler handler) {
        tools_handler_ = std::move(handler);
    }

    void SetAgentsInfoHandler(WebGateway::AgentsInfoHandler handler) {
        agents_handler_ = std::move(handler);
    }

    void SetBackgroundStopHandler(WebGateway::BackgroundStopHandler handler) {
        background_stop_handler_ = std::move(handler);
    }

    void SetBackgroundAckHandler(WebGateway::BackgroundAckHandler handler) {
        background_ack_handler_ = std::move(handler);
    }

    void SetPermissionsListHandler(WebGateway::PermissionsListHandler handler) {
        permissions_list_handler_ = std::move(handler);
    }

    void SetPermissionResolveHandler(WebGateway::PermissionResolveHandler handler) {
        permission_resolve_handler_ = std::move(handler);
    }

    void SetPermissionSetHandler(WebGateway::PermissionSetHandler handler) {
        permission_set_handler_ = std::move(handler);
    }

    void SetPermissionRemoveHandler(WebGateway::PermissionRemoveHandler handler) {
        permission_remove_handler_ = std::move(handler);
    }

    void SetProjectPermissionHandler(WebGateway::ProjectPermissionHandler handler) {
        project_permission_handler_ = std::move(handler);
    }

    void SetTrustSetHandler(WebGateway::TrustSetHandler handler) {
        trust_set_handler_ = std::move(handler);
    }

    void SetPermissionInitHandler(WebGateway::PermissionInitHandler handler) {
        permission_init_handler_ = std::move(handler);
    }

    void SetApproveHandler(WebGateway::ApproveHandler handler) {
        approve_handler_ = std::move(handler);
    }

    void SetCancelObserver(WebGateway::CancelObserver observer) {
        cancel_observer_ = std::move(observer);
    }

    void SetSessionMetaStore(aiagent::agent::SessionMetaStore* session_meta_store) {
        session_meta_store_ = session_meta_store;
    }

    void CancelSession(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        const auto it = active_streams_.find(session_id);
        if (it != active_streams_.end() && it->second && it->second->cancel_flag) {
            it->second->cancel_flag->store(true);
        }
        if (cancel_observer_) {
            cancel_observer_(session_id);
        }
    }

    void PublishSessionEvent(const std::string& session_id, const std::string& event,
                             const nlohmann::json& payload) {
        std::shared_ptr<ActiveStream> stream;
        {
            std::lock_guard<std::mutex> lock(streams_mutex_);
            const auto it = active_streams_.find(session_id);
            if (it == active_streams_.end() || !it->second) {
                return;  // 没有活跃流：丢掉（CLI 轮询 pending_results 兜底）
            }
            stream = it->second;
        }
        std::lock_guard<std::mutex> queue_lock(stream->queue_mutex);
        stream->queued_frames.emplace_back(event, payload);
    }

private:
    void SetupRoutes() {
        server_.Get("/health", [this](const httplib::Request&, httplib::Response& response) {
            response.set_content(R"({"status":"ok"})", "application/json");
        });

        server_.Get("/api/v1/sessions",
                    [this](const httplib::Request&, httplib::Response& response) {
                        // 单次遍历任务表：每个 context 取写入时间最新的历史，
                        // 同时记录 updated_at 以便按最近使用排序。
                        struct Entry {
                            std::string id;
                            std::vector<a2a::models::Message> history;
                            std::optional<a2a::core::TimePoint> updated;
                        };
                        std::map<std::string, Entry> entries;
                        for (const auto& task : task_store_.ListAll()) {
                            if (task.context_id.empty()) {
                                continue;
                            }
                            Entry& entry = entries[task.context_id];
                            entry.id = task.context_id;
                            if (!task.status.timestamp.has_value()) {
                                if (entry.history.empty()) {
                                    entry.history = task.history;
                                }
                                continue;
                            }
                            if (!entry.updated.has_value() ||
                                *task.status.timestamp > *entry.updated) {
                                entry.updated = task.status.timestamp;
                                entry.history = task.history;
                            }
                        }
                        std::vector<Entry> ordered;
                        ordered.reserve(entries.size());
                        for (auto& [id, entry] : entries) {
                            (void)id;
                            if (entry.history.empty()) {
                                entry.history = LatestHistory(task_store_, history_cache_,
                                                              entry.id);
                            }
                            ordered.push_back(std::move(entry));
                        }
                        std::sort(ordered.begin(), ordered.end(),
                                  [](const Entry& left, const Entry& right) {
                                      const auto left_time = left.updated.value_or(
                                              a2a::core::TimePoint{});
                                      const auto right_time = right.updated.value_or(
                                              a2a::core::TimePoint{});
                                      if (left_time != right_time) {
                                          return left_time > right_time;
                                      }
                                      return left.id < right.id;
                                  });
                        nlohmann::json payload = nlohmann::json::array();
                        for (const auto& entry : ordered) {
                            std::string title;
                            if (session_meta_store_ != nullptr) {
                                if (auto stored = session_meta_store_->GetTitle(entry.id);
                                    stored.has_value()) {
                                    title = *stored;
                                }
                            }
                            if (title.empty()) {
                                title = SessionTitleFromHistory(entry.history);
                            }
                            nlohmann::json item = {{"id", entry.id}, {"title", title}};
                            if (entry.updated.has_value()) {
                                item["updated_at"] =
                                        std::chrono::duration_cast<std::chrono::milliseconds>(
                                                entry.updated->time_since_epoch())
                                                .count();
                            }
                            payload.push_back(std::move(item));
                        }
                        response.set_content(payload.dump(), "application/json");
                    });

        server_.Get("/api/v1/sessions/:id/messages",
                    [this](const httplib::Request& request, httplib::Response& response) {
                        const auto it = request.path_params.find("id");
                        if (it == request.path_params.end()) {
                            response.status = 400;
                            response.set_content(R"({"error":"missing session id"})",
                                                 "application/json");
                            return;
                        }
                        const auto history = LatestHistory(task_store_, history_cache_, it->second);
                        nlohmann::json payload = nlohmann::json::array();
                        for (const auto& message : history) {
                            payload.push_back({{"role", RoleName(message.role)},
                                               {"content", FirstText(message)}});
                        }
                        response.set_content(payload.dump(), "application/json");
                    });

        server_.Post("/api/v1/sessions/:id/compact",
                     [this](const httplib::Request& request, httplib::Response& response) {
                         const auto it = request.path_params.find("id");
                         if (it == request.path_params.end()) {
                             response.status = 400;
                             response.set_content(R"({"error":"missing session id"})",
                                                  "application/json");
                             return;
                         }
                         if (!compact_handler_) {
                             response.status = 501;
                             response.set_content(
                                     R"({"error":"compaction not available"})",
                                     "application/json");
                             return;
                         }
                         std::string cwd;
                         if (!request.body.empty()) {
                             try {
                                 const auto body = nlohmann::json::parse(request.body);
                                 cwd = StringValue(body, "cwd");
                             } catch (const nlohmann::json::exception& e) {
                                 response.status = 400;
                                 response.set_content(
                                         nlohmann::json{
                                                 {"error", std::string("invalid JSON: ") +
                                                                   e.what()}}
                                                 .dump(),
                                         "application/json");
                                 return;
                             }
                         }
                         std::string error;
                         if (!compact_handler_(it->second, cwd, &error)) {
                             response.status = 409;
                             response.set_content(
                                     nlohmann::json{{"compacted", false},
                                                    {"error", error}}
                                             .dump(),
                                     "application/json");
                             return;
                         }
                         response.set_content(
                                 nlohmann::json{{"compacted", true}}.dump(),
                                 "application/json");
                     });

        server_.Get("/api/v1/sessions/:id/compaction",
                    [this](const httplib::Request& request, httplib::Response& response) {
                        const auto it = request.path_params.find("id");
                        if (it == request.path_params.end()) {
                            response.status = 400;
                            response.set_content(R"({"error":"missing session id"})",
                                                 "application/json");
                            return;
                        }
                        aiagent::agent::CompactionRecord record;
                        if (!compaction_info_handler_ ||
                            !compaction_info_handler_(it->second, &record)) {
                            response.status = 404;
                            response.set_content(R"({"found":false})",
                                                 "application/json");
                            return;
                        }
                        response.set_content(
                                nlohmann::json{{"found", true},
                                               {"forced", record.forced},
                                               {"tokens_before", record.tokens_before},
                                               {"tokens_after", record.tokens_after},
                                               {"messages_before", record.messages_before},
                                               {"messages_after", record.messages_after}}
                                        .dump(),
                                "application/json");
                    });

        // 设置会话显式标题（/name）。
        server_.Patch("/api/v1/sessions/:id",
                      [this](const httplib::Request& request, httplib::Response& response) {
                          const auto it = request.path_params.find("id");
                          if (it == request.path_params.end()) {
                              response.status = 400;
                              response.set_content(R"({"error":"missing session id"})",
                                                   "application/json");
                              return;
                          }
                          if (session_meta_store_ == nullptr) {
                              response.status = 501;
                              response.set_content(R"({"error":"session rename not available"})",
                                                   "application/json");
                              return;
                          }
                          try {
                              const auto body = nlohmann::json::parse(request.body);
                              const std::string title = StringValue(body, "title");
                              if (title.empty()) {
                                  response.status = 400;
                                  response.set_content(R"({"error":"title is required"})",
                                                       "application/json");
                                  return;
                              }
                              const std::string short_title = ShortTitle(title);
                              session_meta_store_->SetTitle(it->second, short_title);
                              aiagent::DefaultLogger().Log(
                                      aiagent::LogLevel::kInfo, "web",
                                      "session=" + it->second + " event=session_rename");
                              response.set_content(
                                      nlohmann::json{{"id", it->second},
                                                     {"title", short_title}}
                                              .dump(),
                                      "application/json");
                          } catch (const nlohmann::json::exception& e) {
                              response.status = 400;
                              response.set_content(
                                      nlohmann::json{{"error", std::string("invalid JSON: ") +
                                                                      e.what()}}
                                              .dump(),
                                      "application/json");
                          }
                      });

        // 删除会话及其全部任务。
        server_.Delete("/api/v1/sessions/:id",
                       [this](const httplib::Request& request, httplib::Response& response) {
                           const auto it = request.path_params.find("id");
                           if (it == request.path_params.end()) {
                               response.status = 400;
                               response.set_content(R"({"error":"missing session id"})",
                                                    "application/json");
                               return;
                           }
                           if (!delete_handler_) {
                               response.status = 501;
                               response.set_content(R"({"error":"session delete not available"})",
                                                    "application/json");
                               return;
                           }
                           std::string error;
                           if (!delete_handler_(it->second, &error)) {
                               response.status = 404;
                               response.set_content(
                                       nlohmann::json{{"error", error}}.dump(),
                                       "application/json");
                               return;
                           }
                           response.set_content(
                                   nlohmann::json{{"deleted", true}, {"id", it->second}}
                                           .dump(),
                                   "application/json");
                       });

        // 复制会话（/clone、/fork）：以最新完整历史创建新会话。
        server_.Post("/api/v1/sessions/:id/clone",
                     [this](const httplib::Request& request, httplib::Response& response) {
                         const auto it = request.path_params.find("id");
                         if (it == request.path_params.end()) {
                             response.status = 400;
                             response.set_content(R"({"error":"missing session id"})",
                                                  "application/json");
                             return;
                         }
                         if (!clone_handler_) {
                             response.status = 501;
                             response.set_content(R"({"error":"session clone not available"})",
                                                  "application/json");
                             return;
                         }
                         std::string new_id;
                         std::string error;
                         if (!clone_handler_(it->second, &new_id, &error)) {
                             response.status = 404;
                             response.set_content(nlohmann::json{{"error", error}}.dump(),
                                                  "application/json");
                             return;
                         }
                         response.set_content(
                                 nlohmann::json{{"session_id", new_id}}.dump(),
                                 "application/json");
                     });

        // 会话统计（/session）：消息数、字符数、任务数与最近一次压缩。
        server_.Get("/api/v1/sessions/:id/stats",
                    [this](const httplib::Request& request, httplib::Response& response) {
                        const auto it = request.path_params.find("id");
                        if (it == request.path_params.end()) {
                            response.status = 400;
                            response.set_content(R"({"error":"missing session id"})",
                                                 "application/json");
                            return;
                        }
                        const auto history =
                                LatestHistory(task_store_, history_cache_, it->second);
                        if (history.empty()) {
                            response.status = 404;
                            response.set_content(R"({"error":"session not found"})",
                                                 "application/json");
                            return;
                        }
                        std::size_t user_messages = 0;
                        std::size_t assistant_messages = 0;
                        std::size_t characters = 0;
                        for (const auto& message : history) {
                            const std::string text = FirstText(message);
                            characters += text.size();
                            if (message.role == a2a::models::Role::kUser) {
                                ++user_messages;
                            } else if (message.role == a2a::models::Role::kAgent) {
                                ++assistant_messages;
                            }
                        }
                        std::size_t tasks = 0;
                        for (const auto& task : task_store_.ListAll()) {
                            if (task.context_id == it->second) {
                                ++tasks;
                            }
                        }
                        nlohmann::json payload = {{"id", it->second},
                                                  {"messages", history.size()},
                                                  {"user_messages", user_messages},
                                                  {"assistant_messages", assistant_messages},
                                                  {"characters", characters},
                                                  {"tasks", tasks},
                                                  {"compaction", nullptr}};
                        aiagent::agent::CompactionRecord record;
                        if (compaction_info_handler_ &&
                            compaction_info_handler_(it->second, &record)) {
                            payload["compaction"] = {{"forced", record.forced},
                                                     {"tokens_before", record.tokens_before},
                                                     {"tokens_after", record.tokens_after},
                                                     {"messages_before", record.messages_before},
                                                     {"messages_after", record.messages_after}};
                        }
                        std::string details_cwd;
                        std::vector<InstructionSourceInfo> instructions;
                        if (session_details_handler_ != nullptr &&
                            session_details_handler_(it->second, &details_cwd,
                                                     &instructions)) {
                            if (!details_cwd.empty()) {
                                payload["cwd"] = details_cwd;
                            }
                            nlohmann::json files = nlohmann::json::array();
                            for (const auto& file : instructions) {
                                files.push_back({{"path", file.path},
                                                 {"bytes", file.bytes},
                                                 {"truncated", file.truncated}});
                            }
                            payload["instructions"] = std::move(files);
                        } else if (session_meta_store_ != nullptr) {
                            if (auto cwd = session_meta_store_->GetCwd(it->second);
                                cwd.has_value()) {
                                payload["cwd"] = *cwd;
                            }
                        }
                        response.set_content(payload.dump(), "application/json");
                    });

        // 可用工具清单（/tools）：内置 coding 工具 + MCP 工具。
        server_.Get("/api/v1/tools",
                    [this](const httplib::Request&, httplib::Response& response) {
                        if (!tools_handler_) {
                            response.status = 501;
                            response.set_content(R"({"error":"tools not available"})",
                                                 "application/json");
                            return;
                        }
                        nlohmann::json payload = nlohmann::json::array();
                        for (const auto& tool : tools_handler_()) {
                            payload.push_back({{"name", tool.name},
                                               {"description", tool.description}});
                        }
                        response.set_content(payload.dump(), "application/json");
                    });

        // 子 Agent 运行时快照（/agents）：定义清单 + 累计统计。
        // 未注入 handler（未启用多 Agent）时返回 enabled=false（200）。
        server_.Get("/api/v1/agents",
                    [this](const httplib::Request& request, httplib::Response& response) {
                        const std::string session_id =
                                request.has_param("session_id")
                                        ? request.get_param_value("session_id")
                                        : std::string();
                        nlohmann::json payload = {{"enabled", false}};
                        if (agents_handler_) {
                            const SubAgentSnapshot snapshot = agents_handler_(session_id);
                            payload = {{"enabled", snapshot.enabled},
                                       {"max_depth", snapshot.max_depth},
                                       {"max_concurrent", snapshot.max_concurrent},
                                       {"max_background", snapshot.max_background},
                                       {"stats",
                                        {{"runs", snapshot.runs},
                                         {"failures", snapshot.failures},
                                         {"cancelled", snapshot.cancelled},
                                         {"total_ms", snapshot.total_ms},
                                         {"prompt_tokens", snapshot.prompt_tokens},
                                         {"completion_tokens", snapshot.completion_tokens},
                                         {"cache_hit_tokens", snapshot.cache_hit_tokens}}}};
                            nlohmann::json definitions = nlohmann::json::array();
                            for (const auto& definition : snapshot.definitions) {
                                definitions.push_back({{"name", definition.name},
                                                       {"description", definition.description},
                                                       {"model", definition.model},
                                                       {"tools", definition.tools}});
                            }
                            payload["definitions"] = std::move(definitions);
                            payload["running"] = BackgroundTasksJson(snapshot.running);
                            payload["pending_results"] =
                                    BackgroundTasksJson(snapshot.pending_results);
                        }
                        response.set_content(payload.dump(), "application/json");
                    });

        // 停止一条后台子 Agent：POST /api/v1/agents/:id/stop（:id 为 bg-N 或 agent_id）。
        server_.Post("/api/v1/agents/:id/stop",
                     [this](const httplib::Request& request, httplib::Response& response) {
                         if (!background_stop_handler_) {
                             response.status = 501;
                             response.set_content(
                                     R"({"error":"background tasks not available"})",
                                     "application/json");
                             return;
                         }
                         const std::string id = request.path_params.at("id");
                         bool requested = false;
                         std::string message;
                         if (!background_stop_handler_(id, &requested, &message)) {
                             response.status = 404;
                             response.set_content(
                                     nlohmann::json{{"error", message}}.dump(),
                                     "application/json");
                             return;
                         }
                         response.set_content(
                                 nlohmann::json{{"result", requested ? "requested" : "finished"},
                                                {"message", message}}
                                         .dump(),
                                 "application/json");
                     });

        // 后台结果回执：POST /api/v1/agents/ack + {"task_ids":[...]}。
        server_.Post("/api/v1/agents/ack",
                     [this](const httplib::Request& request, httplib::Response& response) {
                         if (!background_ack_handler_) {
                             response.status = 501;
                             response.set_content(
                                     R"({"error":"background tasks not available"})",
                                     "application/json");
                             return;
                         }
                         try {
                             const auto body = nlohmann::json::parse(request.body);
                             std::vector<std::string> task_ids;
                             if (body.contains("task_ids") && body["task_ids"].is_array()) {
                                 for (const auto& id : body["task_ids"]) {
                                     if (id.is_string()) {
                                         task_ids.push_back(id.get<std::string>());
                                     }
                                 }
                             }
                             response.set_content(
                                     nlohmann::json{
                                             {"acked", background_ack_handler_(task_ids)}}
                                             .dump(),
                                     "application/json");
                         } catch (const std::exception& e) {
                             response.status = 400;
                             response.set_content(
                                     nlohmann::json{{"error", e.what()}}.dump(),
                                     "application/json");
                         }
                     });

        // 权限模式：查询/设置/删除（工作路径级）。
        server_.Get("/api/v1/permissions",
                    [this](const httplib::Request&, httplib::Response& response) {
                        if (!permissions_list_handler_) {
                            response.status = 501;
                            response.set_content(R"({"error":"permissions not available"})",
                                                 "application/json");
                            return;
                        }
                        const auto snapshot = permissions_list_handler_();
                        nlohmann::json payload;
                        payload["default_mode"] = snapshot.default_mode;
                        payload["paths"] = nlohmann::json::array();
                        for (const auto& entry : snapshot.entries) {
                            payload["paths"].push_back(
                                    {{"path", entry.path}, {"mode", entry.mode}});
                        }
                        response.set_content(payload.dump(), "application/json");
                    });

        server_.Get("/api/v1/permissions/effective",
                    [this](const httplib::Request& request, httplib::Response& response) {
                        if (!permission_resolve_handler_) {
                            response.status = 501;
                            response.set_content(R"({"error":"permissions not available"})",
                                                 "application/json");
                            return;
                        }
                        const std::string path = request.has_param("path")
                                                         ? request.get_param_value("path")
                                                         : std::string();
                        if (path.empty()) {
                            response.status = 400;
                            response.set_content(R"({"error":"path is required"})",
                                                 "application/json");
                            return;
                        }
                        std::string mode;
                        std::string source;
                        permission_resolve_handler_(path, &mode, &source);
                        response.set_content(
                                nlohmann::json{{"path", path},
                                               {"mode", mode},
                                               {"source", source}}
                                        .dump(),
                                "application/json");
                    });

        server_.Put("/api/v1/permissions",
                    [this](const httplib::Request& request, httplib::Response& response) {
                        if (!permission_set_handler_) {
                            response.status = 501;
                            response.set_content(R"({"error":"permissions not available"})",
                                                 "application/json");
                            return;
                        }
                        try {
                            const auto body = nlohmann::json::parse(request.body);
                            const std::string path = StringValue(body, "path");
                            const std::string mode = StringValue(body, "mode");
                            if (path.empty() || mode.empty()) {
                                response.status = 400;
                                response.set_content(
                                        R"({"error":"path and mode are required"})",
                                        "application/json");
                                return;
                            }
                            std::string effective;
                            std::string error;
                            if (!permission_set_handler_(path, mode, &effective, &error)) {
                                response.status = 400;
                                response.set_content(
                                        nlohmann::json{{"error", error}}.dump(),
                                        "application/json");
                                return;
                            }
                            response.set_content(
                                    nlohmann::json{{"path", path},
                                                   {"mode", effective},
                                                   {"source", "exact"}}
                                            .dump(),
                                    "application/json");
                        } catch (const nlohmann::json::exception& e) {
                            response.status = 400;
                            response.set_content(
                                    nlohmann::json{{"error", std::string("invalid JSON: ") +
                                                                     e.what()}}
                                            .dump(),
                                    "application/json");
                        }
                    });

        server_.Delete("/api/v1/permissions",
                       [this](const httplib::Request& request, httplib::Response& response) {
                           if (!permission_remove_handler_) {
                               response.status = 501;
                               response.set_content(
                                       R"({"error":"permissions not available"})",
                                       "application/json");
                               return;
                           }
                           const std::string path = request.has_param("path")
                                                            ? request.get_param_value("path")
                                                            : std::string();
                           if (path.empty()) {
                               response.status = 400;
                               response.set_content(R"({"error":"path is required"})",
                                                    "application/json");
                               return;
                           }
                           std::string error;
                           if (!permission_remove_handler_(path, &error)) {
                               response.status = 404;
                               response.set_content(
                                       nlohmann::json{{"error", error}}.dump(),
                                       "application/json");
                               return;
                           }
                           response.set_content(
                                   nlohmann::json{{"path", path}, {"removed", true}}.dump(),
                                   "application/json");
                       });

        // 项目级 `.de/`：查询、信任、初始化。
        server_.Get("/api/v1/permissions/project",
                    [this](const httplib::Request& request, httplib::Response& response) {
                        if (!project_permission_handler_) {
                            response.status = 501;
                            response.set_content(
                                    R"({"error":"project permissions not available"})",
                                    "application/json");
                            return;
                        }
                        const std::string path = request.has_param("path")
                                                         ? request.get_param_value("path")
                                                         : std::string();
                        if (path.empty()) {
                            response.status = 400;
                            response.set_content(R"({"error":"path is required"})",
                                                 "application/json");
                            return;
                        }
                        const auto info = project_permission_handler_(path);
                        response.set_content(
                                nlohmann::json{{"path", info.path},
                                               {"root", info.root},
                                               {"has_config", info.has_config},
                                               {"trusted", info.trusted}}
                                        .dump(),
                                "application/json");
                    });

        server_.Put("/api/v1/permissions/trust",
                    [this](const httplib::Request& request, httplib::Response& response) {
                        if (!trust_set_handler_) {
                            response.status = 501;
                            response.set_content(
                                    R"({"error":"trust not available"})",
                                    "application/json");
                            return;
                        }
                        try {
                            const auto body = nlohmann::json::parse(request.body);
                            const std::string path = StringValue(body, "path");
                            if (path.empty() || !body.contains("trusted") ||
                                !body["trusted"].is_boolean()) {
                                response.status = 400;
                                response.set_content(
                                        R"({"error":"path and boolean trusted are required"})",
                                        "application/json");
                                return;
                            }
                            const bool trusted = body["trusted"].get<bool>();
                            std::string root;
                            std::string error;
                            if (!trust_set_handler_(path, trusted, &root, &error)) {
                                response.status = 400;
                                response.set_content(
                                        nlohmann::json{{"error", error}}.dump(),
                                        "application/json");
                                return;
                            }
                            response.set_content(
                                    nlohmann::json{{"root", root}, {"trusted", trusted}}
                                            .dump(),
                                    "application/json");
                        } catch (const nlohmann::json::exception& e) {
                            response.status = 400;
                            response.set_content(
                                    nlohmann::json{{"error", std::string("invalid JSON: ") +
                                                                     e.what()}}
                                            .dump(),
                                    "application/json");
                        }
                    });

        server_.Post("/api/v1/permissions/init",
                     [this](const httplib::Request& request, httplib::Response& response) {
                         if (!permission_init_handler_) {
                             response.status = 501;
                             response.set_content(
                                     R"({"error":"init not available"})",
                                     "application/json");
                             return;
                         }
                         try {
                             const auto body = nlohmann::json::parse(request.body);
                             const std::string path = StringValue(body, "path");
                             if (path.empty()) {
                                 response.status = 400;
                                 response.set_content(
                                         R"({"error":"path is required"})",
                                         "application/json");
                                 return;
                             }
                             const bool seed = body.value("seed_from_global", false);
                             std::string root;
                             std::vector<std::string> created;
                             std::string error;
                             if (!permission_init_handler_(path, seed, &root, &created,
                                                           &error)) {
                                 response.status = 400;
                                 response.set_content(
                                         nlohmann::json{{"error", error}}.dump(),
                                         "application/json");
                                 return;
                             }
                             response.set_content(
                                     nlohmann::json{{"root", root}, {"created", created}}
                                             .dump(),
                                     "application/json");
                         } catch (const nlohmann::json::exception& e) {
                             response.status = 400;
                             response.set_content(
                                     nlohmann::json{{"error", std::string("invalid JSON: ") +
                                                                      e.what()}}
                                             .dump(),
                                     "application/json");
                         }
                     });

        server_.Post("/api/v1/chat/approve",
                     [this](const httplib::Request& request, httplib::Response& response) {
                         if (!approve_handler_) {
                             response.status = 501;
                             response.set_content(
                                     R"({"error":"approval not available"})",
                                     "application/json");
                             return;
                         }
                         try {
                             const auto body = nlohmann::json::parse(request.body);
                             const std::string session_id = StringValue(body, "session_id");
                             const std::string tool_call_id =
                                     StringValue(body, "tool_call_id");
                             const std::string decision = StringValue(body, "decision");
                             if (session_id.empty() || tool_call_id.empty() ||
                                 decision.empty()) {
                                 response.status = 400;
                                 response.set_content(
                                         R"({"error":"session_id, tool_call_id and decision are required"})",
                                         "application/json");
                                 return;
                             }
                             std::string error;
                             if (!approve_handler_(session_id, tool_call_id, decision,
                                                   &error)) {
                                 response.status = 404;
                                 response.set_content(
                                         nlohmann::json{{"error",
                                                         error.empty()
                                                                 ? std::string(
                                                                           "no pending approval")
                                                                 : error}}
                                                 .dump(),
                                         "application/json");
                                 return;
                             }
                             response.set_content(
                                     nlohmann::json{{"resolved", true}}.dump(),
                                     "application/json");
                         } catch (const nlohmann::json::exception& e) {
                             response.status = 400;
                             response.set_content(
                                     nlohmann::json{{"error", std::string("invalid JSON: ") +
                                                                      e.what()}}
                                             .dump(),
                                     "application/json");
                         }
                     });

        server_.Post("/api/v1/chat",
                     [this](const httplib::Request& request, httplib::Response& response) {
                         try {
                             const auto body = nlohmann::json::parse(request.body);
                             const std::string content = StringValue(body, "message");
                             if (content.empty()) {
                                 response.status = 400;
                                 response.set_content(R"({"error":"message is required"})",
                                                      "application/json");
                                 return;
                             }

                             std::string session_id = StringValue(body, "session_id");
                             if (session_id.empty()) {
                                 session_id = std::string(a2a::core::NewUuid());
                             }
                             const auto result = ChatOnce(session_id, content);
                             response.set_content(result.dump(), "application/json");
                         } catch (const nlohmann::json::exception& e) {
                             response.status = 400;
                             response.set_content(
                                     nlohmann::json{{"error", std::string("invalid JSON: ") +
                                                                     e.what()}}
                                             .dump(),
                                     "application/json");
                         } catch (const std::exception& e) {
                             response.status = 500;
                             response.set_content(
                                     nlohmann::json{{"error", e.what()}}.dump(),
                                     "application/json");
                         }
                     });

        server_.Post("/api/v1/chat/stream",
                     [this](const httplib::Request& request, httplib::Response& response) {
                         try {
                             const auto body = nlohmann::json::parse(request.body);
                             const std::string content = StringValue(body, "message");
                             if (content.empty()) {
                                 response.status = 400;
                                 response.set_content(R"({"error":"message is required"})",
                                                      "application/json");
                                 return;
                             }
                             std::string session_id = StringValue(body, "session_id");
                             if (session_id.empty()) {
                                 session_id = std::string(a2a::core::NewUuid());
                             }
                             const std::string cwd = StringValue(body, "cwd");
                             StreamChat(session_id, content, cwd, response);
                         } catch (const std::exception& e) {
                             response.status = 400;
                             response.set_content(
                                     nlohmann::json{{"error", e.what()}}.dump(),
                                     "application/json");
                         }
                     });

        server_.Post("/api/v1/chat/cancel",
                     [this](const httplib::Request& request, httplib::Response& response) {
                         try {
                             const auto body = nlohmann::json::parse(request.body);
                             const std::string session_id = StringValue(body, "session_id");
                             if (session_id.empty()) {
                                 response.status = 400;
                                 response.set_content(R"({"error":"session_id is required"})",
                                                      "application/json");
                                 return;
                             }
                             CancelSession(session_id);
                             aiagent::DefaultLogger().Log(
                                     aiagent::LogLevel::kInfo, "web",
                                     "session=" + session_id +
                                             " event=cancel_requested");
                             response.set_content(R"({"cancelled":true})",
                                                  "application/json");
                         } catch (const std::exception& e) {
                             response.status = 400;
                             response.set_content(
                                     nlohmann::json{{"error", e.what()}}.dump(),
                                     "application/json");
                         }
                     });
    }

    nlohmann::json ChatOnce(const std::string& session_id, const std::string& content) {
        const RequestIdGuard request_guard(std::string(a2a::core::NewUuid()));
        const auto started = std::chrono::steady_clock::now();
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kInfo, "web",
                "session=" + session_id + " event=chat_start sync=true user_bytes=" +
                        std::to_string(content.size()));
        a2a::models::SendMessageRequest request;
        request.message = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, content, "text/plain");
        request.message.context_id = session_id;

        const auto response = agent_server_.task_manager().SendMessage(request);
        const auto& task = response.GetTask();
        std::string reply;
        for (auto it = task.history.rbegin(); it != task.history.rend(); ++it) {
            if (it->role == a2a::models::Role::kAgent) {
                reply = FirstText(*it);
                break;
            }
        }
        const auto duration_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kInfo, "web",
                "session=" + session_id + " task=" + task.id +
                        " event=chat_end sync=true duration_ms=" +
                        std::to_string(duration_ms));
        return {{"session_id", session_id},
                {"task_id", task.id},
                {"content", reply}};
    }

    void StreamChat(const std::string& session_id, const std::string& content,
                    const std::string& cwd, httplib::Response& response) {
        const auto started = std::chrono::steady_clock::now();
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kInfo, "web",
                "session=" + session_id + " event=stream_start user_bytes=" +
                        std::to_string(content.size()) + " cwd=" + cwd);
        response.status = 200;
        response.set_chunked_content_provider(
                "text/event-stream",
                [this, session_id, content, cwd, started](size_t offset,
                                                          httplib::DataSink& sink) {
                    if (offset != 0) {
                        sink.done();
                        return false;
                    }
                    const RequestIdGuard request_guard(std::string(a2a::core::NewUuid()));
                    auto stream = std::make_shared<ActiveStream>();
                    stream->cancel_flag = std::make_shared<std::atomic<bool>>(false);
                    {
                        std::lock_guard<std::mutex> lock(streams_mutex_);
                        active_streams_[session_id] = stream;
                    }
                    const auto unregister = [this, session_id, stream]() {
                        std::lock_guard<std::mutex> lock(streams_mutex_);
                        const auto it = active_streams_.find(session_id);
                        if (it != active_streams_.end() && it->second == stream) {
                            active_streams_.erase(it);
                        }
                    };
                    const auto write = [&sink](const std::string& event,
                                               const nlohmann::json& payload) {
                        const std::string frame =
                                "event: " + event + "\ndata: " + payload.dump() + "\n\n";
                        return sink.write(frame.data(), frame.size());
                    };
                    // 回合外事件（后台任务的派发/结果）先出队再写本回合自己的帧：
                    // 队列由 worker 线程入队，写 socket 只发生在当前（provider）线程。
                    const auto flush_queued = [stream, &write]() {
                        std::vector<std::pair<std::string, nlohmann::json>> frames;
                        {
                            std::lock_guard<std::mutex> lock(stream->queue_mutex);
                            frames.swap(stream->queued_frames);
                        }
                        for (const auto& [name, payload] : frames) {
                            write(name, payload);
                        }
                    };
                    try {
                        write("started", {{"session_id", session_id}});
                        nlohmann::json done_payload = {{"session_id", session_id}};
                        if (stream_event_handler_) {
                            stream_event_handler_(
                                    session_id,
                                    content,
                                    cwd,
                                    [&write, &flush_queued](
                                            const aiagent::agent::AgentEvent& event) {
                                        flush_queued();
                                        WriteAgentEventFrame(write, event);
                                    },
                                    [stream] { return stream->cancel_flag->load(); });
                        } else if (stream_handler_) {
                            stream_handler_(
                                    session_id,
                                    content,
                                    [&write](const std::string& token) {
                                        write("content", {{"content", token}});
                                    });
                        } else {
                            const auto result = ChatOnce(session_id, content);
                            write("content", {{"content", result.value("content", "")}});
                            done_payload["task_id"] = result.value("task_id", "");
                        }
                        flush_queued();
                        write("done", done_payload);
                    } catch (const std::exception& e) {
                        aiagent::DefaultLogger().Log(
                                aiagent::LogLevel::kError, "web",
                                "session=" + session_id +
                                        " event=stream_error message=" + e.what());
                        write("error", {{"error", e.what()}});
                    }
                    const auto duration_ms =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
                    aiagent::DefaultLogger().Log(
                            aiagent::LogLevel::kInfo, "web",
                            "session=" + session_id + " event=stream_end duration_ms=" +
                                    std::to_string(duration_ms));
                    unregister();
                    sink.done();
                    return false;
                });
    }

    a2a::server::AgentServer& agent_server_;
    a2a::server::ITaskStore& task_store_;
    aiagent::agent::HistoryCache* history_cache_ = nullptr;
    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
    bool running_ = false;
    // 活跃 SSE 流：取消标志 + 回合外事件（后台结果）队列。
    // 队列由任意线程入队，只有流的写线程（provider 线程）排空，避免并发写 socket。
    struct ActiveStream {
        std::shared_ptr<std::atomic<bool>> cancel_flag;
        std::mutex queue_mutex;
        std::vector<std::pair<std::string, nlohmann::json>> queued_frames;
    };

    WebGateway::StreamChatHandler stream_handler_;
    WebGateway::StreamEventHandler stream_event_handler_;
    WebGateway::CompactHandler compact_handler_;
    WebGateway::SessionDetailsHandler session_details_handler_;
    WebGateway::CompactionInfoHandler compaction_info_handler_;
    WebGateway::DeleteHandler delete_handler_;
    WebGateway::CloneHandler clone_handler_;
    WebGateway::ToolsHandler tools_handler_;
    WebGateway::AgentsInfoHandler agents_handler_;
    WebGateway::BackgroundStopHandler background_stop_handler_;
    WebGateway::BackgroundAckHandler background_ack_handler_;
    WebGateway::PermissionsListHandler permissions_list_handler_;
    WebGateway::PermissionResolveHandler permission_resolve_handler_;
    WebGateway::PermissionSetHandler permission_set_handler_;
    WebGateway::PermissionRemoveHandler permission_remove_handler_;
    WebGateway::ProjectPermissionHandler project_permission_handler_;
    WebGateway::TrustSetHandler trust_set_handler_;
    WebGateway::PermissionInitHandler permission_init_handler_;
    WebGateway::ApproveHandler approve_handler_;
    WebGateway::CancelObserver cancel_observer_;
    aiagent::agent::SessionMetaStore* session_meta_store_ = nullptr;
    std::mutex streams_mutex_;
    std::map<std::string, std::shared_ptr<ActiveStream>> active_streams_;
};

WebGateway::WebGateway(a2a::server::AgentServer& agent_server,
                       a2a::server::ITaskStore& task_store)
    : impl_(std::make_unique<Impl>(agent_server, task_store)) {}

WebGateway::~WebGateway() = default;

bool WebGateway::Start(const std::string& host, int port) {
    return impl_->Start(host, port);
}
void WebGateway::Stop() { impl_->Stop(); }
int WebGateway::bound_port() const noexcept { return impl_->bound_port(); }
void WebGateway::SetStreamHandler(WebGateway::StreamChatHandler handler) {
    impl_->SetStreamHandler(std::move(handler));
}

void WebGateway::SetStreamEventHandler(WebGateway::StreamEventHandler handler) {
    impl_->SetStreamEventHandler(std::move(handler));
}

void WebGateway::SetCompactHandler(WebGateway::CompactHandler handler) {
    impl_->SetCompactHandler(std::move(handler));
}

void WebGateway::SetSessionDetailsHandler(WebGateway::SessionDetailsHandler handler) {
    impl_->SetSessionDetailsHandler(std::move(handler));
}

void WebGateway::SetCompactionInfoHandler(WebGateway::CompactionInfoHandler handler) {
    impl_->SetCompactionInfoHandler(std::move(handler));
}

void WebGateway::SetDeleteHandler(WebGateway::DeleteHandler handler) {
    impl_->SetDeleteHandler(std::move(handler));
}

void WebGateway::SetCloneHandler(WebGateway::CloneHandler handler) {
    impl_->SetCloneHandler(std::move(handler));
}

void WebGateway::SetToolsHandler(WebGateway::ToolsHandler handler) {
    impl_->SetToolsHandler(std::move(handler));
}

void WebGateway::SetAgentsInfoHandler(WebGateway::AgentsInfoHandler handler) {
    impl_->SetAgentsInfoHandler(std::move(handler));
}

void WebGateway::SetBackgroundStopHandler(WebGateway::BackgroundStopHandler handler) {
    impl_->SetBackgroundStopHandler(std::move(handler));
}

void WebGateway::SetBackgroundAckHandler(WebGateway::BackgroundAckHandler handler) {
    impl_->SetBackgroundAckHandler(std::move(handler));
}

void WebGateway::PublishSessionEvent(const std::string& session_id, const std::string& event,
                                     const nlohmann::json& payload) {
    impl_->PublishSessionEvent(session_id, event, payload);
}

void WebGateway::SetPermissionsListHandler(WebGateway::PermissionsListHandler handler) {
    impl_->SetPermissionsListHandler(std::move(handler));
}

void WebGateway::SetPermissionResolveHandler(
        WebGateway::PermissionResolveHandler handler) {
    impl_->SetPermissionResolveHandler(std::move(handler));
}

void WebGateway::SetPermissionSetHandler(WebGateway::PermissionSetHandler handler) {
    impl_->SetPermissionSetHandler(std::move(handler));
}

void WebGateway::SetPermissionRemoveHandler(
        WebGateway::PermissionRemoveHandler handler) {
    impl_->SetPermissionRemoveHandler(std::move(handler));
}

void WebGateway::SetProjectPermissionHandler(
        WebGateway::ProjectPermissionHandler handler) {
    impl_->SetProjectPermissionHandler(std::move(handler));
}

void WebGateway::SetTrustSetHandler(WebGateway::TrustSetHandler handler) {
    impl_->SetTrustSetHandler(std::move(handler));
}

void WebGateway::SetPermissionInitHandler(
        WebGateway::PermissionInitHandler handler) {
    impl_->SetPermissionInitHandler(std::move(handler));
}

void WebGateway::SetApproveHandler(WebGateway::ApproveHandler handler) {
    impl_->SetApproveHandler(std::move(handler));
}

void WebGateway::SetCancelObserver(WebGateway::CancelObserver observer) {
    impl_->SetCancelObserver(std::move(observer));
}

void WebGateway::SetSessionMetaStore(
        aiagent::agent::SessionMetaStore* session_meta_store) {
    impl_->SetSessionMetaStore(session_meta_store);
}

void WebGateway::SetHistoryCache(aiagent::agent::HistoryCache* history_cache) {
    impl_->SetHistoryCache(history_cache);
}

}  // namespace aiagent::web
