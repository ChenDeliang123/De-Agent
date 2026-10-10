#include "aiagent/permissions/permission_policy.hpp"

#include <cctype>
#include <string>

namespace aiagent::permissions {
namespace {

std::string Lower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

}  // namespace

PermissionPolicy PolicyForMode(PermissionMode mode) {
    PermissionPolicy policy;
    switch (mode) {
        case PermissionMode::kConfirm:
            policy.allow_write = true;
            policy.allow_command = true;
            policy.require_confirm_write = true;
            policy.require_confirm_command = true;
            break;
        case PermissionMode::kWorkspace:
            policy.allow_write = true;
            policy.allow_command = true;
            policy.require_confirm_command = true;
            break;
        case PermissionMode::kFull:
            policy.allow_write = true;
            policy.allow_outside = true;
            policy.allow_command = true;
            break;
    }
    return policy;
}

std::optional<PermissionMode> ParseMode(const std::string& text) {
    const std::string value = Lower(Trim(text));
    if (value == "confirm" || value == "ask") {
        return PermissionMode::kConfirm;
    }
    if (value == "workspace" || value == "ws" || value == "write") {
        return PermissionMode::kWorkspace;
    }
    if (value == "full" || value == "danger" || value == "unrestricted") {
        return PermissionMode::kFull;
    }
    return std::nullopt;
}

const char* ModeName(PermissionMode mode) {
    switch (mode) {
        case PermissionMode::kConfirm:
            return "confirm";
        case PermissionMode::kWorkspace:
            return "workspace";
        case PermissionMode::kFull:
            return "full";
    }
    return "confirm";
}

bool ModeInherits(PermissionMode mode) {
    return mode != PermissionMode::kFull;
}

std::string DescribePolicy(PermissionMode mode) {
    switch (mode) {
        case PermissionMode::kConfirm:
            return "file writes and command execution require approval; writes "
                   "outside the workspace are denied";
        case PermissionMode::kWorkspace:
            return "writes are allowed inside the workspace; command execution "
                   "requires approval; writes outside the workspace are denied";
        case PermissionMode::kFull:
            return "unrestricted file and shell access";
    }
    return {};
}

}  // namespace aiagent::permissions
