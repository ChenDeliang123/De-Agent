#pragma once

#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "a2a/models/message.hpp"
#include "a2a/models/operations.hpp"
#include "a2a/models/requests.hpp"

#include "aiagent/agent/agent_event.hpp"
#include "aiagent/agent/compaction.hpp"
#include "aiagent/llm/deepseek_client.hpp"
#include "aiagent/tools/tool_provider.hpp"

namespace aiagent::agent {

// 把 A2A Message 历史翻译成 LLM messages，并执行一次对话。
// 不负责 HTTP/任务存储，由 A2A 层把 TaskManager 与它接起来。
class AgentRuntime {
public:
    using CancelCallback = std::function<bool()>;
    // 每次成功压缩后的回调（可用于日志/审计/前端展示）。
    using CompactionCallback = std::function<void(const std::string& context_id,
                                                  const CompactionRecord&)>;

    explicit AgentRuntime(std::shared_ptr<llm::IModelClient> model,
                          std::string system_prompt =
                                  "You are a helpful AI agent implemented in C++.");

    // 可选：注入 MCP/工具提供者后，Agent 会自动执行 model 的 tool_calls。
    void SetToolProvider(std::shared_ptr<tools::IToolProvider> tools);
    void SetMaxToolRounds(int rounds);

    // 设置上下文压缩参数；默认 enabled，阈值见 CompactionSettings。
    void SetCompactionSettings(const CompactionSettings& settings);

    // 注册压缩回调（可选）。每次阈值/overflow 压缩成功后触发。
    void SetCompactionCallback(CompactionCallback callback);

    // 手动压缩一段会话历史（对应终端 /compact 与 REST 接口）。
    // 无论是否达到阈值都尝试压缩；成功时输出压缩后的 A2A 历史
    // （system 不入库，摘要 + 保留尾部），并填充 record。
    // 返回 false 时写入 error。
    bool CompactHistory(const std::string& context_id,
                        const std::vector<a2a::models::Message>& history,
                        std::vector<a2a::models::Message>* compacted_history,
                        CompactionRecord* record,
                        std::string* error);

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
            const AgentEventCallback& on_event,
            const std::string& working_directory = {},
            const CancelCallback& should_cancel = {});

private:
    // 流式主循环的公共实现：产出 AgentEvent 事件流，返回最终回复文本。
    std::string RunStreaming(const a2a::models::Message& incoming,
                             const std::vector<a2a::models::Message>& history,
                             const std::string& context_id,
                             const AgentEventCallback& on_event,
                             const std::string& working_directory,
                             const CancelCallback& should_cancel);

    // 模型调用前的阈值压缩。命中阈值时把较早的消息摘要成一条 user 消息，
    // 并重置 usage 跟踪；同一批消息（size 未变）不重复压缩。
    // force=true 时跳过阈值判断与去重（用于 overflow 恢复）。
    // 返回本次压缩记录；未压缩时返回 nullopt。
    std::optional<CompactionRecord> MaybeCompactBeforeCall(
            std::vector<llm::ChatMessage>& chat_messages,
            int& last_usage_tokens,
            std::size_t& last_usage_message_count,
            std::size_t& last_compacted_size,
            const std::string& run_id,
            const std::string& context_id,
            bool force = false);

    std::shared_ptr<llm::IModelClient> model_;
    std::shared_ptr<tools::IToolProvider> tools_;
    std::string system_prompt_;
    // 0 表示不限制轮次（默认，对齐 Pi 的“有工具调用就继续”）。
    // 显式设置正数时作为可选安全上限。
    int max_tool_rounds_ = 0;
    CompactionSettings compaction_settings_;
    CompactionCallback compaction_callback_;
};

}  // namespace aiagent::agent
