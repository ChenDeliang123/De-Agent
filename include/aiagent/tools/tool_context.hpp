#pragma once

#include <functional>
#include <memory>
#include <string>

#include "aiagent/agent/agent_event.hpp"
#include "aiagent/permissions/bash_policy.hpp"
#include "aiagent/permissions/permission_policy.hpp"

namespace aiagent::tools {

// 一次工具调用的上下文：本次请求的工作目录 + 该请求生效的权限策略 +
// 合并后的 bash 命令白/黑名单（全局 + 项目级）。
// AgentRuntime 在每轮请求开头解析并构造，供工具执行层做路径沙箱与能力裁决。
//
// 运行通道（流式路径由 AgentRuntime 填充，其它路径可能为空）：
//   - session_id / tool_call_id：本次运行与会话归属，供 task 等工具做审批
//     会话键、事件归属与日志关联；
//   - emit：工具向上层事件流回传事件（子 Agent 的进度/工具事件靠它进入
//     父会话 SSE 流）；
//   - should_cancel：运行是否已被取消/超时，工具应尽快返回。
struct ToolContext {
    std::string working_directory;
    permissions::PermissionPolicy policy;
    std::shared_ptr<const permissions::BashPolicy> bash_policy;

    std::string session_id;
    std::string tool_call_id;
    agent::AgentEventCallback emit;
    std::function<bool()> should_cancel;
};

}  // namespace aiagent::tools
