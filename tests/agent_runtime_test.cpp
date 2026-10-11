#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "a2a/core/version.hpp"
#include "a2a/models/agent_card.hpp"
#include "a2a/models/message.hpp"
#include "a2a/models/requests.hpp"
#include "a2a/models/task.hpp"
#include "a2a/server/agent_server.hpp"

#include "aiagent/agent/agent_runtime.hpp"
#include "aiagent/llm/deepseek_client.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

class FakeModel final : public aiagent::llm::IModelClient {
public:
    std::string last_user_content;
    int call_count = 0;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>& messages,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        ++call_count;
        for (const auto& message : messages) {
            if (message.role == "user") {
                last_user_content = message.content;
            }
        }

        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "fake-reply-to-" + last_user_content;
        choice.finish_reason = "stop";

        aiagent::llm::ChatResult result;
        result.id = "fake-chat-id";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        aiagent::llm::StreamEvent event;
        event.content = "fake-stream";
        if (on_event) {
            on_event(event);
        }
    }
};

class StreamingFakeModel final : public aiagent::llm::IModelClient {
public:
    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "unused";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        aiagent::llm::StreamEvent first;
        first.content = "hel";
        on_event(first);
        aiagent::llm::StreamEvent second;
        second.content = "lo";
        on_event(second);
    }
};

class AlwaysToolModel final : public aiagent::llm::IModelClient {
public:
    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.tool_calls.push_back(
                aiagent::llm::ToolCall{"call-tool", "ok_tool", "{}"});
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        aiagent::llm::StreamEvent event;
        event.finish_reason = "tool_calls";
        event.tool_calls.push_back(
                aiagent::llm::ToolCall{"call-tool", "ok_tool", "{}"});
        if (on_event) {
            on_event(event);
        }
    }
};

class FiniteToolModel final : public aiagent::llm::IModelClient {
public:
    int calls = 0;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "finished-after-tools";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        ++calls;
        if (calls <= 5) {
            aiagent::llm::StreamEvent event;
            event.finish_reason = "tool_calls";
            event.tool_calls.push_back(
                    aiagent::llm::ToolCall{"call-tool", "ok_tool", "{}"});
            on_event(event);
            return;
        }
        aiagent::llm::StreamEvent event;
        event.content = "finished-after-tools";
        event.finish_reason = "stop";
        on_event(event);
    }
};

class OkToolProvider final : public aiagent::tools::IToolProvider {
public:
    std::vector<aiagent::llm::ToolSpec> ListTools() override {
        aiagent::llm::ToolSpec tool;
        tool.name = "ok_tool";
        return {tool};
    }

    aiagent::tools::ToolExecutionResult Execute(
            const std::string&,
            const nlohmann::json&,
            const aiagent::tools::ToolContext&) override {
        aiagent::tools::ToolExecutionResult result;
        result.ok = true;
        result.text = "ok";
        return result;
    }
};

class BigToolProvider final : public aiagent::tools::IToolProvider {
public:
    std::vector<aiagent::llm::ToolSpec> ListTools() override {
        aiagent::llm::ToolSpec tool;
        tool.name = "big_tool";
        return {tool};
    }

    aiagent::tools::ToolExecutionResult Execute(
            const std::string&,
            const nlohmann::json&,
            const aiagent::tools::ToolContext&) override {
        aiagent::tools::ToolExecutionResult result;
        result.ok = true;
        result.text = std::string(3000, 'x');
        return result;
    }
};

// 记录 read/write/bash 调用次数的 provider（工具名与内置工具一致）。
class RecordingPermissionProvider final : public aiagent::tools::IToolProvider {
public:
    int read_calls = 0;
    int write_calls = 0;
    int bash_calls = 0;

    std::vector<aiagent::llm::ToolSpec> ListTools() override {
        std::vector<aiagent::llm::ToolSpec> tools;
        for (const char* name : {"read", "write", "bash"}) {
            aiagent::llm::ToolSpec spec;
            spec.name = name;
            spec.parameters = nlohmann::json::object();
            tools.push_back(std::move(spec));
        }
        return tools;
    }

    aiagent::tools::ToolExecutionResult Execute(
            const std::string& name,
            const nlohmann::json&,
            const aiagent::tools::ToolContext&) override {
        if (name == "read") {
            ++read_calls;
        } else if (name == "write") {
            ++write_calls;
        } else if (name == "bash") {
            ++bash_calls;
        }
        aiagent::tools::ToolExecutionResult result;
        result.ok = true;
        result.text = "ok";
        return result;
    }
};

// 第一次调用请求指定工具，第二次返回最终回复。
class OneShotToolModel final : public aiagent::llm::IModelClient {
public:
    explicit OneShotToolModel(std::string tool_name, std::string arguments = "{}")
        : tool_name_(std::move(tool_name)), arguments_(std::move(arguments)) {}

    int calls = 0;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        ++calls;
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        if (calls == 1) {
            choice.tool_calls.push_back(
                    aiagent::llm::ToolCall{"call-1", tool_name_, arguments_});
        } else {
            choice.content = "done";
        }
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        ++calls;
        aiagent::llm::StreamEvent event;
        if (calls == 1) {
            event.finish_reason = "tool_calls";
            event.tool_calls.push_back(
                    aiagent::llm::ToolCall{"call-1", tool_name_, arguments_});
        } else {
            event.content = "done";
            event.finish_reason = "stop";
        }
        if (on_event) {
            on_event(event);
        }
    }

private:
    std::string tool_name_;
    std::string arguments_;
};

// 奇数调用请求工具，偶数调用返回最终回复（用于测试会话内“总是允许”缓存）。
class AlternatingToolModel final : public aiagent::llm::IModelClient {
public:
    explicit AlternatingToolModel(std::string tool_name)
        : tool_name_(std::move(tool_name)) {}

    int calls = 0;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        ++calls;
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        if (calls % 2 == 1) {
            choice.tool_calls.push_back(aiagent::llm::ToolCall{
                    "call-" + std::to_string(calls), tool_name_, "{}"});
        } else {
            choice.content = "done";
        }
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        ++calls;
        aiagent::llm::StreamEvent event;
        if (calls % 2 == 1) {
            event.finish_reason = "tool_calls";
            event.tool_calls.push_back(aiagent::llm::ToolCall{
                    "call-" + std::to_string(calls), tool_name_, "{}"});
        } else {
            event.content = "done";
            event.finish_reason = "stop";
        }
        if (on_event) {
            on_event(event);
        }
    }

private:
    std::string tool_name_;
};

void RunPermissionScenario(
        aiagent::permissions::PermissionMode mode,
        const std::string& tool_name,
        const std::shared_ptr<RecordingPermissionProvider>& provider,
        bool interactive = true,
        aiagent::permissions::PermissionMode non_interactive_max =
                aiagent::permissions::PermissionMode::kWorkspace) {
    auto model = std::make_shared<OneShotToolModel>(tool_name);
    aiagent::agent::AgentRuntime runtime(model);
    runtime.SetToolProvider(provider);
    aiagent::agent::PermissionSettings settings;
    settings.default_mode = mode;
    settings.non_interactive_max_mode = non_interactive_max;
    runtime.SetPermissionSettings(settings);
    a2a::models::Message user = a2a::models::MakeTextMessage(
            a2a::models::Role::kUser, "run tool", "text/plain");
    if (interactive) {
        runtime.HandleMessageStreaming(user, {user}, "ctx-permission", nullptr);
    } else {
        runtime.HandleMessage(user, {user}, "ctx-permission");
    }
}

// 第一次流式调用抛出“上下文超长”，压缩后重试成功。
class OverflowOnceModel final : public aiagent::llm::IModelClient {
public:
    int stream_calls = 0;
    int summary_calls = 0;
    bool saw_summary_marker = false;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        ++summary_calls;
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "## Goal\noverflow summary";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(
            const std::vector<aiagent::llm::ChatMessage>& messages,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        ++stream_calls;
        for (const auto& message : messages) {
            if (message.content.find(aiagent::agent::kCompactionSummaryMarker) !=
                std::string::npos) {
                saw_summary_marker = true;
            }
        }
        if (stream_calls == 1) {
            throw aiagent::llm::DeepSeekException(
                    400, "This model's maximum context length is 8192 tokens");
        }
        aiagent::llm::StreamEvent event;
        event.content = "recovered";
        event.finish_reason = "stop";
        if (on_event) {
            on_event(event);
        }
    }
};

// 第一次流式调用以 length 截断结束（带半截输出），压缩后重试成功。
class LengthOnceModel final : public aiagent::llm::IModelClient {
public:
    int stream_calls = 0;
    int summary_calls = 0;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        ++summary_calls;
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "## Goal\nlength summary";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        ++stream_calls;
        aiagent::llm::StreamEvent event;
        if (stream_calls == 1) {
            event.content = "partial answer";
            event.finish_reason = "length";
        } else {
            event.content = "final answer";
            event.finish_reason = "stop";
        }
        if (on_event) {
            on_event(event);
        }
    }
};

// 持续抛出上下文超长：只允许压缩重试一次，之后向上抛错。
class AlwaysOverflowModel final : public aiagent::llm::IModelClient {
public:
    int stream_calls = 0;
    int summary_calls = 0;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        ++summary_calls;
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "## Goal\noverflow summary";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::function<void(const aiagent::llm::StreamEvent&)>&) override {
        ++stream_calls;
        throw aiagent::llm::DeepSeekException(400,
                                              "maximum context length exceeded");
    }
};

// 同步路径：第一次主调用抛超长，摘要调用靠 system 提示词区分，之后成功。
class SyncOverflowOnceModel final : public aiagent::llm::IModelClient {
public:
    int chat_calls = 0;
    int summary_calls = 0;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>& messages,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        if (!messages.empty() &&
            messages.front().content.find("summarization assistant") !=
                    std::string::npos) {
            ++summary_calls;
            aiagent::llm::ChatResult summary;
            aiagent::llm::ChatChoice summary_choice;
            summary_choice.role = "assistant";
            summary_choice.content = "## Goal\nsync summary";
            summary.choices.push_back(std::move(summary_choice));
            return summary;
        }
        ++chat_calls;
        if (chat_calls == 1) {
            throw aiagent::llm::DeepSeekException(
                    400, "This model's maximum context length is 8192 tokens");
        }
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "sync-recovered";
        choice.finish_reason = "stop";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>&) override {
        throw std::runtime_error("SyncOverflowOnceModel::ChatStream should not be called");
    }
};

// 只负责回答摘要请求（无 tools 的 Chat）的 fake 模型。
class SummaryOnlyModel final : public aiagent::llm::IModelClient {
public:
    int summary_calls = 0;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        ++summary_calls;
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "## Goal\nmanual summary";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>&) override {
        throw std::runtime_error("SummaryOnlyModel::ChatStream should not be called");
    }
};

// 前 3 轮请求工具（附带近似真实 usage），之后返回最终答案；
// 摘要调用（无 tools 的 Chat）返回固定摘要。
class CompactionFakeModel final : public aiagent::llm::IModelClient {
public:
    int stream_calls = 0;
    int summary_calls = 0;
    bool saw_summary_marker = false;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        ++summary_calls;
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "## Goal\nfake summary";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(
            const std::vector<aiagent::llm::ChatMessage>& messages,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        ++stream_calls;
        int approx_tokens = 0;
        for (const auto& message : messages) {
            if (message.content.find(aiagent::agent::kCompactionSummaryMarker) !=
                std::string::npos) {
                saw_summary_marker = true;
            }
            std::size_t bytes = message.content.size() +
                                message.reasoning_content.size() +
                                message.tool_call_id.size();
            for (const auto& call : message.tool_calls) {
                bytes += call.name.size() + call.arguments.size();
            }
            approx_tokens += static_cast<int>((bytes + 2) / 3) + 4;
        }

        aiagent::llm::StreamEvent event;
        event.usage.prompt_tokens = approx_tokens;
        event.usage.total_tokens = approx_tokens;
        if (stream_calls <= 3) {
            event.finish_reason = "tool_calls";
            event.tool_calls.push_back(
                    aiagent::llm::ToolCall{"call-big", "big_tool", "{}"});
        } else {
            event.content = "done-after-compaction";
            event.finish_reason = "stop";
        }
        if (on_event) {
            on_event(event);
        }
    }
};

// 只报「大 completion + 小 prompt」的模型：验证触发口径只看 prompt。
// 旧实现用 total_tokens，会被一次大写入顶过阈值而提前压缩。
class CompletionHeavyModel final : public aiagent::llm::IModelClient {
public:
    int stream_calls = 0;
    int summary_calls = 0;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        ++summary_calls;
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "## Goal\nshould not be used";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        ++stream_calls;
        aiagent::llm::StreamEvent event;
        event.usage.prompt_tokens = 200;
        event.usage.completion_tokens = 20000;
        event.usage.total_tokens = 20200;
        if (stream_calls == 1) {
            event.finish_reason = "tool_calls";
            event.tool_calls.push_back(
                    aiagent::llm::ToolCall{"call-ok", "ok_tool", "{}"});
        } else {
            event.content = "done-without-compaction";
            event.finish_reason = "stop";
        }
        if (on_event) {
            on_event(event);
        }
    }
};

// 摘要比原文还长的模型：验证收益下限（压缩后没有明显变小就放弃，且不重复尝试）。
class LowGainSummaryModel final : public aiagent::llm::IModelClient {
public:
    int stream_calls = 0;
    int summary_calls = 0;
    bool saw_summary_marker = false;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>&,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        ++summary_calls;
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        // 故意比被摘要的内容还长：tokens_after 不会低于 before 的 70%。
        choice.content = "## Goal\n" + std::string(40000, 'x');
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(
            const std::vector<aiagent::llm::ChatMessage>& messages,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        ++stream_calls;
        int approx_tokens = 0;
        for (const auto& message : messages) {
            if (message.content.find(aiagent::agent::kCompactionSummaryMarker) !=
                std::string::npos) {
                saw_summary_marker = true;
            }
            std::size_t bytes = message.content.size() +
                                message.reasoning_content.size() +
                                message.tool_call_id.size();
            for (const auto& call : message.tool_calls) {
                bytes += call.name.size() + call.arguments.size();
            }
            approx_tokens += static_cast<int>((bytes + 2) / 3) + 4;
        }
        aiagent::llm::StreamEvent event;
        event.usage.prompt_tokens = approx_tokens;
        event.usage.total_tokens = approx_tokens;
        if (stream_calls <= 4) {
            event.finish_reason = "tool_calls";
            event.tool_calls.push_back(
                    aiagent::llm::ToolCall{"call-ok", "ok_tool", "{}"});
        } else {
            event.content = "done-after-low-gain";
            event.finish_reason = "stop";
        }
        if (on_event) {
            on_event(event);
        }
    }
};

a2a::models::AgentCard BuildCard() {
    using a2a::models::AgentCard;

    AgentCard card("aiagent-runtime-test",
                   "Test agent wired to a fake model.",
                   "0.1.0");
    card.WithDefaultInputModes({"text/plain"})
            .WithDefaultOutputModes({"text/plain"});
    return card;
}

std::string FirstText(const a2a::models::Message& message) {
    for (const auto& part : message.parts) {
        if (part.text.has_value()) {
            return *part.text;
        }
    }
    return {};
}

a2a::models::SendMessageRequest MakeRequest(const std::string& text,
                                            const std::string& task_id,
                                            const std::string& context_id) {
    a2a::models::SendMessageRequest request;
    request.message = a2a::models::MakeTextMessage(
            a2a::models::Role::kUser, text, "text/plain");
    request.message.task_id = task_id;
    request.message.context_id = context_id;
    return request;
}

std::vector<a2a::models::Message> HistoryByContext(
        const std::shared_ptr<a2a::server::ITaskStore>& store,
        const std::string& context_id) {
    std::vector<a2a::models::Message> history;
    if (context_id.empty()) {
        return history;
    }
    std::size_t latest_history_size = 0;
    for (const auto& task : store->ListAll()) {
        if (task.context_id == context_id &&
            task.history.size() > latest_history_size) {
            history = task.history;
            latest_history_size = task.history.size();
        }
    }
    return history;
}

std::vector<a2a::models::Message> MultiTurnHistory() {
    std::vector<a2a::models::Message> history;
    history.push_back(a2a::models::MakeTextMessage(
            a2a::models::Role::kUser, "first request", "text/plain"));
    history.push_back(a2a::models::MakeTextMessage(
            a2a::models::Role::kAgent, "first answer", "text/plain"));
    history.push_back(a2a::models::MakeTextMessage(
            a2a::models::Role::kUser, "second request", "text/plain"));
    return history;
}

}  // namespace

int main() {
    try {
        auto model = std::make_shared<FakeModel>();
        aiagent::agent::AgentRuntime runtime(model);
        a2a::server::AgentServer agent_server(BuildCard());

        agent_server.task_manager().SetMessageHandler(
                [&](const a2a::models::SendMessageRequest& request) {
                    const std::string context_id = request.message.context_id;
                    auto history =
                            HistoryByContext(agent_server.task_manager().task_store(),
                                             context_id);
                    history.push_back(request.message);
                    return runtime.HandleMessage(request.message, history, context_id);
                });

        const auto first_response =
                agent_server.task_manager().SendMessage(MakeRequest("hello", "", "ctx-1"));
        Expect(first_response.IsTask(), "first response should contain a task");
        const std::string task_id = first_response.GetTask().id;
        Expect(!task_id.empty(), "first task should have an id");
        Expect(first_response.GetTask().history.size() == 2,
               "first task history should include user and assistant messages");

        const auto second_response =
                agent_server.task_manager().SendMessage(MakeRequest("again", "", "ctx-1"));
        Expect(second_response.IsTask(), "second response should contain a task");
        Expect(model->call_count == 2, "fake model should be called twice");
        Expect(model->last_user_content == "again",
               "second model call should see the latest user message");

        auto history = HistoryByContext(agent_server.task_manager().task_store(), "ctx-1");
        Expect(history.size() == 4, "persisted history should contain two turns");
        Expect(FirstText(history.back()) == "fake-reply-to-again",
               "last history message should be the fake assistant reply");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime test failed: " << e.what() << '\n';
        return 1;
    }

    try {
        auto model = std::make_shared<StreamingFakeModel>();
        aiagent::agent::AgentRuntime runtime(model);

        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "stream", "text/plain");
        std::string streamed_tokens;
        const std::string reply = runtime.HandleMessageStreaming(
                user, {user}, "ctx-stream", [&](const std::string& token) {
                    streamed_tokens += token;
                });
        Expect(streamed_tokens == "hello", "streaming callback should receive tokens");
        Expect(reply == "hello", "streaming method should return the assembled reply");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime streaming test failed: " << e.what() << '\n';
        return 1;
    }

    try {
        auto model = std::make_shared<AlwaysToolModel>();
        auto tools = std::make_shared<OkToolProvider>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetToolProvider(tools);
        runtime.SetMaxToolRounds(3);

        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "long task", "text/plain");
        bool reached_limit = false;
        try {
            runtime.HandleMessageStreaming(user, {user}, "ctx-limit", nullptr);
        } catch (const std::exception& e) {
            reached_limit = std::string(e.what()).find("max tool rounds") !=
                            std::string::npos;
        }
        Expect(reached_limit,
               "runtime should report reaching max tool rounds instead of silent stop");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime max round test failed: " << e.what() << '\n';
        return 1;
    }

    try {
        auto model = std::make_shared<FiniteToolModel>();
        auto tools = std::make_shared<OkToolProvider>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetToolProvider(tools);

        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "finite long task", "text/plain");
        const std::string reply =
                runtime.HandleMessageStreaming(user, {user}, "ctx-finite", nullptr);
        Expect(reply == "finished-after-tools",
               "default runtime should continue past the old 8-round limit");
        Expect(model->calls == 6, "runtime should finish when the model naturally stops");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime unlimited rounds test failed: " << e.what() << '\n';
        return 1;
    }

    try {
        auto model = std::make_shared<AlwaysToolModel>();
        auto tools = std::make_shared<OkToolProvider>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetToolProvider(tools);

        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "cancel me", "text/plain");
        bool cancelled = false;
        try {
            runtime.HandleMessageStreamingEvents(
                    user, {user}, "ctx-cancel", nullptr, {}, [] { return true; });
        } catch (const std::exception& e) {
            cancelled = std::string(e.what()).find("task interrupted") !=
                        std::string::npos;
        }
        Expect(cancelled, "cancel callback should interrupt the agent runtime");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime cancel test failed: " << e.what() << '\n';
        return 1;
    }

    try {
        auto model = std::make_shared<CompactionFakeModel>();
        auto tools = std::make_shared<BigToolProvider>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetToolProvider(tools);
        aiagent::agent::CompactionSettings settings;
        settings.enabled = true;
        settings.context_window = 2500;
        settings.reserve_tokens = 200;
        settings.keep_recent_tokens = 1500;
        runtime.SetCompactionSettings(settings);

        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "big task", "text/plain");
        const std::string reply = runtime.HandleMessageStreaming(
                user, {user}, "ctx-compact", nullptr);
        Expect(reply == "done-after-compaction",
               "runtime should finish normally after compaction");
        Expect(model->summary_calls == 1,
               "runtime should compact exactly once when the threshold is crossed");
        Expect(model->saw_summary_marker,
               "post-compaction request should include the injected summary");
        Expect(model->stream_calls == 4,
               "compaction should not create extra model turns");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime compaction test failed: " << e.what() << '\n';
        return 1;
    }

    try {
        auto model = std::make_shared<OverflowOnceModel>();
        aiagent::agent::AgentRuntime runtime(model);
        aiagent::agent::CompactionSettings settings;
        settings.enabled = true;
        settings.context_window = 1000000;
        settings.reserve_tokens = 1000;
        settings.keep_recent_tokens = 1;
        // 收益下限拉到 1.0（任何压缩都不达标）：强制压缩必须绕过它，否则防溢出闸门失效。
        settings.min_gain_ratio = 1.0;
        runtime.SetCompactionSettings(settings);

        auto history = MultiTurnHistory();
        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "latest question", "text/plain");
        history.push_back(user);
        const std::string reply = runtime.HandleMessageStreaming(
                user, history, "ctx-overflow", nullptr);
        Expect(reply == "recovered",
               "context overflow should be recovered even with an impossible min_gain_ratio");
        Expect(model->stream_calls == 2, "overflow recovery should retry once");
        Expect(model->summary_calls == 1, "overflow recovery should compact once");
        Expect(model->saw_summary_marker,
               "overflow retry should see the injected summary");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime overflow recovery test failed: " << e.what()
                  << '\n';
        return 1;
    }

    try {
        // 触发口径：只看 prompt_tokens，不被上一轮的大 completion 顶过阈值。
        auto model = std::make_shared<CompletionHeavyModel>();
        auto tools = std::make_shared<OkToolProvider>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetToolProvider(tools);
        aiagent::agent::CompactionSettings settings;
        settings.enabled = true;
        settings.context_window = 10000;
        settings.reserve_tokens = 1000;  // 阈值 9000，远大于 prompt_tokens=200
        settings.keep_recent_tokens = 1000;
        runtime.SetCompactionSettings(settings);

        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "small task", "text/plain");
        const std::string reply = runtime.HandleMessageStreaming(
                user, {user}, "ctx-usage-basis", nullptr);
        Expect(reply == "done-without-compaction",
               "turn should finish without compaction");
        Expect(model->summary_calls == 0,
               "large completion tokens must not trigger compaction (prompt_tokens is the basis)");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime usage-basis test failed: " << e.what() << '\n';
        return 1;
    }

    try {
        // 收益下限：摘要比原文还长时放弃压缩、保留原历史、且不每轮重试。
        auto model = std::make_shared<LowGainSummaryModel>();
        auto tools = std::make_shared<BigToolProvider>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetToolProvider(tools);
        aiagent::agent::CompactionSettings settings;
        settings.enabled = true;
        settings.context_window = 3000;
        settings.reserve_tokens = 200;  // 阈值 2800
        // 保留量要略大于「单次工具结果」的估算，否则切点会落在尾部 tool 消息上、
        // 找不到合法切点而直接跳过（FindCutPoint 的已知边界，见规划文档）。
        settings.keep_recent_tokens = 1200;
        settings.min_gain_ratio = 0.3;
        runtime.SetCompactionSettings(settings);
        std::vector<aiagent::agent::CompactionRecord> recorded;
        runtime.SetCompactionCallback(
                [&](const std::string&, const aiagent::agent::CompactionRecord& record) {
                    recorded.push_back(record);
                });

        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "low gain task", "text/plain");
        const std::string reply = runtime.HandleMessageStreaming(
                user, {user}, "ctx-low-gain", nullptr);
        Expect(reply == "done-after-low-gain", "turn should still finish");
        Expect(model->summary_calls == 1,
               "a low-gain compaction should be attempted once and then skipped");
        Expect(recorded.empty(), "low-gain compaction must not be reported as applied");
        Expect(!model->saw_summary_marker,
               "history must be kept when the compaction has no real gain");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime low-gain test failed: " << e.what() << '\n';
        return 1;
    }

    try {
        auto model = std::make_shared<LengthOnceModel>();
        aiagent::agent::AgentRuntime runtime(model);
        aiagent::agent::CompactionSettings settings;
        settings.enabled = true;
        settings.context_window = 1000000;
        settings.reserve_tokens = 1000;
        settings.keep_recent_tokens = 1;
        runtime.SetCompactionSettings(settings);

        auto history = MultiTurnHistory();
        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "latest question", "text/plain");
        history.push_back(user);
        const std::string reply = runtime.HandleMessageStreaming(
                user, history, "ctx-length", nullptr);
        Expect(reply == "final answer",
               "length-stop recovery should discard the partial output");
        Expect(model->stream_calls == 2, "length recovery should retry once");
        Expect(model->summary_calls == 1, "length recovery should compact once");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime length recovery test failed: " << e.what()
                  << '\n';
        return 1;
    }

    try {
        auto model = std::make_shared<AlwaysOverflowModel>();
        aiagent::agent::AgentRuntime runtime(model);
        aiagent::agent::CompactionSettings settings;
        settings.enabled = true;
        settings.context_window = 1000000;
        settings.reserve_tokens = 1000;
        settings.keep_recent_tokens = 1;
        runtime.SetCompactionSettings(settings);

        auto history = MultiTurnHistory();
        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "latest question", "text/plain");
        history.push_back(user);
        bool threw = false;
        try {
            runtime.HandleMessageStreaming(user, history, "ctx-overflow-twice", nullptr);
        } catch (const aiagent::llm::DeepSeekException&) {
            threw = true;
        }
        Expect(threw, "persistent overflow should surface an error");
        Expect(model->stream_calls == 2,
               "overflow recovery should only retry once");
        Expect(model->summary_calls == 1,
               "overflow recovery should only compact once");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime overflow limit test failed: " << e.what()
                  << '\n';
        return 1;
    }

    try {
        auto model = std::make_shared<SyncOverflowOnceModel>();
        aiagent::agent::AgentRuntime runtime(model);
        aiagent::agent::CompactionSettings settings;
        settings.enabled = true;
        settings.context_window = 1000000;
        settings.reserve_tokens = 1000;
        settings.keep_recent_tokens = 1;
        runtime.SetCompactionSettings(settings);

        auto history = MultiTurnHistory();
        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "latest question", "text/plain");
        history.push_back(user);
        const auto response =
                runtime.HandleMessage(user, history, "ctx-sync-overflow");
        Expect(response.IsTask(), "sync overflow recovery should return a task");
        Expect(FirstText(response.GetTask().history.back()) == "sync-recovered",
               "sync overflow recovery should return the retried reply");
        Expect(model->chat_calls == 2, "sync overflow recovery should retry once");
        Expect(model->summary_calls == 1, "sync overflow recovery should compact once");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime sync overflow test failed: " << e.what() << '\n';
        return 1;
    }

    try {
        auto model = std::make_shared<SummaryOnlyModel>();
        aiagent::agent::AgentRuntime runtime(model);
        aiagent::agent::CompactionSettings settings;
        settings.enabled = true;
        settings.context_window = 1000000;
        settings.reserve_tokens = 1000;
        settings.keep_recent_tokens = 1;
        runtime.SetCompactionSettings(settings);

        std::vector<aiagent::agent::CompactionRecord> recorded;
        runtime.SetCompactionCallback(
                [&](const std::string& context_id,
                    const aiagent::agent::CompactionRecord& record) {
                    Expect(context_id == "ctx-manual",
                           "compaction callback should receive the context id");
                    recorded.push_back(record);
                });

        auto history = MultiTurnHistory();
        history.push_back(a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "third request", "text/plain"));
        // 追加几轮较长内容，确保压缩后 token 确实下降。
        for (int i = 0; i < 4; ++i) {
            history.push_back(a2a::models::MakeTextMessage(
                    a2a::models::Role::kUser,
                    std::string(300, 'x') + std::to_string(i), "text/plain"));
            history.push_back(a2a::models::MakeTextMessage(
                    a2a::models::Role::kAgent,
                    std::string(300, 'y') + std::to_string(i), "text/plain"));
        }
        std::vector<a2a::models::Message> compacted;
        aiagent::agent::CompactionRecord record;
        std::string error;
        const bool ok =
                runtime.CompactHistory("ctx-manual", history, &compacted, &record, &error);
        Expect(ok, std::string("manual compact should succeed: ") + error);
        Expect(!compacted.empty(), "compacted history should not be empty");
        Expect(compacted.front().role == a2a::models::Role::kUser,
               "compacted history should start with the summary user message");
        Expect(FirstText(compacted.front()).find(
                       aiagent::agent::kCompactionSummaryMarker) != std::string::npos,
               "compacted history should contain the summary marker");
        Expect(record.tokens_before > record.tokens_after,
               "manual compact should reduce estimated tokens");
        Expect(recorded.size() == 1 && recorded.front().forced,
               "manual compact should invoke the callback with a forced record");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime manual compact test failed: " << e.what() << '\n';
        return 1;
    }

    try {
        auto model = std::make_shared<CompactionFakeModel>();
        auto tools = std::make_shared<BigToolProvider>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetToolProvider(tools);
        aiagent::agent::CompactionSettings settings;
        settings.enabled = true;
        settings.context_window = 2500;
        settings.reserve_tokens = 200;
        settings.keep_recent_tokens = 1500;
        runtime.SetCompactionSettings(settings);

        std::vector<aiagent::agent::CompactionRecord> recorded;
        runtime.SetCompactionCallback(
                [&](const std::string&, const aiagent::agent::CompactionRecord& record) {
                    recorded.push_back(record);
                });
        std::vector<aiagent::agent::AgentEvent> compaction_events;

        a2a::models::Message user = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "event task", "text/plain");
        runtime.HandleMessageStreamingEvents(
                user, {user}, "ctx-compaction-event",
                [&](const aiagent::agent::AgentEvent& event) {
                    if (event.kind == aiagent::agent::AgentEventKind::kCompaction) {
                        compaction_events.push_back(event);
                    }
                });
        Expect(!compaction_events.empty(), "compaction event should be emitted");
        Expect(compaction_events.front().compaction_tokens_before >
                       compaction_events.front().compaction_tokens_after,
               "compaction event should report reduced tokens");
        Expect(!compaction_events.front().compaction_forced,
               "threshold compaction should not be marked forced");
        Expect(recorded.size() == static_cast<std::size_t>(model->summary_calls),
               "compaction callback should fire once per compaction");
    } catch (const std::exception& e) {
        std::cerr << "agent runtime compaction event test failed: " << e.what() << '\n';
        return 1;
    }

    try {
        using aiagent::permissions::PermissionMode;
        // confirm：write 与 bash 均需确认，M1 无审批通道 -> 拒绝；read 允许。
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            RunPermissionScenario(PermissionMode::kConfirm, "write", provider);
            Expect(provider->write_calls == 0, "confirm denies write without approval");
        }
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            RunPermissionScenario(PermissionMode::kConfirm, "bash", provider);
            Expect(provider->bash_calls == 0, "confirm denies bash without approval");
        }
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            RunPermissionScenario(PermissionMode::kConfirm, "read", provider);
            Expect(provider->read_calls == 1, "confirm allows read");
        }
        // workspace：write 允许，bash 仍拒绝。
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            RunPermissionScenario(PermissionMode::kWorkspace, "write", provider);
            Expect(provider->write_calls == 1, "workspace allows write");
        }
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            RunPermissionScenario(PermissionMode::kWorkspace, "bash", provider);
            Expect(provider->bash_calls == 0, "workspace denies bash without approval");
        }
        // full：write 与 bash 均允许（交互路径）。
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            RunPermissionScenario(PermissionMode::kFull, "write", provider);
            Expect(provider->write_calls == 1, "full allows write");
        }
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            RunPermissionScenario(PermissionMode::kFull, "bash", provider);
            Expect(provider->bash_calls == 1, "full allows bash");
        }
        // 非交互调用方不会拿到 full：full 被夹到 workspace，bash 仍被拒。
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            RunPermissionScenario(PermissionMode::kFull, "bash", provider,
                                  /*interactive=*/false);
            Expect(provider->bash_calls == 0,
                   "non-interactive caller is capped below full");
        }
        // 非交互 workspace：写允许（不需要确认）。
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            RunPermissionScenario(PermissionMode::kWorkspace, "write", provider,
                                  /*interactive=*/false);
            Expect(provider->write_calls == 1,
                   "non-interactive workspace allows write");
        }
        // 非交互 max confirm：非交互下 workspace 被夹到 confirm，
        // 写需要审批 -> 拒绝。
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            RunPermissionScenario(PermissionMode::kWorkspace, "write", provider,
                                  /*interactive=*/false, PermissionMode::kConfirm);
            Expect(provider->write_calls == 0,
                   "non-interactive max confirm blocks writes");
        }
        // 审批 allow_once：发出 approval_request 事件并执行工具。
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            auto model = std::make_shared<OneShotToolModel>("write");
            aiagent::agent::AgentRuntime runtime(model);
            runtime.SetToolProvider(provider);
            aiagent::agent::PermissionSettings settings;
            settings.default_mode = PermissionMode::kConfirm;
            runtime.SetPermissionSettings(settings);
            int approvals = 0;
            runtime.SetApprovalHandler([&](const aiagent::agent::ApprovalRequest& request) {
                ++approvals;
                Expect(request.tool_name == "write", "approval carries tool name");
                return aiagent::agent::ApprovalDecision::kAllowOnce;
            });
            std::vector<aiagent::agent::AgentEvent> events;
            a2a::models::Message user = a2a::models::MakeTextMessage(
                    a2a::models::Role::kUser, "run tool", "text/plain");
            runtime.HandleMessageStreamingEvents(
                    user, {user}, "ctx-approve-once",
                    [&](const aiagent::agent::AgentEvent& event) {
                        if (event.kind == aiagent::agent::AgentEventKind::kApprovalRequest) {
                            events.push_back(event);
                        }
                    });
            Expect(approvals == 1, "approval handler called once");
            Expect(!events.empty(), "approval_request event emitted");
            Expect(provider->write_calls == 1, "approved write executes");
        }
        // 审批 allow_session：同会话第二次不再询问。
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            auto model = std::make_shared<AlternatingToolModel>("write");
            aiagent::agent::AgentRuntime runtime(model);
            runtime.SetToolProvider(provider);
            aiagent::agent::PermissionSettings settings;
            settings.default_mode = PermissionMode::kConfirm;
            runtime.SetPermissionSettings(settings);
            int approvals = 0;
            runtime.SetApprovalHandler([&](const aiagent::agent::ApprovalRequest&) {
                ++approvals;
                return aiagent::agent::ApprovalDecision::kAllowSession;
            });
            a2a::models::Message user = a2a::models::MakeTextMessage(
                    a2a::models::Role::kUser, "run tool", "text/plain");
            runtime.HandleMessageStreamingEvents(user, {user}, "ctx-allow-session", nullptr);
            runtime.HandleMessageStreamingEvents(user, {user}, "ctx-allow-session", nullptr);
            Expect(approvals == 1, "allow_session is cached for the session");
            Expect(provider->write_calls == 2,
                   "both writes execute after session-wide allow");
        }
        // 审批 deny：工具不执行。
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            auto model = std::make_shared<OneShotToolModel>("write");
            aiagent::agent::AgentRuntime runtime(model);
            runtime.SetToolProvider(provider);
            aiagent::agent::PermissionSettings settings;
            settings.default_mode = PermissionMode::kConfirm;
            runtime.SetPermissionSettings(settings);
            runtime.SetApprovalHandler([&](const aiagent::agent::ApprovalRequest&) {
                return aiagent::agent::ApprovalDecision::kDeny;
            });
            a2a::models::Message user = a2a::models::MakeTextMessage(
                    a2a::models::Role::kUser, "run tool", "text/plain");
            runtime.HandleMessageStreamingEvents(user, {user}, "ctx-deny", nullptr);
            Expect(provider->write_calls == 0, "denied write does not execute");
        }
        // bash 黑名单：deny 命中 -> 不执行（即使 full）。
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            auto model = std::make_shared<OneShotToolModel>(
                    "bash", R"({"command":"sudo reboot"})");
            aiagent::agent::AgentRuntime runtime(model);
            runtime.SetToolProvider(provider);
            aiagent::agent::PermissionSettings settings;
            settings.default_mode = PermissionMode::kFull;
            runtime.SetPermissionSettings(settings);
            runtime.SetBashPolicy(aiagent::permissions::ParseBashPolicy("deny sudo\n"));
            a2a::models::Message user = a2a::models::MakeTextMessage(
                    a2a::models::Role::kUser, "run tool", "text/plain");
            runtime.HandleMessageStreaming(user, {user}, "ctx-bash-deny", nullptr);
            Expect(provider->bash_calls == 0,
                   "bash policy deny blocks the command even in full mode");
        }
        // bash 白名单：allow 命中 -> 免确认（workspace 下 bash 本需确认）。
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            auto model = std::make_shared<OneShotToolModel>(
                    "bash", R"({"command":"git status"})");
            aiagent::agent::AgentRuntime runtime(model);
            runtime.SetToolProvider(provider);
            aiagent::agent::PermissionSettings settings;
            settings.default_mode = PermissionMode::kWorkspace;
            runtime.SetPermissionSettings(settings);
            runtime.SetBashPolicy(
                    aiagent::permissions::ParseBashPolicy("allow git status\n"));
            a2a::models::Message user = a2a::models::MakeTextMessage(
                    a2a::models::Role::kUser, "run tool", "text/plain");
            runtime.HandleMessageStreaming(user, {user}, "ctx-bash-allow", nullptr);
            Expect(provider->bash_calls == 1,
                   "bash policy allow skips the confirmation requirement");
        }
        // 项目级收紧：全局 workspace（写允许），项目 confirm -> 写被拒。
        {
            auto provider = std::make_shared<RecordingPermissionProvider>();
            auto model = std::make_shared<OneShotToolModel>("write");
            aiagent::agent::AgentRuntime runtime(model);
            runtime.SetToolProvider(provider);
            aiagent::agent::PermissionSettings settings;
            settings.default_mode = PermissionMode::kWorkspace;
            runtime.SetPermissionSettings(settings);
            runtime.SetFallbackWorkingDirectory("/tmp/aiagent-project-tighten");
            auto store = std::make_shared<aiagent::permissions::PermissionStore>();
            store->Set("/tmp/aiagent-project-tighten", PermissionMode::kConfirm);
            auto project = std::make_shared<aiagent::permissions::ProjectConfig>();
            project->root = "/tmp/aiagent-project-tighten";
            project->permissions = store;
            project->trusted = false;
            runtime.SetProjectConfigProvider([project](const std::string&) {
                return std::shared_ptr<const aiagent::permissions::ProjectConfig>(
                        project);
            });
            a2a::models::Message user = a2a::models::MakeTextMessage(
                    a2a::models::Role::kUser, "run tool", "text/plain");
            runtime.HandleMessageStreaming(user, {user}, "ctx-project-tighten", nullptr);
            Expect(provider->write_calls == 0,
                   "project config can tighten workspace to confirm (write denied)");
        }
    } catch (const std::exception& e) {
        std::cerr << "agent runtime permission test failed: " << e.what() << '\n';
        return 1;
    }

    if (g_failures != 0) {
        std::cerr << "agent runtime test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "agent runtime test passed\n";
    return 0;
}
