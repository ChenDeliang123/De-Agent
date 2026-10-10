#pragma once

#include <memory>
#include <string>

#include "aiagent/permissions/bash_policy.hpp"
#include "aiagent/permissions/permission_policy.hpp"

namespace aiagent::tools {

// 一次工具调用的上下文：本次请求的工作目录 + 该请求生效的权限策略 +
// 合并后的 bash 命令白/黑名单（全局 + 项目级）。
// AgentRuntime 在每轮请求开头解析并构造，供工具执行层做路径沙箱与能力裁决。
struct ToolContext {
    std::string working_directory;
    permissions::PermissionPolicy policy;
    std::shared_ptr<const permissions::BashPolicy> bash_policy;
};

}  // namespace aiagent::tools
