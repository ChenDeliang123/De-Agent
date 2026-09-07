#pragma once

#include <memory>
#include <string>
#include <vector>

#include "a2a/models/message.hpp"
#include "a2a/models/operations.hpp"
#include "a2a/models/requests.hpp"

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

private:
    std::shared_ptr<llm::IModelClient> model_;
    std::shared_ptr<tools::IToolProvider> tools_;
    std::string system_prompt_;
};

}  // namespace aiagent::agent
