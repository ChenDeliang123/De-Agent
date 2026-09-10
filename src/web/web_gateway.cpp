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
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/core/id.hpp"
#include "a2a/models/message.hpp"
#include "a2a/models/requests.hpp"
#include "a2a/models/task.hpp"
#include "a2a/server/agent_server.hpp"
#include "a2a/server/task_store.hpp"

#include "aiagent/agent/history_cache.hpp"
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
    std::vector<a2a::models::Message> history;
    if (history_cache != nullptr && history_cache->TryGet(context_id, history)) {
        return history;
    }
    // 取该会话写入时间最新的任务：手动/自动压缩会写入更短的历史，
    // 单纯比较 history.size() 会错误地回退到压缩前的旧任务。
    std::optional<a2a::core::TimePoint> latest_time;
    for (const auto& task : store.ListAll()) {
        if (task.context_id != context_id || task.history.empty()) {
            continue;
        }
        if (latest_time.has_value()) {
            if (!task.status.timestamp.has_value() ||
                !(*task.status.timestamp > *latest_time)) {
                continue;
            }
        }
        history = task.history;
        latest_time = task.status.timestamp;
    }
    if (history_cache != nullptr && !history.empty()) {
        // 冷启动回填，避免每个会话都重复全表扫描。
        history_cache->Put(context_id, history);
    }
    return history;
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

void SetCommonHeaders(httplib::Response& response) {
    response.set_header("Access-Control-Allow-Origin", "*");
    response.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    response.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
}

// 将 AgentEvent 序列化为 SSE 帧（event + data）。arguments 原始 JSON 若能
// 解析则作为对象嵌入，否则按字符串透传。
void WriteAgentEventFrame(
        const std::function<void(const std::string&, const nlohmann::json&)>& write,
        const aiagent::agent::AgentEvent& event) {
    using aiagent::agent::AgentEventKind;
    switch (event.kind) {
        case AgentEventKind::kToken:
            write("content", {{"content", event.content}});
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
            write("tool_start", {{"id", event.tool_call_id},
                                {"name", event.tool_name},
                                {"arguments", arguments}});
            break;
        }
        case AgentEventKind::kToolOutput:
            write("tool_output", {{"id", event.tool_call_id},
                                  {"content", event.content}});
            break;
        case AgentEventKind::kToolEnd:
            write("tool_end", {{"id", event.tool_call_id},
                               {"ok", event.ok},
                               {"text", event.text}});
            break;
        case AgentEventKind::kCompaction:
            write("compaction", {{"tokens_before", event.compaction_tokens_before},
                                 {"tokens_after", event.compaction_tokens_after},
                                 {"forced", event.compaction_forced}});
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

    void SetCompactionInfoHandler(WebGateway::CompactionInfoHandler handler) {
        compaction_info_handler_ = std::move(handler);
    }

    void CancelSession(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(cancels_mutex_);
        const auto it = active_cancels_.find(session_id);
        if (it != active_cancels_.end() && it->second) {
            it->second->store(true);
        }
    }

private:
    void SetupRoutes() {
        server_.Options("/.*", [](const httplib::Request&, httplib::Response& response) {
            SetCommonHeaders(response);
            response.status = 204;
        });

        server_.Get("/health", [this](const httplib::Request&, httplib::Response& response) {
            SetCommonHeaders(response);
            response.set_content(R"({"status":"ok"})", "application/json");
        });

        server_.Get("/api/v1/sessions",
                    [this](const httplib::Request&, httplib::Response& response) {
                        SetCommonHeaders(response);
                        std::set<std::string> contexts;
                        for (const auto& task : task_store_.ListAll()) {
                            if (!task.context_id.empty()) {
                                contexts.insert(task.context_id);
                            }
                        }
                        nlohmann::json payload = nlohmann::json::array();
                        for (const auto& context : contexts) {
                            payload.push_back(
                                    {{"id", context},
                                     {"title", SessionTitleFromHistory(
                                                       LatestHistory(task_store_,
                                                                     history_cache_, context))}});
                        }
                        response.set_content(payload.dump(), "application/json");
                    });

        server_.Get("/api/v1/sessions/:id/messages",
                    [this](const httplib::Request& request, httplib::Response& response) {
                        SetCommonHeaders(response);
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
                         SetCommonHeaders(response);
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
                         std::string error;
                         if (!compact_handler_(it->second, &error)) {
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
                        SetCommonHeaders(response);
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

        server_.Post("/api/v1/chat",
                     [this](const httplib::Request& request, httplib::Response& response) {
                         SetCommonHeaders(response);
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
                         SetCommonHeaders(response);
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
                         SetCommonHeaders(response);
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
                    auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
                    {
                        std::lock_guard<std::mutex> lock(cancels_mutex_);
                        active_cancels_[session_id] = cancel_flag;
                    }
                    const auto clear_cancel = [this, session_id, cancel_flag]() {
                        std::lock_guard<std::mutex> lock(cancels_mutex_);
                        const auto it = active_cancels_.find(session_id);
                        if (it != active_cancels_.end() && it->second == cancel_flag) {
                            active_cancels_.erase(it);
                        }
                    };
                    const auto write = [&sink](const std::string& event,
                                               const nlohmann::json& payload) {
                        const std::string frame =
                                "event: " + event + "\ndata: " + payload.dump() + "\n\n";
                        return sink.write(frame.data(), frame.size());
                    };
                    try {
                        write("started", {{"session_id", session_id}});
                        nlohmann::json done_payload = {{"session_id", session_id}};
                        if (stream_event_handler_) {
                            stream_event_handler_(
                                    session_id,
                                    content,
                                    cwd,
                                    [&write](const aiagent::agent::AgentEvent& event) {
                                        WriteAgentEventFrame(write, event);
                                    },
                                    [cancel_flag] { return cancel_flag->load(); });
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
                    clear_cancel();
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
    WebGateway::StreamChatHandler stream_handler_;
    WebGateway::StreamEventHandler stream_event_handler_;
    WebGateway::CompactHandler compact_handler_;
    WebGateway::CompactionInfoHandler compaction_info_handler_;
    std::mutex cancels_mutex_;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> active_cancels_;
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

void WebGateway::SetCompactionInfoHandler(WebGateway::CompactionInfoHandler handler) {
    impl_->SetCompactionInfoHandler(std::move(handler));
}

void WebGateway::SetHistoryCache(aiagent::agent::HistoryCache* history_cache) {
    impl_->SetHistoryCache(history_cache);
}

}  // namespace aiagent::web
