#pragma once

#include <regex>
#include <string>
#include <vector>

namespace aiagent::permissions {

// 一条 bash 命令规则。
//   - token 规则：按「shell 令牌前缀」匹配（空格分隔的连续 token）。
//   - regex 规则：pattern 以 "re:" 开头，用正则匹配整条命令。
struct BashRule {
    bool is_regex = false;
    bool valid = true;  // 非法正则会置为 false 并被跳过
    std::string pattern;
    std::vector<std::string> tokens;  // 仅 token 规则使用
    std::regex regex;                 // 仅 regex 且 valid 时使用
};

// 一份 bash 命令策略：黑名单（deny）与白名单（allow）。
struct BashPolicy {
    std::vector<BashRule> deny;
    std::vector<BashRule> allow;

    bool empty() const noexcept { return deny.empty() && allow.empty(); }
};

// 单条命令的裁决结果。
enum class BashDecision {
    kDeny,         // 命中黑名单：永不执行
    kAllow,        // 命中白名单：免确认执行
    kFallthrough,  // 都不命中：交给权限模式决定
};

// 解析规则文件文本。空行与 '#' 开头的行忽略；非法规则被跳过。
BashPolicy ParseBashPolicy(const std::string& content);

// 内置默认策略与默认文件内容（首次运行时写入的内容，带注释）。
const BashPolicy& DefaultBashPolicy();
const char* DefaultBashPolicyFileContent();

// 读取规则文件；成功返回 true。文件不存在/无法打开时返回 false。
bool LoadBashPolicyFile(const std::string& path, BashPolicy* out, std::string* error);

// 写出内置默认文件（含注释）；父目录按需创建。成功返回 true。
bool WriteDefaultBashPolicyFile(const std::string& path);

// 裁决一条 bash 命令。判定顺序：deny（任一分段命中/任意正则命中）
// > allow（全文正则命中，或每个分段都命中某条 token 规则）> 回落。
BashDecision EvaluateBashPolicy(const std::string& command, const BashPolicy& policy);

}  // namespace aiagent::permissions
