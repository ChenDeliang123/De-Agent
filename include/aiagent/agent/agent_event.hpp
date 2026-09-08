#pragma once

#include <functional>
#include <string>

namespace aiagent::agent {

// Agent 流式执行过程中的结构化事件，供上层（Web Gateway / 终端客户端）渲染。
enum class AgentEventKind {
    kToken,      // assistant 文本增量（content）
    kToolStart,  // 工具调用开始（tool_call_id / tool_name / arguments）
    kToolOutput, // 工具执行中的增量输出（tool_call_id / content），当前预留
    kToolEnd,    // 工具执行结束（tool_call_id / ok / text）
};

struct AgentEvent {
    AgentEventKind kind = AgentEventKind::kToken;

    // kToken / kToolOutput
    std::string content;

    // kToolStart / kToolOutput / kToolEnd
    std::string tool_call_id;

    // kToolStart
    std::string tool_name;
    std::string arguments;  // 原始 JSON 字符串

    // kToolEnd
    bool ok = true;
    std::string text;  // 结果文本；ok=false 时为错误描述
};

using AgentEventCallback = std::function<void(const AgentEvent&)>;

}  // namespace aiagent::agent
