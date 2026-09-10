#include "aiagent/agent/compaction.hpp"

namespace aiagent::agent {
namespace {

// 每条消息固定的角色标记、分隔符等开销，避免长会话被系统性低估。
constexpr int kPerMessageOverhead = 4;

int EstimateBytes(std::size_t bytes) {
    if (bytes == 0) {
        return 0;
    }
    // ceil(bytes / 3)：对中文（3 字节/字）比 chars/4 更保守。
    return static_cast<int>((bytes + 2) / 3);
}

}  // namespace

int EstimateTokens(const llm::ChatMessage& message) {
    std::size_t bytes = message.content.size() + message.reasoning_content.size() +
                        message.tool_call_id.size();
    for (const auto& call : message.tool_calls) {
        bytes += call.name.size() + call.arguments.size();
    }
    return EstimateBytes(bytes) + kPerMessageOverhead;
}

int EstimateMessagesTokens(const std::vector<llm::ChatMessage>& messages) {
    int total = 0;
    for (const auto& message : messages) {
        total += EstimateTokens(message);
    }
    return total;
}

ContextUsageEstimate EstimateContextTokens(
        const std::vector<llm::ChatMessage>& messages,
        int last_usage_tokens,
        std::size_t last_usage_message_count) {
    ContextUsageEstimate estimate;
    if (last_usage_tokens > 0 && last_usage_message_count <= messages.size()) {
        estimate.has_usage = true;
        estimate.usage_tokens = last_usage_tokens;
        for (std::size_t i = last_usage_message_count; i < messages.size(); ++i) {
            estimate.trailing_tokens += EstimateTokens(messages[i]);
        }
    } else {
        for (const auto& message : messages) {
            estimate.trailing_tokens += EstimateTokens(message);
        }
    }
    estimate.tokens = estimate.usage_tokens + estimate.trailing_tokens;
    return estimate;
}

bool ShouldCompact(int context_tokens, const CompactionSettings& settings) {
    if (!settings.enabled || settings.context_window <= 0) {
        return false;
    }
    return context_tokens > settings.context_window - settings.reserve_tokens;
}

}  // namespace aiagent::agent
