#pragma once

#include <cstddef>
#include <vector>

#include "aiagent/llm/deepseek_types.hpp"

namespace aiagent::agent {

// 上下文压缩（compaction）配置。对齐 Pi 的 CompactionSettings：
//   - reserve_tokens：给模型输出与摘要预留的空间；
//   - keep_recent_tokens：压缩后保留的最近 token 数。
// 当 context_tokens > context_window - reserve_tokens 时触发压缩。
struct CompactionSettings {
    bool enabled = true;
    int context_window = 64000;
    int reserve_tokens = 16384;
    int keep_recent_tokens = 20000;
};

// 上下文 token 估算结果。
//   - usage_tokens：最近一次模型响应的真实用量；
//   - trailing_tokens：该次响应之后新增消息的估算值；
//   - tokens = usage_tokens + trailing_tokens。
struct ContextUsageEstimate {
    int tokens = 0;
    int usage_tokens = 0;
    int trailing_tokens = 0;
    bool has_usage = false;
};

// 单条消息的 token 估算。按 UTF-8 字节数 / 3 估算（对中文比 chars/4 更保守），
// 再叠加固定的角色/分隔符开销。
int EstimateTokens(const llm::ChatMessage& message);

// 整个消息列表的粗略 token 估算（不依赖真实 usage）。
int EstimateMessagesTokens(const std::vector<llm::ChatMessage>& messages);

// 优先使用最近一次真实 usage，其后的尾部消息再估算相加。
// last_usage_message_count 为产生该 usage 的请求所包含的消息条数。
ContextUsageEstimate EstimateContextTokens(
        const std::vector<llm::ChatMessage>& messages,
        int last_usage_tokens,
        std::size_t last_usage_message_count);

// 是否越过压缩阈值。
bool ShouldCompact(int context_tokens, const CompactionSettings& settings);

}  // namespace aiagent::agent
