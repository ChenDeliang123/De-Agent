#pragma once

#include <string>

#include "aiagent/permissions/permission_policy.hpp"

namespace aiagent::tools {

// 一次工具调用的上下文：本次请求的工作目录 + 该请求生效的权限策略。
// AgentRuntime 在每轮请求开头解析并构造，供工具执行层做路径沙箱与能力裁决。
struct ToolContext {
    std::string working_directory;
    permissions::PermissionPolicy policy;
};

}  // namespace aiagent::tools
