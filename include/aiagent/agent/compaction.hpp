#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "aiagent/llm/deepseek_client.hpp"
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

// 摘要消息的固定前缀，用于回放/测试识别已压缩上下文。
extern const char* const kCompactionSummaryMarker;

// 安全切点：从尾部向前累计 token，累计到 keep_recent_tokens 后在合法位置切分。
// 只允许切在 user / assistant 上，绝不切在 tool / system，保证 tool_calls 与其
// tool 结果成对保留、协议合法。
struct CutPointResult {
    std::size_t first_kept_index = 0;  // 从此索引（含）开始保留
    bool valid = false;                // 是否存在可压缩的切点
};

CutPointResult FindCutPoint(const std::vector<llm::ChatMessage>& messages,
                            int keep_recent_tokens);

// 把消息序列化成文本，供摘要模型阅读（避免它把内容当作对话继续）。
std::string SerializeConversation(const std::vector<llm::ChatMessage>& messages);

// 调用模型生成结构化摘要（不带工具）。失败抛 std::runtime_error。
// previous_summary 非空时使用 UPDATE 模板，保留旧信息并合并新进展。
std::string GenerateSummary(llm::IModelClient& model,
                            const std::vector<llm::ChatMessage>& to_summarize,
                            const std::string& previous_summary = {});

// 检测 DeepSeek 返回的错误信息是否为上下文超长。
bool IsContextOverflowMessage(const std::string& message);

// 检测 finish_reason 是否为可恢复的长度截断。
bool IsRecoverableLengthStop(const std::string& finish_reason);

// 从 tool_calls（read/write/edit 的 path 参数）以及摘要消息中已有的
// <read-files>/<modified-files> 标记里提取文件操作，便于压缩后仍记得
// 动过哪些文件；再次压缩时可累积传递。
struct FileOperations {
    std::set<std::string> read;
    std::set<std::string> written;
    std::set<std::string> edited;
};

FileOperations ExtractFileOperations(const std::vector<llm::ChatMessage>& messages);

// 把文件操作格式化为 <read-files>/<modified-files> 文本（无内容时返回空串）。
// read-files 会排除已被修改的文件。
std::string FormatFileOperations(const FileOperations& operations);

// 压缩结果。compacted=false 时 messages 为空，调用方保持原消息列表。
struct CompactionOutcome {
    bool compacted = false;
    int tokens_before = 0;
    int tokens_after = 0;
    std::vector<llm::ChatMessage> messages;  // 压缩后的完整消息列表
};

// 一次压缩的记录（供回调/日志/审计）。
struct CompactionRecord {
    bool forced = false;  // overflow/length 触发的强制压缩
    int tokens_before = 0;
    int tokens_after = 0;
    std::size_t messages_before = 0;
    std::size_t messages_after = 0;
};

// 执行一次压缩：摘要 [1, cut) 的消息，保留 system 与 [cut, end)，
// 并把摘要作为一条 user 消息插在 system 之后。
CompactionOutcome CompactMessages(llm::IModelClient& model,
                                  const std::vector<llm::ChatMessage>& messages,
                                  const CompactionSettings& settings);

}  // namespace aiagent::agent
