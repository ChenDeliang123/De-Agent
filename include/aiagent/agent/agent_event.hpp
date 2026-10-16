#pragma once

#include <functional>
#include <string>

namespace aiagent::agent {

// Agent 流式执行过程中的结构化事件，供上层（Web Gateway / 终端客户端）渲染。
enum class AgentEventKind {
    kToken,            // assistant 文本增量（content）
    kToolStart,        // 工具调用开始（tool_call_id / tool_name / arguments）
    kToolOutput,       // 工具执行中的增量输出（tool_call_id / content），当前预留
    kToolEnd,          // 工具执行结束（tool_call_id / ok / text）
    kCompaction,       // 上下文压缩完成（compaction_tokens_before/after）
    kApprovalRequest,  // 工具需要人工确认（tool_call_id / tool_name / arguments / approval_reason）
    kSubAgentStart,    // 子 Agent 开始运行（agent_id / parent_tool_call_id）
    kSubAgentEnd,      // 子 Agent 结束（agent_id / ok / duration_ms / tool_calls / text）
};

struct AgentEvent {
    AgentEventKind kind = AgentEventKind::kToken;

    // kToken / kToolOutput
    std::string content;

    // 归属：空 = 根 Agent 自身的事件；子 Agent 的事件由 SubAgentManager 填写
    // （agent_id 形如 explorer#1，parent_tool_call_id 为父侧 task 工具调用 id）。
    std::string agent_id;
    std::string parent_tool_call_id;

    // kToolStart / kToolOutput / kToolEnd
    std::string tool_call_id;

    // kToolStart
    std::string tool_name;
    std::string arguments;  // 原始 JSON 字符串

    // kToolEnd
    bool ok = true;
    std::string text;  // 结果文本；ok=false 时为错误描述

    // kCompaction
    int compaction_tokens_before = 0;
    int compaction_tokens_after = 0;
    bool compaction_forced = false;  // overflow/length 触发的强制压缩

    // kSubAgentEnd
    int duration_ms = 0;   // 子 Agent 运行耗时
    int tool_calls = 0;    // 子 Agent 内部的工具调用次数
    // kSubAgentEnd：子 Agent 的 token 用量（口径同 deepseek 客户端 request_end 日志）。
    long long prompt_tokens = 0;
    long long completion_tokens = 0;
    long long cache_hit_tokens = 0;

    // kApprovalRequest
    std::string approval_reason;
};

using AgentEventCallback = std::function<void(const AgentEvent&)>;

}  // namespace aiagent::agent
