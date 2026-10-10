#pragma once

#include <optional>
#include <string>

namespace aiagent::permissions {

// Agent 在某个工作路径下的权限档位。
//
// 与需求约定（docs/权限模式规划.md）：
//   - confirm   : 写文件与执行命令都需要人工确认（默认）
//   - workspace : 工作目录内可自由读写，执行命令需确认
//   - full      : 关闭路径沙箱，读写与命令全部放行（危险）
enum class PermissionMode {
    kConfirm,    // "confirm" / "ask"
    kWorkspace,  // "workspace" / "ws" / "write"
    kFull,       // "full" / "danger"
};

// 档位展开后的能力策略。工具执行层只看这个结构，不看档位字符串。
struct PermissionPolicy {
    bool allow_read = true;
    bool allow_write = false;    // write / edit
    bool allow_outside = false;  // 是否允许访问工作目录之外（读 + 写）
    bool allow_command = false;  // bash
    bool require_confirm_write = false;
    bool require_confirm_command = false;
};

// 档位 -> 能力策略。
PermissionPolicy PolicyForMode(PermissionMode mode);

// 解析档位字符串（大小写不敏感，含别名）；无法识别返回 nullopt。
std::optional<PermissionMode> ParseMode(const std::string& text);

// 档位规范名（confirm / workspace / full）。
const char* ModeName(PermissionMode mode);

// 该档位是否可被父目录继承（full 不继承，避免误设后扩散）。
bool ModeInherits(PermissionMode mode);

// 生成给系统提示词用的一句话描述。
std::string DescribePolicy(PermissionMode mode);

}  // namespace aiagent::permissions
