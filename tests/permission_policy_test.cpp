// 权限档位（PermissionMode）与能力策略（PermissionPolicy）的单元测试。

#include <iostream>
#include <string>

#include "aiagent/config.hpp"
#include "aiagent/permissions/permission_policy.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

using aiagent::permissions::PermissionMode;
using aiagent::permissions::PolicyForMode;

void TestPolicyMatrix() {
    const auto confirm = PolicyForMode(PermissionMode::kConfirm);
    Expect(confirm.allow_read, "confirm allows read");
    Expect(confirm.allow_write, "confirm allows write");
    Expect(!confirm.allow_outside, "confirm denies outside");
    Expect(confirm.allow_command, "confirm allows command tool");
    Expect(confirm.require_confirm_write, "confirm requires write approval");
    Expect(confirm.require_confirm_command, "confirm requires command approval");

    const auto workspace = PolicyForMode(PermissionMode::kWorkspace);
    Expect(workspace.allow_read, "workspace allows read");
    Expect(workspace.allow_write, "workspace allows write");
    Expect(!workspace.allow_outside, "workspace denies outside");
    Expect(!workspace.require_confirm_write, "workspace does not require write approval");
    Expect(workspace.require_confirm_command, "workspace requires command approval");

    const auto full = PolicyForMode(PermissionMode::kFull);
    Expect(full.allow_write, "full allows write");
    Expect(full.allow_outside, "full allows outside");
    Expect(full.allow_command, "full allows command");
    Expect(!full.require_confirm_write, "full does not require write approval");
    Expect(!full.require_confirm_command, "full does not require command approval");
}

void TestParseMode() {
    using aiagent::permissions::ParseMode;
    Expect(ParseMode("confirm") == PermissionMode::kConfirm, "parse confirm");
    Expect(ParseMode("ask") == PermissionMode::kConfirm, "parse ask alias");
    Expect(ParseMode("  CONFIRM ") == PermissionMode::kConfirm, "parse is trim+case insensitive");
    Expect(ParseMode("workspace") == PermissionMode::kWorkspace, "parse workspace");
    Expect(ParseMode("ws") == PermissionMode::kWorkspace, "parse ws alias");
    Expect(ParseMode("write") == PermissionMode::kWorkspace, "parse write alias");
    Expect(ParseMode("full") == PermissionMode::kFull, "parse full");
    Expect(ParseMode("danger") == PermissionMode::kFull, "parse danger alias");
    Expect(!ParseMode("read-only").has_value(), "parse rejects unknown");
    Expect(!ParseMode("").has_value(), "parse rejects empty");
}

void TestNamesAndInheritance() {
    using aiagent::permissions::ModeInherits;
    using aiagent::permissions::ModeName;
    Expect(std::string(ModeName(PermissionMode::kConfirm)) == "confirm", "name confirm");
    Expect(std::string(ModeName(PermissionMode::kWorkspace)) == "workspace", "name workspace");
    Expect(std::string(ModeName(PermissionMode::kFull)) == "full", "name full");
    Expect(ModeInherits(PermissionMode::kConfirm), "confirm inherits");
    Expect(ModeInherits(PermissionMode::kWorkspace), "workspace inherits");
    Expect(!ModeInherits(PermissionMode::kFull), "full does not inherit");
}

void TestClampMode() {
    using aiagent::permissions::ClampMode;
    Expect(ClampMode(PermissionMode::kFull, PermissionMode::kWorkspace, true) ==
                   PermissionMode::kWorkspace,
           "full is clamped to max workspace");
    Expect(ClampMode(PermissionMode::kWorkspace, PermissionMode::kFull, true) ==
                   PermissionMode::kWorkspace,
           "workspace under max full is unchanged");
    Expect(ClampMode(PermissionMode::kFull, PermissionMode::kFull, false) ==
                   PermissionMode::kWorkspace,
           "full is downgraded when full access is disallowed");
    Expect(ClampMode(PermissionMode::kConfirm, PermissionMode::kFull, false) ==
                   PermissionMode::kConfirm,
           "confirm is unaffected by full-access flag");
}

}  // namespace

// [permissions] 的数值键：越界/非法必须报配置错误（不静默退回默认值）。
void TestPermissionsConfigParsing() {
    std::string error;
    const auto defaults = aiagent::ParsePermissionsConfig("[permissions]\nenabled = true\n", &error);
    Expect(defaults.has_value() && defaults->approval_timeout_seconds == 120,
           "approval timeout should default to 120s");

    error.clear();
    const auto ok =
            aiagent::ParsePermissionsConfig("[permissions]\napproval_timeout_seconds = 5\n", &error);
    Expect(ok.has_value() && ok->approval_timeout_seconds == 5,
           "in-range approval timeout should parse");

    error.clear();
    const auto zero =
            aiagent::ParsePermissionsConfig("[permissions]\napproval_timeout_seconds = 0\n", &error);
    Expect(!zero.has_value() && error.find("approval_timeout_seconds") != std::string::npos,
           "zero approval timeout should be rejected with the key in the message, got: " + error);

    error.clear();
    const auto too_large =
            aiagent::ParsePermissionsConfig("[permissions]\napproval_timeout_seconds = 99999\n", &error);
    Expect(!too_large.has_value() &&
                   error.find("[1, 3600]") != std::string::npos,
           "out-of-range approval timeout should be rejected, got: " + error);

    error.clear();
    const auto garbage =
            aiagent::ParsePermissionsConfig("[permissions]\napproval_timeout_seconds = abc\n", &error);
    Expect(!garbage.has_value() && error.find("invalid approval_timeout_seconds") != std::string::npos,
           "non-numeric approval timeout should be rejected, got: " + error);
}

int main() {
    TestPolicyMatrix();
    TestParseMode();
    TestNamesAndInheritance();
    TestClampMode();
    TestPermissionsConfigParsing();

    if (g_failures != 0) {
        std::cerr << "permission policy test failed with " << g_failures
                  << " failure(s)\n";
        return 1;
    }
    std::cout << "permission policy test passed\n";
    return 0;
}
