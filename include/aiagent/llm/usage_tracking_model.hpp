#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "aiagent/llm/deepseek_client.hpp"
#include "aiagent/llm/deepseek_types.hpp"

namespace aiagent::llm {

// 模型客户端装饰器：把下游客户端的 token 用量转发给调用方的 sink。
//
// 用途：子 Agent 的用量归集。子 Agent 用的是独立模型客户端（不与父共享
// IModelClient），用量既不会出现在父的 usage 日志里，也不会自动进会话统计；
// 由 Manager 提供一个 sink，装配方在创建子模型客户端时套上本装饰器即可。
//
// 线程安全性取决于 sink；装饰器自身无可变状态。
class UsageTrackingModel final : public IModelClient {
public:
    using UsageSink = std::function<void(const ChatUsage&)>;

    UsageTrackingModel(std::shared_ptr<IModelClient> inner, UsageSink on_usage)
        : inner_(std::move(inner)), on_usage_(std::move(on_usage)) {}

    ChatResult Chat(const std::vector<ChatMessage>& messages,
                    const std::vector<ToolSpec>& tools = {},
                    const std::optional<std::string>& tool_choice = std::nullopt) override {
        ChatResult result = inner_->Chat(messages, tools, tool_choice);
        if (on_usage_ && result.usage.total_tokens > 0) {
            on_usage_(result.usage);
        }
        return result;
    }

    void ChatStream(const std::vector<ChatMessage>& messages,
                    const std::vector<ToolSpec>& tools,
                    const std::function<void(const StreamEvent&)>& on_event) override {
        inner_->ChatStream(messages, tools, [this, &on_event](const StreamEvent& event) {
            if (on_usage_ && event.usage.total_tokens > 0) {
                on_usage_(event.usage);
            }
            if (on_event) {
                on_event(event);
            }
        });
    }

private:
    std::shared_ptr<IModelClient> inner_;
    UsageSink on_usage_;
};

}  // namespace aiagent::llm
