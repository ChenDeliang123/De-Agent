#pragma once

#include <memory>
#include <functional>
#include <string>
#include <vector>

#include "a2a/models/message.hpp"
#include "a2a/models/operations.hpp"
#include "a2a/models/requests.hpp"

#include "aiagent/agent/agent_event.hpp"
#include "aiagent/llm/deepseek_client.hpp"
#include "aiagent/tools/tool_provider.hpp"

namespace aiagent::agent {

// 把 A2A Message 历史翻译成 LLM messages，并执行一次对话。
// 不负责 HTTP/任务存储，由 A2A 层把 TaskManager 与它接起来。
class AgentRuntime {
public:
    explicit AgentRuntime(std::shared_ptr<llm::IModelClient> model,
                          std::string system_prompt =
                                  "You are a helpful AI agent implemented in C++.");

    // 可选：注入 MCP/工具提供者后，Agent 会自动执行 model 的 tool_calls。
    void SetToolProvider(std::shared_ptr<tools::IToolProvider> tools);

    // history 应包含本次用户消息（续聊时由调用方从 TaskStore 取回）。
    // 返回一个已完成 Task，其 history 追加了 assistant 回复。
    a2a::models::SendMessageResponse HandleMessage(
            const a2a::models::Message& incoming,
            const std::vector<a2a::models::Message>& history,
            const std::string& context_id = {});

    // 流式版本：逐 token 回调，支持工具调用循环；返回最终回复文本。
    std::string HandleMessageStreaming(
            const a2a::models::Message& incoming,
            const std::vector<a2a::models::Message>& history,
            const std::string& context_id,
            const std::function<void(const std::string& token)>& on_token);

    // 流式版本（结构化事件）：逐 token 与工具调用起止均以 AgentEvent 回调。
    std::string HandleMessageStreamingEvents(
            const a2a::models::Message& incoming,
            const std::vector<a2a::models::Message>& history,
            const std::string& context_id,
            const AgentEventCallback& on_event);

private:
    // 流式主循环的公共实现：产出 AgentEvent 事件流，返回最终回复文本。
    std::string RunStreaming(const a2a::models::Message& incoming,
                             const std::vector<a2a::models::Message>& history,
                             const std::string& context_id,
                             const AgentEventCallback& on_event);

    std::shared_ptr<llm::IModelClient> model_;
    std::shared_ptr<tools::IToolProvider> tools_;
    std::string system_prompt_;
};

}  // namespace aiagent::agent
