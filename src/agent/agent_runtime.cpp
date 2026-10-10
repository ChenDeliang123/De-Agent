#include "aiagent/agent/agent_runtime.hpp"
#include "aiagent/log.hpp"

#include "a2a/core/id.hpp"

#include <chrono>
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

void AppendPermissionMode(std::string& content, permissions::PermissionMode mode) {
    content += "\n\nCurrent permission mode: ";
    content += permissions::ModeName(mode);
    content += " (";
    content += permissions::DescribePolicy(mode);
    content += ").";
}

void EmitCompactionEvent(const CompactionRecord& record,
                         const AgentEventCallback& on_event) {
    if (!on_event) {
        return;
    }
    AgentEvent event;
    event.kind = AgentEventKind::kCompaction;
    event.compaction_tokens_before = record.tokens_before;
    event.compaction_tokens_after = record.tokens_after;
    event.compaction_forced = record.forced;
    on_event(event);
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

void AgentRuntime::SetMaxToolRounds(int rounds) {
    max_tool_rounds_ = rounds > 0 ? rounds : 0;
}

void AgentRuntime::SetPermissionStore(
        std::shared_ptr<permissions::PermissionStore> store) {
    permission_store_ = std::move(store);
}

void AgentRuntime::SetPermissionSettings(const PermissionSettings& settings) {
    permission_settings_ = settings;
}

permissions::PermissionMode AgentRuntime::ResolvePermissionMode(
        const std::string& working_directory) const {
    permissions::PermissionMode mode = permission_settings_.default_mode;
    const std::string directory =
            working_directory.empty() ? fallback_working_directory_ : working_directory;
    if (permission_store_ != nullptr && !directory.empty()) {
        if (const auto entry = permission_store_->Resolve(directory);
            entry.has_value()) {
            mode = entry->mode;
        }
    }
    // allow_full_access=false 时把 full 夹到 workspace。
    return permissions::ClampMode(mode, permission_settings_.max_mode,
                                  permission_settings_.allow_full_access);
}

tools::ToolContext AgentRuntime::BuildToolContext(
        const std::string& working_directory) const {
    tools::ToolContext context;
    context.working_directory =
            working_directory.empty() ? fallback_working_directory_ : working_directory;
    context.policy =
            permissions::PolicyForMode(ResolvePermissionMode(working_directory));
    return context;
}

std::string AgentRuntime::CheckToolPermission(
        const std::string& tool_name,
        const permissions::PermissionPolicy& policy) const {
    if (tool_name == "read") {
        if (!policy.allow_read) {
            return "permission denied: read is disabled by the current permission mode";
        }
        return {};
    }
    if (tool_name == "write" || tool_name == "edit") {
        if (!policy.allow_write) {
            return "permission denied: file writes are disabled by the current permission mode";
        }
        if (policy.require_confirm_write) {
            return "permission denied: writes require approval, which is not available yet; "
                   "use /permissions workspace or /permissions full to change the mode";
        }
        return {};
    }
    if (tool_name == "bash") {
        if (!policy.allow_command) {
            return "permission denied: command execution is disabled by the current permission mode";
        }
        if (policy.require_confirm_command) {
            return "permission denied: command execution requires approval, which is not "
                   "available yet; use /permissions full to allow commands (this disables "
                   "the path sandbox)";
        }
        return {};
    }
    // 未分类工具（含 MCP）：M1 暂不拦截，保持既有行为；工具分类与
    // “非 full 下需确认”将在 M4 引入。
    return {};
}

void AgentRuntime::SetProjectInstructionsLoader(
        std::shared_ptr<ProjectInstructionsLoader> loader) {
    instructions_loader_ = std::move(loader);
}

void AgentRuntime::SetFallbackWorkingDirectory(std::string directory) {
    fallback_working_directory_ = std::move(directory);
}

llm::ChatMessage AgentRuntime::BuildSystemMessage(
        const std::string& working_directory) const {
    llm::ChatMessage system;
    system.role = "system";
    system.content = system_prompt_;

    if (instructions_loader_ != nullptr) {
        const std::string load_directory =
                working_directory.empty() ? fallback_working_directory_
                                          : working_directory;
        const LoadedInstructions instructions =
                instructions_loader_->Load(load_directory,
                                           fallback_working_directory_);
        if (!instructions.rendered.empty()) {
            system.content += "\n\n" + instructions.rendered;
        }
    }

    if (!working_directory.empty()) {
        system.content += "\n\nCurrent working directory: " + working_directory;
    }
    AppendPermissionMode(system.content, ResolvePermissionMode(working_directory));
    return system;
}

void AgentRuntime::SetCompactionSettings(const CompactionSettings& settings) {
    compaction_settings_ = settings;
}

void AgentRuntime::SetCompactionCallback(CompactionCallback callback) {
    compaction_callback_ = std::move(callback);
}

std::optional<CompactionRecord> AgentRuntime::MaybeCompactBeforeCall(
        std::vector<llm::ChatMessage>& chat_messages,
        int& last_usage_tokens,
        std::size_t& last_usage_message_count,
        std::size_t& last_compacted_size,
        const std::string& run_id,
        const std::string& context_id,
        bool force) {
    if (!force) {
        if (!compaction_settings_.enabled) {
            return std::nullopt;
        }
        const ContextUsageEstimate estimate = EstimateContextTokens(
                chat_messages, last_usage_tokens, last_usage_message_count);
        if (!ShouldCompact(estimate.tokens, compaction_settings_)) {
            return std::nullopt;
        }
        // 同一批消息不重复压缩，避免压缩后仍超阈值时反复调用摘要模型。
        if (chat_messages.size() == last_compacted_size) {
            return std::nullopt;
        }
    }
    try {
        CompactionOutcome outcome =
                CompactMessages(*model_, chat_messages, compaction_settings_);
        if (!outcome.compacted) {
            // 无法找到安全切点时标记已尝试，避免每轮重复计算。
            last_compacted_size = chat_messages.size();
            return std::nullopt;
        }
        CompactionRecord record;
        record.forced = force;
        record.tokens_before = outcome.tokens_before;
        record.tokens_after = outcome.tokens_after;
        record.messages_before = chat_messages.size();
        record.messages_after = outcome.messages.size();

        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kInfo, "agent_runtime",
                "run=" + run_id + " event=compaction" +
                        (force ? std::string(" reason=overflow") : std::string()) +
                        " tokens_before=" + std::to_string(record.tokens_before) +
                        " tokens_after=" + std::to_string(record.tokens_after) +
                        " messages_before=" + std::to_string(record.messages_before) +
                        " messages_after=" +
                        std::to_string(record.messages_after));
        chat_messages = std::move(outcome.messages);
        last_usage_tokens = 0;
        last_usage_message_count = 0;
        last_compacted_size = chat_messages.size();
        if (compaction_callback_) {
            compaction_callback_(context_id, record);
        }
        return record;
    } catch (const std::exception& e) {
        // 摘要失败不影响主流程：保持原消息，记录日志。
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kWarning, "agent_runtime",
                "run=" + run_id + " event=compaction_failed message=" + e.what());
        last_compacted_size = chat_messages.size();
        return std::nullopt;
    }
}

bool AgentRuntime::CompactHistory(
        const std::string& context_id,
        const std::vector<a2a::models::Message>& history,
        std::vector<a2a::models::Message>* compacted_history,
        CompactionRecord* record,
        std::string* error,
        const std::string& working_directory) {
    if (compacted_history == nullptr || record == nullptr) {
        if (error != nullptr) {
            *error = "invalid compact output arguments";
        }
        return false;
    }

    std::vector<llm::ChatMessage> chat_messages;
    chat_messages.push_back(BuildSystemMessage(working_directory));
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
    }

    if (chat_messages.size() <= 2) {
        if (error != nullptr) {
            *error = "nothing to compact (session too small)";
        }
        return false;
    }

    try {
        const CompactionOutcome outcome =
                CompactMessages(*model_, chat_messages, compaction_settings_);
        if (!outcome.compacted) {
            if (error != nullptr) {
                *error = "nothing to compact (no safe cut point)";
            }
            return false;
        }

        std::vector<a2a::models::Message> result;
        result.reserve(outcome.messages.size());
        for (const auto& message : outcome.messages) {
            if (message.role != "user" && message.role != "assistant") {
                continue;
            }
            if (message.content.empty()) {
                continue;
            }
            const auto role = message.role == "user" ? a2a::models::Role::kUser
                                                      : a2a::models::Role::kAgent;
            result.push_back(a2a::models::MakeTextMessage(role, message.content,
                                                          "text/plain"));
        }

        CompactionRecord manual_record;
        manual_record.forced = true;
        manual_record.tokens_before = outcome.tokens_before;
        manual_record.tokens_after = outcome.tokens_after;
        manual_record.messages_before = chat_messages.size();
        manual_record.messages_after = outcome.messages.size();
        *compacted_history = std::move(result);
        *record = manual_record;
        if (compaction_callback_) {
            compaction_callback_(context_id, manual_record);
        }
        return true;
    } catch (const std::exception& e) {
        if (error != nullptr) {
            *error = std::string("compaction failed: ") + e.what();
        }
        return false;
    }
}

a2a::models::SendMessageResponse AgentRuntime::HandleMessage(
        const a2a::models::Message& incoming,
        const std::vector<a2a::models::Message>& history,
        const std::string& context_id) {
    const std::string run_id = std::string(a2a::core::NewUuid());
    aiagent::DefaultLogger().Log(
            aiagent::LogLevel::kInfo, "agent_runtime",
            "run=" + run_id + " session=" + (context_id.empty() ? "-" : context_id) +
                    " event=request_start messages=" + std::to_string(history.size()));

    const tools::ToolContext tool_context = BuildToolContext({});

    std::vector<llm::ChatMessage> chat_messages;

    llm::ChatMessage system;
    system.role = "system";
    system.content = system_prompt_;
    AppendPermissionMode(system.content, ResolvePermissionMode({}));
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

    bool reached_max_rounds = false;
    int completed_rounds = 0;
    // 上下文计量：usage 覆盖 messages[0, last_usage_message_count)，其后尾部再估算。
    int last_usage_tokens = 0;
    std::size_t last_usage_message_count = 0;
    std::size_t last_compacted_size = static_cast<std::size_t>(-1);
    bool overflow_retry_used = false;
    llm::ChatResult result;
    for (int round = 0;; ++round) {
        if (max_tool_rounds_ > 0 && round >= max_tool_rounds_) {
            reached_max_rounds = true;
            break;
        }
        MaybeCompactBeforeCall(chat_messages, last_usage_tokens,
                               last_usage_message_count, last_compacted_size, run_id,
                               context_id);
        const std::vector<llm::ToolSpec> tool_specs = tools_ ? tools_->ListTools() : std::vector<llm::ToolSpec>();
        const std::size_t messages_at_call = chat_messages.size();
        try {
            result = model_->Chat(chat_messages, tool_specs);
        } catch (const llm::DeepSeekException& e) {
            // 上下文超长：强制压缩后重试本回合一次。
            if (!overflow_retry_used && IsContextOverflowMessage(e.what()) &&
                MaybeCompactBeforeCall(chat_messages, last_usage_tokens,
                                       last_usage_message_count, last_compacted_size,
                                       run_id, context_id, /*force=*/true)) {
                overflow_retry_used = true;
                --round;
                continue;
            }
            throw;
        }
        if (result.choices.empty()) {
            throw std::runtime_error("model returned no choices");
        }
        const bool has_usage = result.usage.total_tokens > 0;

        const auto& choice = result.choices.front();
        // 长度截断：压缩后重试一次，丢弃这次的半截结果。
        if (!overflow_retry_used && choice.tool_calls.empty() &&
            IsRecoverableLengthStop(choice.finish_reason) &&
            MaybeCompactBeforeCall(chat_messages, last_usage_tokens,
                                   last_usage_message_count, last_compacted_size,
                                   run_id, context_id, /*force=*/true)) {
            overflow_retry_used = true;
            --round;
            continue;
        }
        if (choice.tool_calls.empty()) {
            if (has_usage) {
                last_usage_tokens = result.usage.total_tokens;
                last_usage_message_count = messages_at_call;
            }
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
        if (has_usage) {
            // usage 覆盖到这条 assistant（含其 completion），工具结果算尾部。
            last_usage_tokens = result.usage.total_tokens;
            last_usage_message_count = chat_messages.size();
        }

        for (const auto& call : choice.tool_calls) {
            const auto tool_started = std::chrono::steady_clock::now();
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kInfo, "agent_runtime",
                    "run=" + run_id + " event=tool_start name=" + call.name +
                            " args_bytes=" + std::to_string(call.arguments.size()));
            tools::ToolExecutionResult execution_result;
            const std::string denial =
                    CheckToolPermission(call.name, tool_context.policy);
            if (!denial.empty()) {
                execution_result.ok = false;
                execution_result.error = denial;
                aiagent::DefaultLogger().Log(
                        aiagent::LogLevel::kWarning, "agent_runtime",
                        "run=" + run_id + " event=permission_denied tool=" +
                                call.name + " reason=" + denial);
            } else {
                try {
                    nlohmann::json arguments = nlohmann::json::object();
                    if (!call.arguments.empty()) {
                        arguments = nlohmann::json::parse(call.arguments);
                    }
                    execution_result =
                            tools_->Execute(call.name, arguments, tool_context);
                } catch (const std::exception& e) {
                    execution_result.ok = false;
                    execution_result.error = e.what();
                }
            }
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - tool_started)
                                             .count();
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kInfo, "agent_runtime",
                    "run=" + run_id + " event=tool_end name=" + call.name + " ok=" +
                            (execution_result.ok ? "true" : "false") +
                            " duration_ms=" + std::to_string(duration_ms) +
                            " output_bytes=" +
                            std::to_string(execution_result.ok
                                                   ? execution_result.text.size()
                                                   : execution_result.error.size()));

            llm::ChatMessage tool_result;
            tool_result.role = "tool";
            tool_result.tool_call_id = call.id;
            tool_result.content = execution_result.ok
                                          ? execution_result.text
                                          : std::string("tool error: ") + execution_result.error;
            chat_messages.push_back(std::move(tool_result));
        }
        completed_rounds = round + 1;
    }

    if (reached_max_rounds) {
        throw std::runtime_error(
                "reached max tool rounds without a final answer; task may be incomplete");
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
    aiagent::DefaultLogger().Log(
            aiagent::LogLevel::kInfo, "agent_runtime",
            "run=" + run_id + " session=" + context_id +
                    " event=request_end reply_bytes=" +
                    std::to_string(reply_text.size()) +
                    " tool_rounds=" + std::to_string(completed_rounds));
    return response;
}

std::string AgentRuntime::RunStreaming(
        const a2a::models::Message& incoming,
        const std::vector<a2a::models::Message>& history,
        const std::string& context_id,
        const AgentEventCallback& on_event,
        const std::string& working_directory,
        const CancelCallback& should_cancel) {
    const std::string run_id = std::string(a2a::core::NewUuid());
    aiagent::DefaultLogger().Log(
            aiagent::LogLevel::kInfo, "agent_runtime",
            "run=" + run_id + " session=" + (context_id.empty() ? "-" : context_id) +
                    " event=request_start streaming=true messages=" +
                    std::to_string(history.size()));

    const tools::ToolContext tool_context = BuildToolContext(working_directory);
    std::vector<llm::ChatMessage> chat_messages;
    chat_messages.push_back(BuildSystemMessage(working_directory));

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

    bool reached_max_rounds = false;
    int completed_rounds = 0;
    std::string reply;
    int last_usage_tokens = 0;
    std::size_t last_usage_message_count = 0;
    std::size_t last_compacted_size = static_cast<std::size_t>(-1);
    bool overflow_retry_used = false;
    for (int round = 0;; ++round) {
        if (should_cancel && should_cancel()) {
            throw std::runtime_error("task interrupted");
        }
        if (max_tool_rounds_ > 0 && round >= max_tool_rounds_) {
            reached_max_rounds = true;
            break;
        }
        if (const auto record = MaybeCompactBeforeCall(
                    chat_messages, last_usage_tokens, last_usage_message_count,
                    last_compacted_size, run_id, context_id);
            record.has_value()) {
            EmitCompactionEvent(*record, on_event);
        }
        const std::vector<llm::ToolSpec> tool_specs =
                tools_ ? tools_->ListTools() : std::vector<llm::ToolSpec>();
        std::vector<llm::ToolCall> streamed_tool_calls;
        std::string round_reply;
        std::string finish_reason;
        const std::size_t messages_at_call = chat_messages.size();
        int stream_usage_tokens = 0;

        try {
            model_->ChatStream(
                    chat_messages,
                    tool_specs,
                    [&](const llm::StreamEvent& event) {
                        if (event.usage.total_tokens > 0) {
                            stream_usage_tokens = event.usage.total_tokens;
                        }
                        if (!event.content.empty()) {
                            round_reply += event.content;
                            if (on_event) {
                                AgentEvent agent_event;
                                agent_event.kind = AgentEventKind::kToken;
                                agent_event.content = event.content;
                                on_event(agent_event);
                            }
                        }
                        if (!event.finish_reason.empty()) {
                            finish_reason = event.finish_reason;
                        }
                        if (!event.tool_calls.empty()) {
                            streamed_tool_calls = event.tool_calls;
                        }
                    });
        } catch (const llm::DeepSeekException& e) {
            // 上下文超长：强制压缩后重试本回合一次（丢弃已生成内容）。
            if (!overflow_retry_used && IsContextOverflowMessage(e.what())) {
                if (const auto record = MaybeCompactBeforeCall(
                            chat_messages, last_usage_tokens,
                            last_usage_message_count, last_compacted_size, run_id,
                            context_id, /*force=*/true);
                    record.has_value()) {
                    EmitCompactionEvent(*record, on_event);
                    overflow_retry_used = true;
                    --round;
                    continue;
                }
            }
            throw;
        }

        // 长度截断：压缩后重试一次，丢弃本轮半截输出。
        if (!overflow_retry_used && streamed_tool_calls.empty() &&
            IsRecoverableLengthStop(finish_reason)) {
            if (const auto record = MaybeCompactBeforeCall(
                        chat_messages, last_usage_tokens, last_usage_message_count,
                        last_compacted_size, run_id, context_id, /*force=*/true);
                record.has_value()) {
                EmitCompactionEvent(*record, on_event);
                overflow_retry_used = true;
                --round;
                continue;
            }
        }

        reply += round_reply;
        const bool has_usage = stream_usage_tokens > 0;
        if (streamed_tool_calls.empty()) {
            if (has_usage) {
                last_usage_tokens = stream_usage_tokens;
                last_usage_message_count = messages_at_call;
            }
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
        if (has_usage) {
            last_usage_tokens = stream_usage_tokens;
            last_usage_message_count = chat_messages.size();
        }

        for (const auto& call : streamed_tool_calls) {
            if (should_cancel && should_cancel()) {
                throw std::runtime_error("task interrupted");
            }
            const auto tool_started = std::chrono::steady_clock::now();
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kInfo, "agent_runtime",
                    "run=" + run_id + " event=tool_start name=" + call.name +
                            " args_bytes=" + std::to_string(call.arguments.size()));
            if (on_event) {
                AgentEvent agent_event;
                agent_event.kind = AgentEventKind::kToolStart;
                agent_event.tool_call_id = call.id;
                agent_event.tool_name = call.name;
                agent_event.arguments = call.arguments;
                on_event(agent_event);
            }

            tools::ToolExecutionResult execution_result;
            const std::string denial =
                    CheckToolPermission(call.name, tool_context.policy);
            if (!denial.empty()) {
                execution_result.ok = false;
                execution_result.error = denial;
                aiagent::DefaultLogger().Log(
                        aiagent::LogLevel::kWarning, "agent_runtime",
                        "run=" + run_id + " event=permission_denied tool=" +
                                call.name + " reason=" + denial);
            } else {
                try {
                    nlohmann::json arguments = nlohmann::json::object();
                    if (!call.arguments.empty()) {
                        arguments = nlohmann::json::parse(call.arguments);
                    }
                    execution_result =
                            tools_->Execute(call.name, arguments, tool_context);
                } catch (const std::exception& e) {
                    execution_result.ok = false;
                    execution_result.error = e.what();
                }
            }
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - tool_started)
                                             .count();
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kInfo, "agent_runtime",
                    "run=" + run_id + " event=tool_end name=" + call.name + " ok=" +
                            (execution_result.ok ? "true" : "false") +
                            " duration_ms=" + std::to_string(duration_ms));

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
        completed_rounds = round + 1;
    }

    if (reached_max_rounds) {
        throw std::runtime_error(
                "reached max tool rounds without a final answer; task may be incomplete");
    }
    if (reply.empty()) {
        throw std::runtime_error("model returned an empty reply");
    }
    aiagent::DefaultLogger().Log(
            aiagent::LogLevel::kInfo, "agent_runtime",
            "run=" + run_id + " session=" + context_id +
                    " event=request_end streaming=true reply_bytes=" +
                    std::to_string(reply.size()) +
                    " tool_rounds=" + std::to_string(completed_rounds));
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
                        {}, {});
}

std::string AgentRuntime::HandleMessageStreamingEvents(
        const a2a::models::Message& incoming,
        const std::vector<a2a::models::Message>& history,
        const std::string& context_id,
        const AgentEventCallback& on_event,
        const std::string& working_directory,
        const CancelCallback& should_cancel) {
    return RunStreaming(incoming, history, context_id, on_event, working_directory,
                        should_cancel);
}

}  // namespace aiagent::agent
