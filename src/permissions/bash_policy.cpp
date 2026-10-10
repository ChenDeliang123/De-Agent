#include "aiagent/permissions/bash_policy.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace aiagent::permissions {
namespace {

// 内置默认规则：首次运行时会以带注释的形式写入 bash_policy.rules。
constexpr const char* kDefaultRules = R"BASH(# ai-agent bash 命令策略（白名单 / 黑名单）
#
# 一行一条规则；空行与以 # 开头的行会被忽略。
#
# 语法：
#   deny  <pattern>    永不执行（优先级最高，人工确认也无法绕过）
#   allow <pattern>    免逐次确认；要求命令的每个分段都命中至少一条 allow
#
# 匹配方式：
#   - 默认按「shell 令牌前缀」匹配（空格分隔）。
#     例：`git status` 会命中 `git status -s`。
#   - 前缀加 `re:` 用正则匹配整条命令。例：`re: dd\s+.*of=/dev/`
#
# 判定顺序：deny > allow > 权限模式（confirm/workspace 需确认，full 放行）。
# 注意：字符串匹配不是安全边界，可能被 $IFS、引号、别名等绕过。

# ---------------- 黑名单：灾难级，永远拒绝 ----------------
# 磁盘 / 文件系统
deny re: \bmkfs(\.\w+)?\b
deny re: \b(mke2fs|fdisk|parted|wipefs|shred)\b
deny re: \bdd\b.*\bof=/dev/
# 关机 / 重启
deny shutdown
deny reboot
deny poweroff
deny halt
# 权限提升
deny sudo
deny su
# 系统级破坏
deny re: chmod\s+-R\s+777\s+/
deny re: \brm\b.*\s(/|~|\$HOME)(\s|$)
deny rm -rf .git
deny re: \bchown\b\s+-R\s+/\s*$
# 远程代码执行 / 反弹 shell
deny re: (curl|wget)[^|]*\|\s*(sudo\s+)?(sh|bash|zsh)
deny re: \beval\b.*\$\s*\(
deny nc
deny ncat
deny socat
# 服务 / 账号
deny crontab
deny systemctl
deny useradd
deny passwd
deny iptables
deny ufw
# Git 历史破坏
deny re: \bgit\s+push\b.*(--force|-f)\b
deny git reset --hard
deny git clean -fdx
deny git filter-branch
# fork bomb
deny re: :\(\)\s*\{.*\|.*&\s*\}\s*;\s*:
# 容器 / 云
deny re: (docker\s+system\s+prune|kubectl\s+delete|terraform\s+destroy)

# ---------------- 白名单：只读 / 无副作用，免确认 ----------------
# 导航 / 查看
allow pwd
allow ls
allow cat
allow head
allow tail
allow wc
allow file
allow stat
allow tree
allow du
allow df
allow readlink
allow realpath
# 搜索
allow grep
allow rg
allow fd
# 文本 / 环境
allow echo
allow printf
allow date
allow uname
allow whoami
allow id
allow hostname
allow which
allow env
allow printenv
# Git 只读
allow git status
allow git diff
allow git log
allow git show
allow git blame
allow git remote -v
allow git stash list
allow git tag
allow git describe
allow git rev-parse
allow git ls-files
# 版本查看
allow node --version
allow npm --version
allow python --version
allow python3 --version
allow cargo --version
allow go version
allow cmake --version
allow git --version
# 依赖列表
allow npm ls
allow pip list
allow pip show
allow cargo metadata
)BASH";

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

std::string Lower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

// 按空白切分。
std::vector<std::string> SplitWhitespace(const std::string& text) {
    std::vector<std::string> parts;
    std::istringstream input(text);
    std::string token;
    while (input >> token) {
        parts.push_back(std::move(token));
    }
    return parts;
}

// 判断 token 是否形如 KEY=value 的环境变量赋值。
bool IsEnvAssignment(const std::string& token) {
    const std::size_t eq = token.find('=');
    if (eq == std::string::npos || eq == 0) {
        return false;
    }
    const char first = token[0];
    if (!(std::isalpha(static_cast<unsigned char>(first)) != 0 || first == '_')) {
        return false;
    }
    for (std::size_t i = 1; i < eq; ++i) {
        const char c = token[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_')) {
            return false;
        }
    }
    return true;
}

// 把一条命令按 shell 分隔符拆成多段（启发式，不处理引号转义）。
// 过度切分是安全的：只会让 deny 更容易命中、allow 更难命中。
std::vector<std::vector<std::string>> SplitCommand(const std::string& command) {
    std::vector<std::string> raw_segments;
    std::string current;
    const auto flush = [&]() {
        raw_segments.push_back(current);
        current.clear();
    };
    for (std::size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (c == '\n' || c == ';') {
            flush();
        } else if (c == '&' && i + 1 < command.size() && command[i + 1] == '&') {
            flush();
            ++i;
        } else if (c == '|' && i + 1 < command.size() && command[i + 1] == '|') {
            flush();
            ++i;
        } else if (c == '|') {
            flush();
        } else {
            current.push_back(c);
        }
    }
    flush();

    std::vector<std::vector<std::string>> segments;
    for (const std::string& raw : raw_segments) {
        std::vector<std::string> tokens = SplitWhitespace(Trim(raw));
        std::size_t start = 0;
        while (start + 1 < tokens.size() && IsEnvAssignment(tokens[start])) {
            ++start;
        }
        std::vector<std::string> segment(tokens.begin() + start, tokens.end());
        if (!segment.empty()) {
            segments.push_back(std::move(segment));
        }
    }
    return segments;
}

bool TokenPrefixMatches(const std::vector<std::string>& segment,
                        const std::vector<std::string>& rule) {
    if (rule.empty() || segment.size() < rule.size()) {
        return false;
    }
    for (std::size_t i = 0; i < rule.size(); ++i) {
        if (segment[i] != rule[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

BashPolicy ParseBashPolicy(const std::string& content) {
    BashPolicy policy;
    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const std::size_t space = trimmed.find_first_of(" \t");
        if (space == std::string::npos) {
            continue;  // 需要 "deny <pattern>" 或 "allow <pattern>"
        }
        const std::string directive = Lower(Trim(trimmed.substr(0, space)));
        const std::string pattern = Trim(trimmed.substr(space + 1));
        if (pattern.empty() || (directive != "deny" && directive != "allow")) {
            continue;
        }
        BashRule rule;
        if (pattern.rfind("re:", 0) == 0) {
            rule.is_regex = true;
            rule.pattern = Trim(pattern.substr(3));
            if (rule.pattern.empty()) {
                continue;
            }
            try {
                rule.regex = std::regex(rule.pattern);
            } catch (const std::regex_error&) {
                rule.valid = false;
            }
        } else {
            rule.pattern = pattern;
            rule.tokens = SplitWhitespace(pattern);
            if (rule.tokens.empty()) {
                continue;
            }
        }
        if (directive == "deny") {
            policy.deny.push_back(std::move(rule));
        } else {
            policy.allow.push_back(std::move(rule));
        }
    }
    return policy;
}

const BashPolicy& DefaultBashPolicy() {
    static const BashPolicy policy = ParseBashPolicy(kDefaultRules);
    return policy;
}

const char* DefaultBashPolicyFileContent() {
    return kDefaultRules;
}

bool LoadBashPolicyFile(const std::string& path, BashPolicy* out, std::string* error) {
    if (path.empty()) {
        if (error != nullptr) {
            *error = "empty bash policy path";
        }
        return false;
    }
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open bash policy file: " + path;
        }
        return false;
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    if (out != nullptr) {
        *out = ParseBashPolicy(buffer.str());
    }
    return true;
}

bool WriteDefaultBashPolicyFile(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path target(path);
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        return false;
    }
    output << kDefaultRules;
    return output.good();
}

BashDecision EvaluateBashPolicy(const std::string& command, const BashPolicy& policy) {
    if (command.empty() || policy.empty()) {
        return BashDecision::kFallthrough;
    }
    const std::vector<std::vector<std::string>> segments = SplitCommand(command);

    // 1) deny：任一分段命中 token 规则，或任意正则命中整条命令。
    for (const BashRule& rule : policy.deny) {
        if (!rule.valid) {
            continue;
        }
        if (rule.is_regex) {
            if (std::regex_search(command, rule.regex)) {
                return BashDecision::kDeny;
            }
        } else {
            for (const std::vector<std::string>& segment : segments) {
                if (TokenPrefixMatches(segment, rule.tokens)) {
                    return BashDecision::kDeny;
                }
            }
        }
    }

    // 2) allow：全文正则命中，或每个分段都命中某条 token 规则。
    if (!policy.allow.empty()) {
        for (const BashRule& rule : policy.allow) {
            if (rule.is_regex && rule.valid && std::regex_search(command, rule.regex)) {
                return BashDecision::kAllow;
            }
        }
        if (!segments.empty()) {
            bool all_matched = true;
            for (const std::vector<std::string>& segment : segments) {
                bool matched = false;
                for (const BashRule& rule : policy.allow) {
                    if (!rule.is_regex && rule.valid &&
                        TokenPrefixMatches(segment, rule.tokens)) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    all_matched = false;
                    break;
                }
            }
            if (all_matched) {
                return BashDecision::kAllow;
            }
        }
    }

    return BashDecision::kFallthrough;
}

}  // namespace aiagent::permissions
