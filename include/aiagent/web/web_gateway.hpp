#pragma once

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
    WebGateway(a2a::server::AgentServer& agent_server,
               a2a::server::ITaskStore& task_store);
    ~WebGateway();

    WebGateway(const WebGateway&) = delete;
    WebGateway& operator=(const WebGateway&) = delete;

    bool Start(const std::string& host, int port);
    void Stop();
    int bound_port() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aiagent::web
