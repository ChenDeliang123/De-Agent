#include "aiagent/agent/agent_runtime.hpp"

#include "a2a/core/id.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

namespace aiagent::agent {
namespace {

std::string FirstText(const a2a::models::Message& message) {
    for (const auto& part : message.parts) {
        if (part.text.has_value()) {
            return *part.text;
        }
    }
    return {};
}

std::string RoleToLlmRole(a2a::models::Role role) {
    switch (role) {
        case a2a::models::Role::kUser:
            return "user";
        case a2a::models::Role::kAgent:
            return "assistant";
        default:
            return {};
    }
}

}  // namespace

AgentRuntime::AgentRuntime(std::shared_ptr<llm::IModelClient> model, std::string system_prompt)
    : model_(std::move(model)), system_prompt_(std::move(system_prompt)) {
    if (!model_) {
        throw std::invalid_argument("AgentRuntime requires a model client");
    }
}

void AgentRuntime::SetToolProvider(std::shared_ptr<tools::IToolProvider> tools) {
    tools_ = std::move(tools);
}

a2a::models::SendMessageResponse AgentRuntime::HandleMessage(
        const a2a::models::Message& incoming,
        const std::vector<a2a::models::Message>& history,
        const std::string& context_id) {
    std::vector<llm::ChatMessage> chat_messages;

    llm::ChatMessage system;
    system.role = "system";
    system.content = system_prompt_;
    chat_messages.push_back(std::move(system));

    bool has_user_message = false;
    for (const auto& message : history) {
        const std::string role = RoleToLlmRole(message.role);
        const std::string text = FirstText(message);
        if (role.empty() || text.empty()) {
            continue;
        }
        llm::ChatMessage chat_message;
        chat_message.role = role;
        chat_message.content = text;
        chat_messages.push_back(std::move(chat_message));
        has_user_message = has_user_message || role == "user";
    }
    if (!has_user_message) {
        const std::string text = FirstText(incoming);
        llm::ChatMessage user;
        user.role = "user";
        user.content = text;
        chat_messages.push_back(std::move(user));
    }

    constexpr int kMaxToolRounds = 8;
    llm::ChatResult result;
    for (int round = 0; round < kMaxToolRounds; ++round) {
        const std::vector<llm::ToolSpec> tool_specs = tools_ ? tools_->ListTools() : std::vector<llm::ToolSpec>();
        result = model_->Chat(chat_messages, tool_specs);
        if (result.choices.empty()) {
            throw std::runtime_error("model returned no choices");
        }

        const auto& choice = result.choices.front();
        if (choice.tool_calls.empty()) {
            break;
        }
        if (!tools_) {
            throw std::runtime_error("model requested tools but no tool provider is configured");
        }

        llm::ChatMessage assistant;
        assistant.role = "assistant";
        assistant.content = choice.content;
        assistant.reasoning_content = choice.reasoning_content;
        assistant.tool_calls = choice.tool_calls;
        chat_messages.push_back(std::move(assistant));

        for (const auto& call : choice.tool_calls) {
            tools::ToolExecutionResult execution_result;
            try {
                nlohmann::json arguments = nlohmann::json::object();
                if (!call.arguments.empty()) {
                    arguments = nlohmann::json::parse(call.arguments);
                }
                execution_result = tools_->Execute(call.name, arguments);
            } catch (const std::exception& e) {
                execution_result.ok = false;
                execution_result.error = e.what();
            }

            llm::ChatMessage tool_result;
            tool_result.role = "tool";
            tool_result.tool_call_id = call.id;
            tool_result.content = execution_result.ok
                                          ? execution_result.text
                                          : std::string("tool error: ") + execution_result.error;
            chat_messages.push_back(std::move(tool_result));
        }
    }

    if (result.choices.empty()) {
        throw std::runtime_error("model returned no choices");
    }
    const std::string& reply_text = result.choices.front().content;
    if (reply_text.empty()) {
        throw std::runtime_error("model returned an empty reply");
    }

    a2a::models::Message reply =
            a2a::models::MakeTextMessage(a2a::models::Role::kAgent, reply_text, "text/plain");

    a2a::models::Task task;
    task.id = incoming.task_id.empty() ? std::string(a2a::core::NewUuid()) : incoming.task_id;
    task.context_id = context_id;
    task.status = a2a::models::MakeTaskStatus(a2a::models::TaskState::kCompleted);
    task.history = history;
    task.history.push_back(std::move(reply));

    a2a::models::SendMessageResponse response;
    response.task = std::move(task);
    return response;
}

std::string AgentRuntime::RunStreaming(
        const a2a::models::Message& incoming,
        const std::vector<a2a::models::Message>& history,
        const std::string& context_id,
        const AgentEventCallback& on_event,
        const std::string& working_directory) {
    (void)context_id;

    std::vector<llm::ChatMessage> chat_messages;
    llm::ChatMessage system;
    system.role = "system";
    system.content = system_prompt_;
    if (!working_directory.empty()) {
        system.content += "\n\nCurrent working directory: " + working_directory;
    }
    chat_messages.push_back(std::move(system));

    bool has_user_message = false;
    for (const auto& message : history) {
        const std::string role = RoleToLlmRole(message.role);
        const std::string text = FirstText(message);
        if (role.empty() || text.empty()) {
            continue;
        }
        llm::ChatMessage chat_message;
        chat_message.role = role;
        chat_message.content = text;
        chat_messages.push_back(std::move(chat_message));
        has_user_message = has_user_message || role == "user";
    }
    if (!has_user_message) {
        llm::ChatMessage user;
        user.role = "user";
        user.content = FirstText(incoming);
        chat_messages.push_back(std::move(user));
    }

    constexpr int kMaxToolRounds = 8;
    std::string reply;
    for (int round = 0; round < kMaxToolRounds; ++round) {
        const std::vector<llm::ToolSpec> tool_specs =
                tools_ ? tools_->ListTools() : std::vector<llm::ToolSpec>();
        std::vector<llm::ToolCall> streamed_tool_calls;

        model_->ChatStream(
                chat_messages,
                tool_specs,
                [&](const llm::StreamEvent& event) {
                    if (!event.content.empty()) {
                        reply += event.content;
                        if (on_event) {
                            AgentEvent agent_event;
                            agent_event.kind = AgentEventKind::kToken;
                            agent_event.content = event.content;
                            on_event(agent_event);
                        }
                    }
                    if (!event.tool_calls.empty()) {
                        streamed_tool_calls = event.tool_calls;
                    }
                });

        if (streamed_tool_calls.empty()) {
            break;
        }
        if (!tools_) {
            throw std::runtime_error("model requested tools but no tool provider is configured");
        }

        llm::ChatMessage assistant;
        assistant.role = "assistant";
        assistant.content = "";
        assistant.tool_calls = streamed_tool_calls;
        chat_messages.push_back(std::move(assistant));

        for (const auto& call : streamed_tool_calls) {
            if (on_event) {
                AgentEvent agent_event;
                agent_event.kind = AgentEventKind::kToolStart;
                agent_event.tool_call_id = call.id;
                agent_event.tool_name = call.name;
                agent_event.arguments = call.arguments;
                on_event(agent_event);
            }

            tools::ToolExecutionResult execution_result;
            try {
                nlohmann::json arguments = nlohmann::json::object();
                if (!call.arguments.empty()) {
                    arguments = nlohmann::json::parse(call.arguments);
                }
                execution_result =
                        tools_->Execute(call.name, arguments, working_directory);
            } catch (const std::exception& e) {
                execution_result.ok = false;
                execution_result.error = e.what();
            }

            if (on_event) {
                AgentEvent agent_event;
                agent_event.kind = AgentEventKind::kToolEnd;
                agent_event.tool_call_id = call.id;
                agent_event.ok = execution_result.ok;
                agent_event.text = execution_result.ok
                                           ? execution_result.text
                                           : std::string("tool error: ") + execution_result.error;
                on_event(agent_event);
            }

            llm::ChatMessage tool_result;
            tool_result.role = "tool";
            tool_result.tool_call_id = call.id;
            tool_result.content = execution_result.ok
                                          ? execution_result.text
                                          : std::string("tool error: ") + execution_result.error;
            chat_messages.push_back(std::move(tool_result));
        }
    }

    if (reply.empty()) {
        throw std::runtime_error("model returned an empty reply");
    }
    return reply;
}

std::string AgentRuntime::HandleMessageStreaming(
        const a2a::models::Message& incoming,
        const std::vector<a2a::models::Message>& history,
        const std::string& context_id,
        const std::function<void(const std::string& token)>& on_token) {
    return RunStreaming(incoming, history, context_id,
                        [&on_token](const AgentEvent& event) {
                            if (event.kind == AgentEventKind::kToken && on_token) {
                                on_token(event.content);
                            }
                        },
                        {});
}

std::string AgentRuntime::HandleMessageStreamingEvents(
        const a2a::models::Message& incoming,
        const std::vector<a2a::models::Message>& history,
        const std::string& context_id,
        const AgentEventCallback& on_event,
        const std::string& working_directory) {
    return RunStreaming(incoming, history, context_id, on_event, working_directory);
}

}  // namespace aiagent::agent
