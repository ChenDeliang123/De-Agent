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

}  // namespace aiagent::agent
