#pragma once

#include <json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace aiagent::llm {

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;  // 原始 JSON 字符串，方便增量拼接与回传
    int index = 0;          // 流式工具调用 delta 中的序号
    bool has_index = false; // 非流式响应中不存在该字段
};

struct ChatMessage {
    std::string role;                 // system / user / assistant / tool
    std::string content;
    std::string reasoning_content;    // DeepSeek 思考模型的推理文本
    std::string tool_call_id;         // role == tool 时使用
    std::string name;                 // 可选
    std::vector<ToolCall> tool_calls; // assistant 携带工具调用时使用
};

struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json parameters = nlohmann::json::object();
};

struct ChatUsage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    // DeepSeek 的自动前缀缓存命中量（未命中则 0）。
    // 用途：观测"压缩是否把缓存打掉"，以及长上下文的真实成本。
    int prompt_cache_hit_tokens = 0;
};

struct ChatChoice {
    std::string role;
    std::string content;
    std::string reasoning_content;
    std::vector<ToolCall> tool_calls;
    std::string finish_reason;  // stop / tool_calls / length ...
};

struct ChatResult {
    std::string id;
    std::vector<ChatChoice> choices;
    ChatUsage usage;
};

// 单个 SSE data 帧解析后的增量。
struct StreamChunk {
    std::string id;
    std::string role;
    std::string content;
    std::string reasoning_content;
    std::vector<ToolCall> tool_calls;
    std::string finish_reason;
    // 仅在请求开启 stream_options.include_usage 时，最后一帧携带。
    ChatUsage usage;
};

// ChatStream 对外回调中使用的高层事件。
struct StreamEvent {
    std::string role;
    std::string content;
    std::string reasoning_content;
    std::string finish_reason;
    std::vector<ToolCall> tool_calls;
    // 仅在开启 include_usage 时，流结束时由 usage 帧填充。
    ChatUsage usage;
};

class DeepSeekException : public std::runtime_error {
public:
    DeepSeekException(int status_code, const std::string& message)
        : std::runtime_error(message), status_code_(status_code) {}

    int status_code() const noexcept { return status_code_; }

private:
    int status_code_ = 0;
};

// 构造 Chat Completions 请求体（不含 api_key）。
std::string BuildChatRequestBody(const std::string& model,
                                 const std::vector<ChatMessage>& messages,
                                 const std::vector<ToolSpec>& tools,
                                 bool stream,
                                 const std::optional<std::string>& tool_choice = std::nullopt);

// 解析非流式 Chat Completions 响应。
ChatResult ParseChatResponse(const std::string& body);

// 解析单个流式 SSE data 帧。
StreamChunk ParseStreamChunk(const std::string& data);

// 从错误响应体中提取用户可读信息，失败时返回空字符串。
std::string DeepSeekErrorMessage(const std::string& body);

}  // namespace aiagent::llm
