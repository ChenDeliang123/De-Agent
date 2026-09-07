#include "aiagent/web/web_gateway.hpp"

#include <httplib.h>
#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <functional>
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

namespace aiagent::web {
namespace {

std::string FirstText(const a2a::models::Message& message) {
    for (const auto& part : message.parts) {
        if (part.text.has_value()) {
            return *part.text;
        }
    }
    return {};
}

std::vector<a2a::models::Message> LatestHistory(a2a::server::ITaskStore& store,
                                                const std::string& context_id) {
    std::vector<a2a::models::Message> history;
    std::size_t latest_size = 0;
    for (const auto& task : store.ListAll()) {
        if (task.context_id == context_id && task.history.size() > latest_size) {
            history = task.history;
            latest_size = task.history.size();
        }
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
                                                       LatestHistory(task_store_, context))}});
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
                        const auto history = LatestHistory(task_store_, it->second);
                        nlohmann::json payload = nlohmann::json::array();
                        for (const auto& message : history) {
                            payload.push_back({{"role", RoleName(message.role)},
                                               {"content", FirstText(message)}});
                        }
                        response.set_content(payload.dump(), "application/json");
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
                             StreamChat(session_id, content, response);
                         } catch (const std::exception& e) {
                             response.status = 400;
                             response.set_content(
                                     nlohmann::json{{"error", e.what()}}.dump(),
                                     "application/json");
                         }
                     });
    }

    nlohmann::json ChatOnce(const std::string& session_id, const std::string& content) {
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
        return {{"session_id", session_id},
                {"task_id", task.id},
                {"content", reply}};
    }

    void StreamChat(const std::string& session_id, const std::string& content,
                    httplib::Response& response) {
        response.status = 200;
        response.set_chunked_content_provider(
                "text/event-stream",
                [this, session_id, content](size_t offset, httplib::DataSink& sink) {
                    if (offset != 0) {
                        sink.done();
                        return false;
                    }
                    const auto write = [&sink](const std::string& event,
                                               const nlohmann::json& payload) {
                        const std::string frame =
                                "event: " + event + "\ndata: " + payload.dump() + "\n\n";
                        return sink.write(frame.data(), frame.size());
                    };
                    try {
                        write("started", {{"session_id", session_id}});
                        nlohmann::json done_payload = {{"session_id", session_id}};
                        if (stream_handler_) {
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
                        write("error", {{"error", e.what()}});
                    }
                    sink.done();
                    return false;
                });
    }

    a2a::server::AgentServer& agent_server_;
    a2a::server::ITaskStore& task_store_;
    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
    bool running_ = false;
    WebGateway::StreamChatHandler stream_handler_;
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

}  // namespace aiagent::web
