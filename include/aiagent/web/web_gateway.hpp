#pragma once

#include <functional>
#include <memory>
#include <string>

#include "aiagent/agent/agent_event.hpp"

namespace a2a::server {
class AgentServer;
class ITaskStore;
}  // namespace a2a::server

namespace aiagent::agent {
class HistoryCache;
}  // namespace aiagent::agent

namespace aiagent::web {

// 面向 Node/浏览器前端的 REST + SSE Gateway。
// 只做协议转换与会话编排，业务逻辑仍由 AgentServer/TaskManager 承担。
class WebGateway {
public:
    using TokenCallback = std::function<void(const std::string& token)>;
    using CancelCallback = std::function<bool()>;
    using StreamChatHandler =
            std::function<void(const std::string& session_id,
                               const std::string& message,
                               const TokenCallback& on_token)>;

    // 新：结构化事件版流式回调（token + tool_start/tool_output/tool_end）。
    using StreamEventHandler = std::function<void(
            const std::string& session_id,
            const std::string& message,
            const std::string& cwd,
            const aiagent::agent::AgentEventCallback& on_event,
            const CancelCallback& should_cancel)>;

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

    // 注入结构化事件版流式处理逻辑（token + 工具事件）。设置后优先于
    // SetStreamHandler 使用；未设置时保留旧的 token-only 行为。
    void SetStreamEventHandler(StreamEventHandler handler);

    // 注入会话历史内存缓存（可选）。命中后，读取会话历史/标题的 REST 接口
    // 不再每次全表扫描 task store；未注入时行为与旧实现一致。
    void SetHistoryCache(aiagent::agent::HistoryCache* history_cache);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aiagent::web
