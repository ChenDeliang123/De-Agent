#pragma once

#include <functional>
#include <string>
#include <vector>

#include "aiagent/config.hpp"
#include "aiagent/llm/deepseek_types.hpp"

namespace aiagent::llm {

// 模型网关抽象：Agent Runtime 依赖接口，便于用 fake 模型做测试。
class IModelClient {
public:
    virtual ~IModelClient() = default;

    virtual ChatResult Chat(const std::vector<ChatMessage>& messages,
                            const std::vector<ToolSpec>& tools = {},
                            const std::optional<std::string>& tool_choice = std::nullopt) = 0;

    virtual void ChatStream(const std::vector<ChatMessage>& messages,
                            const std::vector<ToolSpec>& tools,
                            const std::function<void(const StreamEvent&)>& on_event) = 0;
};

class DeepSeekClient : public IModelClient {
public:
    explicit DeepSeekClient(DeepSeekConfig config);

    // 非流式对话。工具由模型通过 tool_calls 返回，本方法不做工具执行。
    ChatResult Chat(const std::vector<ChatMessage>& messages,
                    const std::vector<ToolSpec>& tools = {},
                    const std::optional<std::string>& tool_choice = std::nullopt) override;

    // 流式对话：文本/思考/结束事件实时回调；工具调用参数会跨 chunk 聚合，
    // 在 finish_reason == "tool_calls" 时一次性给出完整调用列表。
    void ChatStream(const std::vector<ChatMessage>& messages,
                    const std::vector<ToolSpec>& tools,
                    const std::function<void(const StreamEvent&)>& on_event) override;

private:
    std::string RequestBody(const std::vector<ChatMessage>& messages,
                            const std::vector<ToolSpec>& tools,
                            bool stream,
                            const std::optional<std::string>& tool_choice) const;

    DeepSeekConfig config_;
};

}  // namespace aiagent::llm
