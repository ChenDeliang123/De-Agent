#pragma once

#include <memory>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "a2a/models/message.hpp"
#include "a2a/models/operations.hpp"
#include "a2a/models/requests.hpp"

#include "aiagent/agent/agent_event.hpp"
#include "aiagent/agent/approval_broker.hpp"
#include "aiagent/agent/compaction.hpp"
#include "aiagent/agent/project_instructions.hpp"
#include "aiagent/llm/deepseek_client.hpp"
#include "aiagent/permissions/bash_policy.hpp"
#include "aiagent/permissions/permission_policy.hpp"
#include "aiagent/permissions/permission_store.hpp"
#include "aiagent/permissions/project_config.hpp"
#include "aiagent/permissions/trust_store.hpp"
#include "aiagent/tools/tool_context.hpp"
#include "aiagent/tools/tool_provider.hpp"

namespace aiagent::agent {

// 权限模式配置（来自 [permissions]）。
struct PermissionSettings {
    permissions::PermissionMode default_mode = permissions::PermissionMode::kConfirm;
    permissions::PermissionMode max_mode = permissions::PermissionMode::kFull;
    bool allow_full_access = true;
    // 非交互调用方（A2A / 同步接口）允许的最高档位，且永远不会拿到 full。
    permissions::PermissionMode non_interactive_max_mode =
            permissions::PermissionMode::kWorkspace;
};

// 一次工具审批请求（发送给上层/CLI）。
struct ApprovalRequest {
    std::string context_id;
    std::string tool_call_id;
    std::string tool_name;
    std::string arguments;  // 原始 JSON 字符串
    std::string reason;     // 为什么需要确认
};

// 把 A2A Message 历史翻译成 LLM messages，并执行一次对话。
// 不负责 HTTP/任务存储，由 A2A 层把 TaskManager 与它接起来。
class AgentRuntime {
public:
    using CancelCallback = std::function<bool()>;
    // 每次成功压缩后的回调（可用于日志/审计/前端展示）。
    using CompactionCallback = std::function<void(const std::string& context_id,
                                                  const CompactionRecord&)>;

    // 工具需要逐次确认时的回调；应阻塞直到有决定（由 ApprovalBroker 提供）。
    using ApprovalHandler = std::function<ApprovalDecision(const ApprovalRequest&)>;

    explicit AgentRuntime(std::shared_ptr<llm::IModelClient> model,
                          std::string system_prompt =
                                  "You are de agent, a general-purpose coding agent "
                                  "created by 德先生.");

    // 可选：注入 MCP/工具提供者后，Agent 会自动执行 model 的 tool_calls。
    void SetToolProvider(std::shared_ptr<tools::IToolProvider> tools);
    void SetMaxToolRounds(int rounds);

    // 可选：注入按路径权限存储与默认设置。未注入时使用 default_mode。
    void SetPermissionStore(std::shared_ptr<permissions::PermissionStore> store);
    void SetPermissionSettings(const PermissionSettings& settings);

    // 可选：注入 bash 命令白/黑名单。未注入时不做命令级拦截。
    void SetBashPolicy(permissions::BashPolicy policy);

    // 可选：注入项目级配置提供者（读 `<project>/.de/`）。
    // 未注入时只用用户级/全局配置。
    using ProjectConfigProvider =
            std::function<std::shared_ptr<const permissions::ProjectConfig>(
                    const std::string& cwd)>;
    void SetProjectConfigProvider(ProjectConfigProvider provider);

    // 可选：注入用户级信任存储。项目 `.de/` 被信任后才允许放松权限。
    void SetTrustStore(std::shared_ptr<permissions::TrustStore> store);

    // 可选：注入审批回调。未注入时“需要确认”的工具直接拒绝。
    void SetApprovalHandler(ApprovalHandler handler);

    // 可选：声明「本运行时可派发子 Agent」（main 在注册 task 工具后调用）。
    // 开启后系统消息会附带派发准则（何时该派、何时不该派）；默认关闭，
    // 避免在子 Agent 未启用时误导模型去调不存在的工具。
    void SetDelegationEnabled(bool enabled);

    // 可选：注入项目指令加载器（AGENTS.md）与无 cwd 请求的默认工作目录。
    void SetProjectInstructionsLoader(
            std::shared_ptr<ProjectInstructionsLoader> loader);
    void SetFallbackWorkingDirectory(std::string directory);

    // 设置上下文压缩参数；默认 enabled，阈值见 CompactionSettings。
    void SetCompactionSettings(const CompactionSettings& settings);

    // 注册压缩回调（可选）。每次阈值/overflow 压缩成功后触发。
    void SetCompactionCallback(CompactionCallback callback);

    // 手动压缩一段会话历史（对应终端 /compact 与 REST 接口）。
    // 无论是否达到阈值都尝试压缩；成功时输出压缩后的 A2A 历史
    // （system 不入库，摘要 + 保留尾部），并填充 record。
    // 返回 false 时写入 error。
    bool CompactHistory(const std::string& context_id,
                        const std::vector<a2a::models::Message>& history,
                        std::vector<a2a::models::Message>* compacted_history,
                        CompactionRecord* record,
                        std::string* error,
                        const std::string& working_directory = {});

    // history 应包含本次用户消息（续聊时由调用方从 TaskStore 取回）。
    // 返回一个已完成 Task，其 history 追加了 assistant 回复。
    a2a::models::SendMessageResponse HandleMessage(
            const a2a::models::Message& incoming,
            const std::vector<a2a::models::Message>& history,
            const std::string& context_id = {});

    // 流式版本：逐 token 回调，支持工具调用循环；返回最终回复文本。
    std::string HandleMessageStreaming(
            const a2a::models::Message& incoming,
            const std::vector<a2a::models::Message>& history,
            const std::string& context_id,
            const std::function<void(const std::string& token)>& on_token);

    // 流式版本（结构化事件）：逐 token 与工具调用起止均以 AgentEvent 回调。
    std::string HandleMessageStreamingEvents(
            const a2a::models::Message& incoming,
            const std::vector<a2a::models::Message>& history,
            const std::string& context_id,
            const AgentEventCallback& on_event,
            const std::string& working_directory = {},
            const CancelCallback& should_cancel = {});

private:
    // 流式主循环的公共实现：产出 AgentEvent 事件流，返回最终回复文本。
    std::string RunStreaming(const a2a::models::Message& incoming,
                             const std::vector<a2a::models::Message>& history,
                             const std::string& context_id,
                             const AgentEventCallback& on_event,
                             const std::string& working_directory,
                             const CancelCallback& should_cancel);

    // 组装请求使用的 system message：基础 prompt + 项目指令 + 工作目录 + 权限模式。
    llm::ChatMessage BuildSystemMessage(
            const std::string& working_directory) const;

    // 追加派发准则（仅在 SetDelegationEnabled(true) 时生效）。流式与非流式
    // 两条系统消息构造路径共用，避免只有一条路径能派发。
    void AppendDelegationGuidance(std::string& content) const;

    // 解析某工作目录当前生效的权限档位（含 max_mode/allow_full_access 夹取）。
    // interactive=false（A2A / 同步接口）时还会按 non_interactive_max_mode 夹取。
    permissions::PermissionMode ResolvePermissionMode(
            const std::string& working_directory, bool interactive = true) const;

    // 构造本次请求的工具上下文（工作目录 + 展开后的权限策略）。
    tools::ToolContext BuildToolContext(const std::string& working_directory,
                                        bool interactive = true) const;

    // 结合权限策略与会话级「总是允许」判断某工具是直接放行、需要审批还是拒绝。
    enum class ToolGate { kAllow, kNeedsApproval, kDeny };
    struct ToolGateResult {
        ToolGate gate = ToolGate::kAllow;
        std::string reason;
    };
    ToolGateResult CheckToolGate(const std::string& tool_name,
                                 const tools::ToolContext& context,
                                 const std::string& context_id,
                                 const std::string& bash_command) const;

    // 解析 cwd 对应项目的只读配置（可能为空）。
    std::shared_ptr<const permissions::ProjectConfig> ResolveProjectConfig(
            const std::string& directory) const;

    // 项目是否被信任（项目自带标记或信任存储）。
    bool IsProjectTrusted(const permissions::ProjectConfig& project) const;

    // 合并全局与项目级 bash 规则：deny 叠加；allow 仅当项目被信任时叠加。
    std::shared_ptr<const permissions::BashPolicy> BuildEffectiveBashPolicy(
            const std::string& directory) const;

    // 结合审批通道得出工具是否可执行；needs-approval 时会发 kApprovalRequest
    // 事件并调用 approval_handler_ 阻塞等待。
    struct ToolDecision {
        bool allowed = false;
        std::string reason;
    };
    ToolDecision DecideTool(const llm::ToolCall& call,
                            const tools::ToolContext& context,
                            const std::string& context_id,
                            const AgentEventCallback& on_event,
                            bool interactive);

    bool SessionAllows(const std::string& context_id, const std::string& tool_name) const;
    void GrantSessionAllow(const std::string& context_id, const std::string& tool_name);

    // 模型调用前的阈值压缩。命中阈值时把较早的消息摘要成一条 user 消息，
    // 并重置 usage 跟踪；同一批消息（size 未变）不重复压缩。
    // force=true 时跳过阈值判断与去重（用于 overflow 恢复）。
    // 返回本次压缩记录；未压缩时返回 nullopt。
    std::optional<CompactionRecord> MaybeCompactBeforeCall(
            std::vector<llm::ChatMessage>& chat_messages,
            int& last_usage_tokens,
            std::size_t& last_usage_message_count,
            std::size_t& last_compacted_size,
            int& gain_retry_at_tokens,
            const std::string& run_id,
            const std::string& context_id,
            bool force = false);

    std::shared_ptr<llm::IModelClient> model_;
    std::shared_ptr<tools::IToolProvider> tools_;
    std::shared_ptr<ProjectInstructionsLoader> instructions_loader_;
    std::shared_ptr<permissions::PermissionStore> permission_store_;
    PermissionSettings permission_settings_;
    permissions::BashPolicy bash_policy_;
    ProjectConfigProvider project_config_provider_;
    std::shared_ptr<permissions::TrustStore> trust_store_;
    ApprovalHandler approval_handler_;
    // 是否在系统消息里注入派发准则（见 SetDelegationEnabled）。
    bool delegation_enabled_ = false;
    mutable std::mutex session_allow_mutex_;
    // context_id -> 本会话「总是允许」的工具名集合（仅内存，不持久化）。
    std::map<std::string, std::set<std::string>> session_allow_;
    std::string fallback_working_directory_;
    std::string system_prompt_;
    // 0 表示不限制轮次（默认，对齐 Pi 的“有工具调用就继续”）。
    // 显式设置正数时作为可选安全上限。
    int max_tool_rounds_ = 0;
    CompactionSettings compaction_settings_;
    CompactionCallback compaction_callback_;
};

}  // namespace aiagent::agent
