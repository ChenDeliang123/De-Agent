#pragma once

#include <functional>
#include <memory>
#include <string>

namespace a2a::server {
class AgentServer;
class ITaskStore;
}  // namespace a2a::server

namespace aiagent::web {

// 面向 Node/浏览器前端的 REST + SSE Gateway。
// 只做协议转换与会话编排，业务逻辑仍由 AgentServer/TaskManager 承担。
class WebGateway {
public:
    using TokenCallback = std::function<void(const std::string& token)>;
    using StreamChatHandler =
            std::function<void(const std::string& session_id,
                               const std::string& message,
                               const TokenCallback& on_token)>;

    WebGateway(a2a::server::AgentServer& agent_server,
               a2a::server::ITaskStore& task_store);
    ~WebGateway();

    WebGateway(const WebGateway&) = delete;
    WebGateway& operator=(const WebGateway&) = delete;

    bool Start(const std::string& host, int port);
    void Stop();
    int bound_port() const noexcept;

    // 注入真正的逐 token 流式处理逻辑（由 main 接线到 AgentRuntime）。
    void SetStreamHandler(StreamChatHandler handler);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aiagent::web
