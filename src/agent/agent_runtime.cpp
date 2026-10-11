#include "aiagent/agent/agent_runtime.hpp"
#include "aiagent/log.hpp"

#include "a2a/core/id.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

namespace aiagent::agent {
namespace {

// 上下文占用口径：只算 prompt（不含 model 的 completion）。
// 旧实现用 usage.total_tokens，一次大写入（例如 24KB file content ≈ 7K completion）
// 会把估算顶过阈值、提前触发压缩；实测 28 次压缩里 13 次"压完反而更大"。
// 兼容只报 total 的实现：用 total - completion 反推。
int UsageContextTokens(const llm::ChatUsage& usage) {
    if (usage.prompt_tokens > 0) {
        return usage.prompt_tokens;
    }
    const int derived = usage.total_tokens - usage.completion_tokens;
    return derived > 0 ? derived : usage.total_tokens;
}

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

std::string BashCommandFromArguments(const std::string& arguments) {
    if (arguments.empty()) {
        return {};
    }
    try {
        const nlohmann::json json = nlohmann::json::parse(arguments);
        if (json.is_object() && json.contains("command") &&
            json["command"].is_string()) {
            return json["command"].get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
        // 参数不是合法 JSON 时视为无命令。
    }
    return {};
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

void AgentRuntime::SetBashPolicy(permissions::BashPolicy policy) {
    bash_policy_ = std::move(policy);
}

void AgentRuntime::SetProjectConfigProvider(ProjectConfigProvider provider) {
    project_config_provider_ = std::move(provider);
}

void AgentRuntime::SetTrustStore(std::shared_ptr<permissions::TrustStore> store) {
    trust_store_ = std::move(store);
}

bool AgentRuntime::IsProjectTrusted(const permissions::ProjectConfig& project) const {
    if (project.trusted) {
        return true;
    }
    if (trust_store_ != nullptr && !project.root.empty()) {
        return trust_store_->IsTrusted(project.root);
    }
    return false;
}

std::shared_ptr<const permissions::ProjectConfig> AgentRuntime::ResolveProjectConfig(
        const std::string& directory) const {
    if (!project_config_provider_ || directory.empty()) {
        return nullptr;
    }
    return project_config_provider_(directory);
}

std::shared_ptr<const permissions::BashPolicy>
AgentRuntime::BuildEffectiveBashPolicy(const std::string& directory) const {
    auto merged = std::make_shared<permissions::BashPolicy>(bash_policy_);
    if (const auto project = ResolveProjectConfig(directory);
        project && project->bash_policy != nullptr) {
        // deny 永远叠加（项目级总能加严）。
        merged->deny.insert(merged->deny.end(), project->bash_policy->deny.begin(),
                            project->bash_policy->deny.end());
        // allow 仅当项目被信任时叠加。
        if (IsProjectTrusted(*project)) {
            merged->allow.insert(merged->allow.end(),
                                 project->bash_policy->allow.begin(),
                                 project->bash_policy->allow.end());
        }
    }
    return merged;
}

permissions::PermissionMode AgentRuntime::ResolvePermissionMode(
        const std::string& working_directory, bool interactive) const {
    permissions::PermissionMode mode = permission_settings_.default_mode;
    const std::string directory =
            working_directory.empty() ? fallback_working_directory_ : working_directory;
    if (permission_store_ != nullptr && !directory.empty()) {
        if (const auto entry = permission_store_->Resolve(directory);
            entry.has_value()) {
            mode = entry->mode;
        }
    }
    // 项目级权限：已信任则覆盖，未信任只能收紧（P0 恒为未信任）。
    if (const auto project = ResolveProjectConfig(directory);
        project && project->permissions != nullptr) {
        if (const auto entry = project->permissions->Resolve(directory);
            entry.has_value()) {
            if (IsProjectTrusted(*project)) {
                mode = entry->mode;
            } else if (permissions::ModeRank(entry->mode) <
                       permissions::ModeRank(mode)) {
                mode = entry->mode;
            }
        }
    }
    // 非交互调用方：取 max_mode 与 non_interactive_max_mode 中更严的一档，
    // 且不允许 full。
    permissions::PermissionMode max_mode = permission_settings_.max_mode;
    bool allow_full = permission_settings_.allow_full_access;
    if (!interactive) {
        if (permissions::ModeRank(permission_settings_.non_interactive_max_mode) <
            permissions::ModeRank(max_mode)) {
            max_mode = permission_settings_.non_interactive_max_mode;
        }
        allow_full = false;
    }
    return permissions::ClampMode(mode, max_mode, allow_full);
}

tools::ToolContext AgentRuntime::BuildToolContext(
        const std::string& working_directory, bool interactive) const {
    tools::ToolContext context;
    context.working_directory =
            working_directory.empty() ? fallback_working_directory_ : working_directory;
    context.policy =
            permissions::PolicyForMode(ResolvePermissionMode(working_directory, interactive));
    context.bash_policy = BuildEffectiveBashPolicy(context.working_directory);
    return context;
}

void AgentRuntime::SetApprovalHandler(ApprovalHandler handler) {
    approval_handler_ = std::move(handler);
}

bool AgentRuntime::SessionAllows(const std::string& context_id,
                                 const std::string& tool_name) const {
    std::lock_guard<std::mutex> lock(session_allow_mutex_);
    const auto it = session_allow_.find(context_id);
    return it != session_allow_.end() && it->second.count(tool_name) > 0;
}

void AgentRuntime::GrantSessionAllow(const std::string& context_id,
                                     const std::string& tool_name) {
    std::lock_guard<std::mutex> lock(session_allow_mutex_);
    session_allow_[context_id].insert(tool_name);
}

AgentRuntime::ToolGateResult AgentRuntime::CheckToolGate(
        const std::string& tool_name,
        const tools::ToolContext& context,
        const std::string& context_id,
        const std::string& bash_command) const {
    const permissions::PermissionPolicy& policy = context.policy;
    if (tool_name == "read") {
        if (!policy.allow_read) {
            return {ToolGate::kDeny,
                    "permission denied: read is disabled by the current permission mode"};
        }
        return {ToolGate::kAllow, {}};
    }
    if (tool_name == "write" || tool_name == "edit") {
        if (!policy.allow_write) {
            return {ToolGate::kDeny,
                    "permission denied: file writes are disabled by the current "
                    "permission mode"};
        }
        if (policy.require_confirm_write && !SessionAllows(context_id, tool_name)) {
            return {ToolGate::kNeedsApproval, "file write needs approval"};
        }
        return {ToolGate::kAllow, {}};
    }
    if (tool_name == "bash") {
        // 命令级白/黑名单优先于模式：deny 不可被确认绕过，allow 免确认。
        const permissions::BashPolicy& effective_policy =
                context.bash_policy ? *context.bash_policy : bash_policy_;
        if (!bash_command.empty()) {
            const permissions::BashDecision bash_decision =
                    permissions::EvaluateBashPolicy(bash_command, effective_policy);
            if (bash_decision == permissions::BashDecision::kDeny) {
                return {ToolGate::kDeny,
                        "permission denied: command blocked by bash policy"};
            }
            if (bash_decision == permissions::BashDecision::kAllow) {
                return {ToolGate::kAllow, {}};
            }
        }
        if (!policy.allow_command) {
            return {ToolGate::kDeny,
                    "permission denied: command execution is disabled by the current "
                    "permission mode"};
        }
        if (policy.require_confirm_command && !SessionAllows(context_id, tool_name)) {
            return {ToolGate::kNeedsApproval, "command execution needs approval"};
        }
        return {ToolGate::kAllow, {}};
    }
    // 未分类工具（含 MCP）：M1 暂不拦截，保持既有行为；工具分类与
    // “非 full 下需确认”将在 M4 引入。
    return {ToolGate::kAllow, {}};
}

AgentRuntime::ToolDecision AgentRuntime::DecideTool(
        const llm::ToolCall& call,
        const tools::ToolContext& context,
        const std::string& context_id,
        const AgentEventCallback& on_event,
        bool interactive) {
    std::string bash_command;
    if (call.name == "bash") {
        bash_command = BashCommandFromArguments(call.arguments);
    }
    const ToolGateResult gate =
            CheckToolGate(call.name, context, context_id, bash_command);
    if (gate.gate == ToolGate::kAllow) {
        return {true, {}};
    }
    if (gate.gate == ToolGate::kDeny) {
        return {false, gate.reason};
    }
    // kNeedsApproval：无审批通道（非交互或未注入 handler）则直接拒绝。
    if (!interactive || !approval_handler_) {
        return {false, "permission denied: " + gate.reason +
                               " (no approver available); use /permissions to adjust"};
    }
    if (on_event) {
        AgentEvent event;
        event.kind = AgentEventKind::kApprovalRequest;
        event.tool_call_id = call.id;
        event.tool_name = call.name;
        event.arguments = call.arguments;
        event.approval_reason = gate.reason;
        on_event(event);
    }
    ApprovalRequest request;
    request.context_id = context_id;
    request.tool_call_id = call.id;
    request.tool_name = call.name;
    request.arguments = call.arguments;
    request.reason = gate.reason;
    const ApprovalDecision decision = approval_handler_(request);
    if (decision == ApprovalDecision::kAllowSession) {
        GrantSessionAllow(context_id, call.name);
        return {true, {}};
    }
    if (decision == ApprovalDecision::kAllowOnce) {
        return {true, {}};
    }
    return {false, "permission denied: user rejected the request"};
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
    // 记一行策略快照：之后任何一次会话日志都能看出当时的阈值/保留量/收益下限，
    // 避免"压缩为什么这么频繁"这类问题只能靠翻配置猜。
    aiagent::DefaultLogger().Log(
            aiagent::LogLevel::kInfo, "agent_runtime",
            "event=compaction_settings enabled=" +
                    std::string(compaction_settings_.enabled ? "true" : "false") +
                    " context_window=" + std::to_string(compaction_settings_.context_window) +
                    " reserve_tokens=" + std::to_string(compaction_settings_.reserve_tokens) +
                    " compact_at_tokens=" +
                    std::to_string(compaction_settings_.compact_at_tokens) +
                    " threshold=" +
                    std::to_string(EffectiveCompactThreshold(compaction_settings_)) +
                    " keep_recent_tokens=" +
                    std::to_string(compaction_settings_.keep_recent_tokens) +
                    " min_gain_ratio=" +
                    std::to_string(compaction_settings_.min_gain_ratio));
}

void AgentRuntime::SetCompactionCallback(CompactionCallback callback) {
    compaction_callback_ = std::move(callback);
}

std::optional<CompactionRecord> AgentRuntime::MaybeCompactBeforeCall(
        std::vector<llm::ChatMessage>& chat_messages,
        int& last_usage_tokens,
        std::size_t& last_usage_message_count,
        std::size_t& last_compacted_size,
        int& gain_retry_at_tokens,
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
        // 上次因收益不足跳过时，至少要涨到"那时再压就能达标"的规模才值得再试一次，
        // 否则会退化成"每轮都白花一次摘要调用"。
        if (gain_retry_at_tokens > 0 && estimate.tokens < gain_retry_at_tokens) {
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
        // 收益下限：压完至少要降 min_gain_ratio，否则保持原历史（强制压缩不受限）。
        // 没有这道闸时实测 28 次压缩里有 13 次"压完反而更大"，白白多花一次摘要往返。
        if (!force && compaction_settings_.min_gain_ratio > 0.0 &&
            outcome.tokens_before > 0) {
            const double allowed_after =
                    static_cast<double>(outcome.tokens_before) *
                    (1.0 - compaction_settings_.min_gain_ratio);
            if (static_cast<double>(outcome.tokens_after) > allowed_after) {
                aiagent::DefaultLogger().Log(
                        aiagent::LogLevel::kInfo, "agent_runtime",
                        "run=" + run_id +
                                " event=compaction_skipped reason=low_gain tokens_before=" +
                                std::to_string(outcome.tokens_before) +
                                " tokens_after=" + std::to_string(outcome.tokens_after) +
                                " min_gain_ratio=" +
                                std::to_string(compaction_settings_.min_gain_ratio));
                const double remaining =
                        std::max(0.0, 1.0 - compaction_settings_.min_gain_ratio);
                gain_retry_at_tokens =
                        remaining > 0.0
                                ? static_cast<int>(static_cast<double>(outcome.tokens_after) /
                                                   remaining) +
                                          1
                                : 0;
                // 同一批消息不再重试（下一条消息进来后自然重新评估）。
                last_compacted_size = chat_messages.size();
                return std::nullopt;
            }
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
        gain_retry_at_tokens = 0;
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

    const tools::ToolContext tool_context = BuildToolContext({}, /*interactive=*/false);

    std::vector<llm::ChatMessage> chat_messages;

    llm::ChatMessage system;
    system.role = "system";
    system.content = system_prompt_;
    AppendPermissionMode(system.content, ResolvePermissionMode({}, /*interactive=*/false));
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
    int gain_retry_at_tokens = 0;
    bool overflow_retry_used = false;
    llm::ChatResult result;
    for (int round = 0;; ++round) {
        if (max_tool_rounds_ > 0 && round >= max_tool_rounds_) {
            reached_max_rounds = true;
            break;
        }
        MaybeCompactBeforeCall(chat_messages, last_usage_tokens,
                               last_usage_message_count, last_compacted_size,
                               gain_retry_at_tokens, run_id, context_id);
        const std::vector<llm::ToolSpec> tool_specs = tools_ ? tools_->ListTools() : std::vector<llm::ToolSpec>();
        const std::size_t messages_at_call = chat_messages.size();
        try {
            result = model_->Chat(chat_messages, tool_specs);
        } catch (const llm::DeepSeekException& e) {
            // 上下文超长：强制压缩后重试本回合一次。
            if (!overflow_retry_used && IsContextOverflowMessage(e.what()) &&
                MaybeCompactBeforeCall(chat_messages, last_usage_tokens,
                                       last_usage_message_count, last_compacted_size,
                                       gain_retry_at_tokens, run_id, context_id,
                                       /*force=*/true)) {
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
                                   gain_retry_at_tokens, run_id, context_id,
                                   /*force=*/true)) {
            overflow_retry_used = true;
            --round;
            continue;
        }
        if (choice.tool_calls.empty()) {
            if (has_usage) {
                last_usage_tokens = UsageContextTokens(result.usage);
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
            // 这里只取 prompt 部分作为上下文占用（见 UsageContextTokens）。
            last_usage_tokens = UsageContextTokens(result.usage);
            last_usage_message_count = chat_messages.size();
        }

        for (const auto& call : choice.tool_calls) {
            const auto tool_started = std::chrono::steady_clock::now();
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kInfo, "agent_runtime",
                    "run=" + run_id + " event=tool_start name=" + call.name +
                            " args_bytes=" + std::to_string(call.arguments.size()));
            const ToolDecision decision = DecideTool(call, tool_context, context_id,
                                                     nullptr, /*interactive=*/false);
            tools::ToolExecutionResult execution_result;
            if (!decision.allowed) {
                execution_result.ok = false;
                execution_result.error = decision.reason;
                aiagent::DefaultLogger().Log(
                        aiagent::LogLevel::kWarning, "agent_runtime",
                        "run=" + run_id + " event=permission_denied tool=" +
                                call.name + " reason=" + decision.reason);
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
    int gain_retry_at_tokens = 0;
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
                    last_compacted_size, gain_retry_at_tokens, run_id, context_id);
            record.has_value()) {
            EmitCompactionEvent(*record, on_event);
        }
        const std::vector<llm::ToolSpec> tool_specs =
                tools_ ? tools_->ListTools() : std::vector<llm::ToolSpec>();
        std::vector<llm::ToolCall> streamed_tool_calls;
        std::string round_reply;
        std::string finish_reason;
        const std::size_t messages_at_call = chat_messages.size();
        llm::ChatUsage round_usage;

        try {
            model_->ChatStream(
                    chat_messages,
                    tool_specs,
                    [&](const llm::StreamEvent& event) {
                        if (event.usage.total_tokens > 0) {
                            round_usage = event.usage;
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
                            last_usage_message_count, last_compacted_size,
                            gain_retry_at_tokens, run_id, context_id, /*force=*/true);
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
                        last_compacted_size, gain_retry_at_tokens, run_id, context_id,
                        /*force=*/true);
                record.has_value()) {
                EmitCompactionEvent(*record, on_event);
                overflow_retry_used = true;
                --round;
                continue;
            }
        }

        reply += round_reply;
        const bool has_usage = round_usage.total_tokens > 0;
        if (streamed_tool_calls.empty()) {
            if (has_usage) {
                // 口径：只把 prompt 当作上下文占用（不含本轮输出）。
                last_usage_tokens = UsageContextTokens(round_usage);
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
            last_usage_tokens = UsageContextTokens(round_usage);
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

            const ToolDecision decision = DecideTool(call, tool_context, context_id,
                                                     on_event, /*interactive=*/true);
            tools::ToolExecutionResult execution_result;
            if (!decision.allowed) {
                execution_result.ok = false;
                execution_result.error = decision.reason;
                aiagent::DefaultLogger().Log(
                        aiagent::LogLevel::kWarning, "agent_runtime",
                        "run=" + run_id + " event=permission_denied tool=" +
                                call.name + " reason=" + decision.reason);
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
