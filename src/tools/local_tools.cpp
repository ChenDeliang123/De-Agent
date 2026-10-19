#include "aiagent/tools/local_tools.hpp"

#include <json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "aiagent/platform/paths.hpp"
#include "aiagent/tools/run_command.hpp"
#include "aiagent/log.hpp"

namespace aiagent::tools {
namespace {

namespace fs = std::filesystem;

constexpr size_t kReadMaxBytes = 50 * 1024;
constexpr int kReadMaxLines = 2000;

// 只读检索工具（ls / glob / grep）的上限：输出字节、被搜索文件大小、
// 二进制嗅探长度、grep 单行长度。
constexpr size_t kSearchMaxBytes = 50 * 1024;
constexpr std::uintmax_t kSearchMaxFileBytes = 1024 * 1024;
constexpr size_t kSearchSniffBytes = 8192;
constexpr size_t kGrepMaxLineBytes = 500;

// 从头按字节截断，且不劈开 UTF-8 序列（read / glob / grep 共用）。
void TruncateUtf8Head(std::string* text, size_t max_bytes, bool* truncated) {
    if (text->size() <= max_bytes) {
        return;
    }
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>((*text)[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    text->resize(cut);
    *truncated = true;
}

// 读取文本并按 offset/limit 切片，再按行/字节截断（头部保留）。
std::string ReadSliced(const std::string& content, const nlohmann::json& arguments,
                       bool* truncated) {
    *truncated = false;

    std::vector<std::string> lines;
    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }

    long offset = 1;
    long limit = -1;  // -1 表示不限
    if (arguments.contains("offset") && arguments["offset"].is_number_integer()) {
        offset = arguments["offset"].get<long>();
    }
    if (arguments.contains("limit") && arguments["limit"].is_number_integer()) {
        limit = arguments["limit"].get<long>();
    }
    if (offset < 1) {
        offset = 1;
    }

    const size_t begin = static_cast<size_t>(offset - 1);
    size_t end = lines.size();
    if (limit >= 0 && begin + static_cast<size_t>(limit) < end) {
        end = begin + static_cast<size_t>(limit);
    }

    std::ostringstream out;
    size_t total_lines = 0;
    for (size_t i = begin; i < end && total_lines < static_cast<size_t>(kReadMaxLines); ++i) {
        if (i > begin) {
            out << '\n';
        }
        out << lines[i];
        ++total_lines;
    }
    if (end - begin > total_lines) {
        *truncated = true;
    }

    std::string text = out.str();
    TruncateUtf8Head(&text, kReadMaxBytes, truncated);
    return text;
}

std::string ReadFile(const std::string& path, bool* ok, std::string* error) {
    std::ifstream input(aiagent::platform::FromUtf8(path), std::ios::binary);
    if (!input) {
        *ok = false;
        *error = "cannot open file: " + path;
        return {};
    }
    std::ostringstream content;
    content << input.rdbuf();
    *ok = true;
    return content.str();
}

// child 是否位于 root 之内（或等于 root）。按路径分量比较，
// 避免 /foo 误判 /foobar。
bool PathComponentEqual(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
#else
    return left == right;
#endif
}

bool IsWithin(const fs::path& child, const fs::path& root) {
    if (root.empty()) {
        return false;
    }
    auto root_it = root.begin();
    auto child_it = child.begin();
    for (; root_it != root.end(); ++root_it, ++child_it) {
        if (child_it == child.end() ||
            !PathComponentEqual(*child_it, *root_it)) {
            return false;
        }
    }
    return true;
}

// canonical 化；失败（如路径不可访问）时退回 lexically_normal，不抛异常。
fs::path CanonicalOrNormal(const fs::path& path) {
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(path, ec);
    if (ec) {
        return path.lexically_normal();
    }
    return canonical;
}

// ---------- 只读检索工具（ls / glob / grep）的公共实现 ----------

std::string NormalizeGlobPattern(std::string pattern) {
#ifdef _WIN32
    // 模型/用户可能写 `**\*.cpp`；统一成模型可见的 '/' 形式。
    std::replace(pattern.begin(), pattern.end(), '\\', '/');
#endif
    return pattern;
}

// 把 glob（`*` / `?` / `**`，以 '/' 分隔）编译成锚定的 ECMAScript 正则源：
// `**` 匹配任意多段（含 0 段），`*` / `?` 只在单个路径段内匹配。
std::string GlobToRegexSource(const std::string& pattern) {
    constexpr char kRegexSpecials[] = ".^$|()[]{}+\\*?";

    std::string out = "^";
    size_t i = 0;
    while (i < pattern.size()) {
        const char c = pattern[i];
        if (c == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                while (i < pattern.size() && pattern[i] == '*') {
                    ++i;
                }
                if (i < pattern.size() && pattern[i] == '/') {
                    ++i;
                    out += "(?:[^/]+/)*";
                } else {
                    out += ".*";
                }
            } else {
                ++i;
                out += "[^/]*";
            }
        } else if (c == '?') {
            ++i;
            out += "[^/]";
        } else {
            ++i;
            if (std::strchr(kRegexSpecials, c) != nullptr) {
                out += '\\';
            }
            out += c;
        }
    }
    out += '$';
    return out;
}

// 递归遍历目录（DFS，同层按文件名字典序、目录与文件混排）。
// 跳过所有以 '.' 开头的条目（隐藏文件与目录），不进入符号链接目录。
// visit(abs, relative, is_dir, is_file) 返回 false 时提前结束遍历；
// is_dir 只对真实目录为真，is_file 跟随文件符号链接（目录符号链接两者皆为假）。
// max_depth <= 0 表示不限深度。
template <typename Visit>
bool WalkTree(const fs::path& dir, const fs::path& relative, int depth, int max_depth,
              const Visit& visit) {
    std::error_code iterator_error;
    const fs::directory_iterator iterator(
            dir, fs::directory_options::skip_permission_denied, iterator_error);
    if (iterator_error) {
        return true;  // 打不开的目录直接跳过
    }

    std::vector<fs::directory_entry> children;
    for (const fs::directory_entry& entry : iterator) {
        if (aiagent::platform::IsHiddenPath(entry.path())) {
            continue;
        }
        children.push_back(entry);
    }
    std::sort(children.begin(), children.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) {
                  return aiagent::platform::ToGenericUtf8(a.path().filename()) <
                         aiagent::platform::ToGenericUtf8(b.path().filename());
              });

    for (const fs::directory_entry& child : children) {
        std::error_code status_error;
        const fs::file_status link_status = child.symlink_status(status_error);
        const bool is_dir = !status_error && fs::is_directory(link_status);
        bool is_file = false;
        if (!is_dir) {
            std::error_code file_error;
            is_file = fs::is_regular_file(child.status(file_error));
        }
        const fs::path child_relative = relative / child.path().filename();
        if (!visit(child.path(), child_relative, is_dir, is_file)) {
            return false;
        }
        if (is_dir && (max_depth <= 0 || depth + 1 < max_depth)) {
            if (!WalkTree(child.path(), child_relative, depth + 1, max_depth, visit)) {
                return false;
            }
        }
    }
    return true;
}

// 检索根必须存在且是目录。
bool CheckSearchRoot(const std::string& path, std::string* error) {
    std::error_code status_error;
    const fs::file_status status =
            fs::status(aiagent::platform::FromUtf8(path), status_error);
    if (status_error || !fs::exists(status)) {
        *error = "path does not exist: " + path;
        return false;
    }
    if (!fs::is_directory(status)) {
        *error = "not a directory: " + path;
        return false;
    }
    return true;
}

long IntArgument(const nlohmann::json& arguments, const char* name, long fallback) {
    const auto it = arguments.find(name);
    if (it != arguments.end() && it->is_number_integer()) {
        return it->get<long>();
    }
    return fallback;
}

bool FlagArgument(const nlohmann::json& arguments, const char* name, bool fallback) {
    const auto it = arguments.find(name);
    if (it != arguments.end() && it->is_boolean()) {
        return it->get<bool>();
    }
    return fallback;
}

std::string JoinLines(const std::vector<std::string>& lines, size_t count) {
    std::ostringstream out;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            out << '\n';
        }
        out << lines[i];
    }
    return out.str();
}

std::string ToLowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

// ls：递归列出目录树，目录条目带结尾 '/'，路径相对根、每层字典序。
ToolExecutionResult RunLs(const std::string& root, const nlohmann::json& arguments) {
    ToolExecutionResult result;
    std::string error;
    if (!CheckSearchRoot(root, &error)) {
        result.error = error;
        return result;
    }

    long depth = IntArgument(arguments, "depth", 2);
    depth = std::max<long>(1, std::min<long>(4, depth));
    long limit = IntArgument(arguments, "limit", 200);
    limit = std::max<long>(1, limit);

    std::vector<std::string> lines;
    bool truncated = false;
    WalkTree(aiagent::platform::FromUtf8(root), fs::path(), 0, static_cast<int>(depth),
             [&](const fs::path&, const fs::path& relative, bool is_dir, bool) {
                 std::string line = aiagent::platform::ToGenericUtf8(relative);
                 if (is_dir) {
                     line += '/';
                 }
                 lines.push_back(std::move(line));
                 if (lines.size() > static_cast<size_t>(limit)) {
                     truncated = true;
                     return false;
                 }
                 return true;
             });
    if (truncated) {
        lines.resize(static_cast<size_t>(limit));
    }

    result.ok = true;
    result.text = lines.empty() ? "(empty)" : JoinLines(lines, lines.size());
    if (truncated) {
        result.text += "\n... (truncated)";
    }
    return result;
}

// glob：按 glob 模式匹配搜索根下的文件（只返回文件）。
ToolExecutionResult RunGlob(const std::string& root, const std::string& pattern,
                            const nlohmann::json& arguments) {
    ToolExecutionResult result;
    if (pattern.empty()) {
        result.error = "glob requires a non-empty pattern";
        return result;
    }

    const std::string normalized_pattern = NormalizeGlobPattern(pattern);
    std::regex expression;
    try {
        expression.assign(GlobToRegexSource(normalized_pattern),
                          std::regex_constants::ECMAScript);
    } catch (const std::regex_error& regex_error) {
        result.error = "invalid glob pattern: " + pattern + " (" + regex_error.what() + ")";
        return result;
    }

    std::string error;
    if (!CheckSearchRoot(root, &error)) {
        result.error = error;
        return result;
    }

    long limit = IntArgument(arguments, "limit", 200);
    limit = std::max<long>(1, limit);

    std::vector<std::string> matches;
    bool truncated = false;
    WalkTree(aiagent::platform::FromUtf8(root), fs::path(), 0, 0,
             [&](const fs::path&, const fs::path& relative, bool, bool is_file) {
                 if (!is_file) {
                     return true;
                 }
                 const std::string relative_text = aiagent::platform::ToGenericUtf8(relative);
                 if (!std::regex_search(relative_text, expression)) {
                     return true;
                 }
                 matches.push_back(relative_text);
                 if (matches.size() > static_cast<size_t>(limit)) {
                     truncated = true;
                     return false;
                 }
                 return true;
             });
    if (truncated) {
        matches.resize(static_cast<size_t>(limit));
    }
    if (matches.empty()) {
        result.ok = true;
        result.text = "no matches";
        return result;
    }

    std::sort(matches.begin(), matches.end());
    std::string text = JoinLines(matches, matches.size());
    bool bytes_truncated = false;
    TruncateUtf8Head(&text, kSearchMaxBytes, &bytes_truncated);
    if (truncated || bytes_truncated) {
        text += "\n... (truncated)";
    }
    result.ok = true;
    result.text = std::move(text);
    return result;
}

// grep：逐行搜索目录下的文本文件，输出 `相对路径:行号: 行内容`。
ToolExecutionResult RunGrep(const std::string& root, const std::string& pattern,
                            const nlohmann::json& arguments) {
    ToolExecutionResult result;
    if (pattern.empty()) {
        result.error = "grep requires a non-empty pattern";
        return result;
    }

    const bool ignore_case = FlagArgument(arguments, "ignore_case", false);
    const bool literal = FlagArgument(arguments, "literal", false);
    long max_results = IntArgument(arguments, "max_results", 100);
    max_results = std::max<long>(1, max_results);

    std::regex expression;
    if (!literal) {
        try {
            std::regex::flag_type flags = std::regex_constants::ECMAScript;
            if (ignore_case) {
                flags |= std::regex_constants::icase;
            }
            expression.assign(pattern, flags);
        } catch (const std::regex_error& regex_error) {
            result.error = "invalid pattern: " + pattern + " (" + regex_error.what() + ")";
            return result;
        }
    }
    const std::string folded_pattern =
            literal && ignore_case ? ToLowerAscii(pattern) : std::string();

    // include：不含 '/' 时按文件名匹配，否则按相对路径匹配。
    const std::string include =
            NormalizeGlobPattern(arguments.value("include", std::string()));
    bool include_on_basename = false;
    std::regex include_expression;
    if (!include.empty()) {
        include_on_basename = include.find('/') == std::string::npos;
        try {
            include_expression.assign(GlobToRegexSource(include),
                                      std::regex_constants::ECMAScript);
        } catch (const std::regex_error& regex_error) {
            result.error =
                    "invalid include pattern: " + include + " (" + regex_error.what() + ")";
            return result;
        }
    }

    std::string error;
    if (!CheckSearchRoot(root, &error)) {
        result.error = error;
        return result;
    }

    std::vector<std::string> matches;
    bool truncated = false;
    WalkTree(aiagent::platform::FromUtf8(root), fs::path(), 0, 0,
             [&](const fs::path& absolute, const fs::path& relative, bool, bool is_file) {
                 if (!is_file) {
                     return true;
                 }
                 const std::string relative_text = aiagent::platform::ToGenericUtf8(relative);
                 if (!include.empty()) {
                     const std::string target = include_on_basename
                                                        ? aiagent::platform::ToGenericUtf8(relative.filename())
                                                        : relative_text;
                     if (!std::regex_search(target, include_expression)) {
                         return true;
                     }
                 }

                 std::error_code size_error;
                 const std::uintmax_t size = fs::file_size(absolute, size_error);
                 if (size_error || size > kSearchMaxFileBytes) {
                     return true;  // 拿不到大小或超过 1MiB 的文件跳过
                 }
                 std::ifstream input(absolute, std::ios::binary);
                 if (!input) {
                     return true;
                 }
                 std::ostringstream buffer;
                 buffer << input.rdbuf();
                 const std::string content = buffer.str();

                 const size_t nul = content.find('\0');
                 if (nul != std::string::npos && nul < kSearchSniffBytes) {
                     return true;  // 二进制文件跳过
                 }

                 long line_number = 0;
                 std::istringstream lines(content);
                 std::string line;
                 while (std::getline(lines, line)) {
                     ++line_number;
                     line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
                     bool matched = false;
                     if (literal) {
                         matched = ignore_case ? ToLowerAscii(line).find(folded_pattern) !=
                                                          std::string::npos
                                               : line.find(pattern) != std::string::npos;
                     } else {
                         matched = std::regex_search(line, expression);
                     }
                     if (!matched) {
                         continue;
                     }
                     bool line_truncated = false;
                     TruncateUtf8Head(&line, kGrepMaxLineBytes, &line_truncated);
                     if (line_truncated) {
                         line += "...";
                     }
                     matches.push_back(relative_text + ":" + std::to_string(line_number) +
                                       ": " + line);
                     if (matches.size() > static_cast<size_t>(max_results)) {
                         truncated = true;
                         return false;
                     }
                 }
                 return true;
             });
    if (truncated) {
        matches.resize(static_cast<size_t>(max_results));
    }
    if (matches.empty()) {
        result.ok = true;
        result.text = "no matches";
        return result;
    }

    std::string text = JoinLines(matches, matches.size());
    bool bytes_truncated = false;
    TruncateUtf8Head(&text, kSearchMaxBytes, &bytes_truncated);
    if (truncated || bytes_truncated) {
        text += "\n... (truncated)";
    }
    result.ok = true;
    result.text = std::move(text);
    return result;
}

}  // namespace

LocalCodingTools::LocalCodingTools(std::string working_directory,
                                   std::vector<std::string> extra_roots)
    : cwd_(std::move(working_directory)), extra_roots_(std::move(extra_roots)) {}

bool LocalCodingTools::ResolvePath(const std::string& path,
                                   const std::string& working_directory,
                                   const permissions::PermissionPolicy& policy,
                                   std::string* resolved,
                                   std::string* error) const {
    if (path.empty()) {
        if (error != nullptr) {
            *error = "path must not be empty";
        }
        return false;
    }

    const fs::path base = working_directory.empty()
                                  ? aiagent::platform::FromUtf8(cwd_)
                                  : aiagent::platform::FromUtf8(working_directory);
    fs::path target = aiagent::platform::FromUtf8(path);
    if (!target.is_absolute()) {
        target = base / target;
    }
    const fs::path effective = CanonicalOrNormal(target);

    if (policy.allow_outside) {
        *resolved = aiagent::platform::ToUtf8(effective);
        return true;
    }

    std::vector<fs::path> roots;
    roots.push_back(CanonicalOrNormal(base));
    for (const std::string& extra : extra_roots_) {
        if (extra.empty()) {
            continue;
        }
        fs::path root = aiagent::platform::FromUtf8(extra);
        if (!root.is_absolute()) {
            root = base / root;
        }
        roots.push_back(CanonicalOrNormal(root));
    }

    for (const fs::path& root : roots) {
        if (IsWithin(effective, root)) {
            *resolved = aiagent::platform::ToUtf8(effective);
            return true;
        }
    }

    if (error != nullptr) {
        *error = "path escapes sandbox: " + path;
    }
    return false;
}

std::vector<llm::ToolSpec> LocalCodingTools::ListTools() {
    using nlohmann::json;

    llm::ToolSpec read;
    read.name = "read";
    read.description =
            "Read the contents of a file. For text files, output is truncated to 2000 lines "
            "or 50KB (whichever is hit first). Use offset/limit for large files.";
    read.parameters = {{"type", "object"},
                       {"properties",
                        {{"path", {{"type", "string"},
                                   {"description", "Path to the file to read (relative or absolute)"}}},
                         {"offset", {{"type", "integer"},
                                     {"description", "Line number to start reading from (1-indexed)"}}},
                         {"limit", {{"type", "integer"},
                                    {"description", "Maximum number of lines to read"}}}}},
                       {"required", json::array({"path"})}};

    llm::ToolSpec write;
    write.name = "write";
    write.description = "Write content to a file, overwriting it if it exists.";
    write.parameters = {{"type", "object"},
                        {"properties",
                         {{"path", {{"type", "string"},
                                    {"description", "Path to the file to write (relative or absolute)"}}},
                          {"content", {{"type", "string"},
                                       {"description", "Full content to write"}}}}},
                        {"required", json::array({"path", "content"})}};

    llm::ToolSpec edit;
    edit.name = "edit";
    edit.description =
            "Replace exact text in a file. When several independent places in the same file "
            "change, use ONE call with an edits[] array instead of many single edits. Each "
            "oldText must match exactly once in the ORIGINAL file and blocks must not overlap; "
            "the whole call is rejected (nothing written) if any block fails.";
    const json edit_block_properties = {
            {"oldText",
             {{"type", "string"},
              {"description", "Exact text to replace (must be unique in the original file)"}}},
            {"newText",
             {{"type", "string"},
              {"description", "Replacement text (empty deletes oldText)"}}}};
    edit.parameters = {
            {"type", "object"},
            {"properties",
             {{"path",
               {{"type", "string"},
                {"description", "Path to the file to edit (relative or absolute)"}}},
              {"oldText",
               {{"type", "string"},
                {"description",
                 "Single replacement: exact text to replace (must be unique in the file)"}}},
              {"newText",
               {{"type", "string"},
                {"description",
                 "Single replacement: replacement text (empty deletes oldText)"}}},
              {"edits",
               {{"type", "array"},
                {"description",
                 "Multiple replacements applied atomically; every oldText is matched against "
                 "the original file."},
                {"items",
                 {{"type", "object"},
                  {"properties", edit_block_properties},
                  {"required", json::array({"oldText"})}}}}}}},
            {"required", json::array({"path"})}};

    llm::ToolSpec bash;
    bash.name = "bash";
    bash.description =
            "Execute a shell command in the working directory. POSIX uses /bin/sh -c; "
            "Windows uses cmd.exe /d /s /c. Returns combined stdout and stderr, truncated "
            "to the last 2000 lines or 50KB. Optionally provide a timeout in seconds "
            "(default 60). For aggregation tasks (line or file counts, size or symbol "
            "statistics, enumerating matches) prefer ONE pipeline that prints the summary "
            "(e.g. `git ls-files | xargs wc -l`) over several small commands or a helper "
            "script.";
    bash.parameters = {{"type", "object"},
                       {"properties",
                        {{"command", {{"type", "string"},
                                      {"description", "Shell command to execute"}}},
                         {"timeout", {{"type", "integer"},
                                      {"description", "Timeout in seconds (optional, default 60)"}}}}},
                       {"required", json::array({"command"})}};

    llm::ToolSpec ls;
    ls.name = "ls";
    ls.description =
            "List a directory tree. Entries are paths relative to the listed root (directories "
            "end with '/'), sorted by name within each level. Hidden files and directories "
            "(dot-prefix or Windows hidden attribute) are skipped. Depth is clamped to 1..4 (default 2) and at "
            "most 200 entries are returned (default).";
    ls.parameters = {{"type", "object"},
                     {"properties",
                      {{"path", {{"type", "string"},
                                 {"description", "Directory to list (relative or absolute, "
                                                 "defaults to the working directory)"}}},
                       {"depth", {{"type", "integer"},
                                  {"description", "Recursion depth, clamped to 1..4 (default 2)"}}},
                       {"limit", {{"type", "integer"},
                                  {"description", "Maximum number of entries (default 200)"}}}}},
                     {"required", json::array()}};

    llm::ToolSpec glob;
    glob.name = "glob";
    glob.description =
            "Find files by glob pattern ('*', '?' and '**' are supported; '**' also matches zero "
            "path segments). Only files are returned, as sorted paths relative to the search "
            "root. Hidden files and directories (dot-prefix or Windows hidden attribute) are skipped. At most "
            "200 matches (default) are returned and output is truncated to 50KB.";
    glob.parameters = {{"type", "object"},
                       {"properties",
                        {{"pattern", {{"type", "string"},
                                      {"description", "Glob pattern to match, e.g. '**/*.cpp'"}}},
                         {"path", {{"type", "string"},
                                   {"description", "Directory to search (relative or absolute, "
                                                   "defaults to the working directory)"}}},
                         {"limit", {{"type", "integer"},
                                    {"description", "Maximum number of matches (default 200)"}}}}},
                       {"required", json::array({"pattern"})}};

    llm::ToolSpec grep;
    grep.name = "grep";
    grep.description =
            "Search file contents line by line under a directory. By default pattern is an "
            "ECMAScript regular expression; set literal=true for plain substring matching. "
            "Hidden files and directories (dot-prefix or Windows hidden attribute) are "
            "skipped, as are binary files and files larger than 1MB. Results are reported as "
            "'path:line: content' "
            "(content truncated to 500 characters); at most 100 results (default) are returned "
            "and output is truncated to 50KB.";
    grep.parameters = {
            {"type", "object"},
            {"properties",
             {{"pattern", {{"type", "string"},
                           {"description", "Text or regular expression to search for"}}},
              {"path", {{"type", "string"},
                        {"description", "Directory to search (relative or absolute, defaults to "
                                        "the working directory)"}}},
              {"include", {{"type", "string"},
                           {"description", "Glob filter on file paths, e.g. '*.cpp'"}}},
              {"max_results", {{"type", "integer"},
                               {"description", "Maximum number of matching lines (default 100)"}}},
              {"ignore_case", {{"type", "boolean"}, {"description", "Case-insensitive match"}}},
              {"literal", {{"type", "boolean"},
                           {"description", "Treat pattern as a literal substring instead of a "
                                           "regular expression"}}}}},
            {"required", json::array({"pattern"})}};

    return {read, write, edit, bash, ls, glob, grep};
}

ToolExecutionResult LocalCodingTools::Execute(const std::string& name,
                                              const nlohmann::json& arguments,
                                              const ToolContext& context) {
    ToolExecutionResult result;
    const std::string& working_directory = context.working_directory;

    if (name == "read") {
        std::string path;
        std::string resolve_error;
        if (!ResolvePath(arguments.value("path", std::string()), working_directory,
                         context.policy, &path, &resolve_error)) {
            result.error = resolve_error;
            return result;
        }
        aiagent::DefaultLogger().Log(aiagent::LogLevel::kDebug, "tools",
                                     "tool=read path=" + path + " event=start");
        bool ok = false;
        std::string error;
        const std::string content = ReadFile(path, &ok, &error);
        if (!ok) {
            result.error = error;
            return result;
        }
        bool truncated = false;
        std::string text = ReadSliced(content, arguments, &truncated);
        if (truncated) {
            text += "\n... (truncated)";
        }
        result.ok = true;
        result.text = text;
        aiagent::DefaultLogger().Log(aiagent::LogLevel::kInfo, "tools",
                                     "tool=read path=" + path +
                                             " event=end ok=true bytes=" +
                                             std::to_string(text.size()));
        return result;
    }

    if (name == "write") {
        std::string path;
        std::string resolve_error;
        if (!ResolvePath(arguments.value("path", std::string()), working_directory,
                         context.policy, &path, &resolve_error)) {
            result.error = resolve_error;
            return result;
        }
        const std::string content = arguments.value("content", std::string());
        aiagent::DefaultLogger().Log(aiagent::LogLevel::kDebug, "tools",
                                     "tool=write path=" + path +
                                             " event=start content_bytes=" +
                                             std::to_string(content.size()));
        std::ofstream output(aiagent::platform::FromUtf8(path), std::ios::binary | std::ios::trunc);
        if (!output) {
            result.error = "cannot open file for write: " + path;
            return result;
        }
        output << content;
        output.close();
        if (!output) {
            result.error = "failed to write file: " + path;
            return result;
        }
        result.ok = true;
        result.text = "wrote " + std::to_string(content.size()) + " bytes to " + path;
        aiagent::DefaultLogger().Log(aiagent::LogLevel::kInfo, "tools",
                                     "tool=write path=" + path +
                                             " event=end ok=true content_bytes=" +
                                             std::to_string(content.size()));
        return result;
    }

    if (name == "edit") {
        std::string path;
        std::string resolve_error;
        if (!ResolvePath(arguments.value("path", std::string()), working_directory,
                         context.policy, &path, &resolve_error)) {
            result.error = resolve_error;
            return result;
        }

        // 参数形态：旧式 {path, oldText, newText} 或新式 {path, edits:[{oldText,newText}]}。
        // 两者不允许混用，避免"到底改了哪些块"产生歧义。
        struct EditBlock {
            std::string old_text;
            std::string new_text;
        };
        std::vector<EditBlock> blocks;
        const bool has_edits_array =
                arguments.contains("edits") && arguments["edits"].is_array();
        if (has_edits_array) {
            if (arguments.contains("oldText")) {
                result.error =
                        "edit accepts either edits[] or oldText/newText, not both";
                return result;
            }
            const auto& edits = arguments["edits"];
            if (edits.empty()) {
                result.error = "edit requires a non-empty edits[] array";
                return result;
            }
            for (std::size_t i = 0; i < edits.size(); ++i) {
                const auto& item = edits[i];
                if (!item.is_object()) {
                    result.error = "edits[" + std::to_string(i) + "] must be an object";
                    return result;
                }
                EditBlock block;
                block.old_text = item.value("oldText", std::string());
                block.new_text = item.value("newText", std::string());
                if (block.old_text.empty()) {
                    result.error = "edits[" + std::to_string(i) +
                                   "].oldText must be non-empty";
                    return result;
                }
                blocks.push_back(std::move(block));
            }
        } else {
            EditBlock block;
            block.old_text = arguments.value("oldText", std::string());
            block.new_text = arguments.value("newText", std::string());
            if (block.old_text.empty()) {
                result.error = "edit requires path and non-empty oldText (or an edits[] array)";
                return result;
            }
            blocks.push_back(std::move(block));
        }

        aiagent::DefaultLogger().Log(aiagent::LogLevel::kDebug, "tools",
                                     "tool=edit path=" + path +
                                             " blocks=" + std::to_string(blocks.size()) +
                                             " event=start");
        bool ok = false;
        std::string error;
        const std::string content = ReadFile(path, &ok, &error);
        if (!ok) {
            result.error = error;
            return result;
        }

        // 先在原文上校验每一块（恰好匹配一次），再检查区间不重叠；
        // 全部通过才写盘 —— 任一块失败就整体拒绝，避免"改了一半"。
        struct Span {
            std::size_t start = 0;
            std::size_t end = 0;
            std::size_t block = 0;
        };
        std::vector<Span> spans;
        spans.reserve(blocks.size());
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const std::string& old_text = blocks[i].old_text;
            const std::size_t first = content.find(old_text);
            if (first == std::string::npos) {
                result.error = "edits[" + std::to_string(i) + "].oldText not found in file: " +
                               path;
                return result;
            }
            if (content.find(old_text, first + old_text.size()) != std::string::npos) {
                result.error = "edits[" + std::to_string(i) +
                               "].oldText is not unique in file: " + path;
                return result;
            }
            spans.push_back(Span{first, first + old_text.size(), i});
        }
        std::sort(spans.begin(), spans.end(),
                  [](const Span& a, const Span& b) { return a.start < b.start; });
        for (std::size_t i = 1; i < spans.size(); ++i) {
            if (spans[i].start < spans[i - 1].end) {
                result.error = "edits[" + std::to_string(spans[i - 1].block) + "] and edits[" +
                               std::to_string(spans[i].block) + "] overlap in file: " + path;
                return result;
            }
        }

        std::string updated = content;
        // 从后往前替换，前面的下标不会失效。
        for (std::size_t i = spans.size(); i-- > 0;) {
            const Span& span = spans[i];
            updated.replace(span.start, span.end - span.start,
                            blocks[span.block].new_text);
        }
        std::ofstream output(aiagent::platform::FromUtf8(path), std::ios::binary | std::ios::trunc);
        if (!output) {
            result.error = "cannot open file for write: " + path;
            return result;
        }
        output << updated;
        output.close();
        result.ok = true;
        result.text = "edited " + path + ": " + std::to_string(blocks.size()) +
                      " replacement(s)";
        aiagent::DefaultLogger().Log(aiagent::LogLevel::kInfo, "tools",
                                     "tool=edit path=" + path +
                                             " blocks=" + std::to_string(blocks.size()) +
                                             " event=end ok=true");
        return result;
    }

    if (name == "bash") {
        const std::string command = arguments.value("command", std::string());
        if (command.empty()) {
            result.error = "bash requires a command";
            return result;
        }
        aiagent::DefaultLogger().Log(aiagent::LogLevel::kDebug, "tools",
                                     "tool=bash event=start command_bytes=" +
                                             std::to_string(command.size()));
        long timeout = 60;
        if (arguments.contains("timeout") && arguments["timeout"].is_number_integer()) {
            timeout = arguments["timeout"].get<long>();
        }
        const std::string run_cwd = working_directory.empty() ? cwd_ : working_directory;
        const CommandResult command_result = RunCommand(command, run_cwd, timeout);
        result.ok = command_result.ok;
        result.text = command_result.output;
        result.error = command_result.error;
        return result;
    }

    if (name == "ls" || name == "glob" || name == "grep") {
        // path 省略（或为空）时以 working_directory（空则 cwd_）为搜索根。
        std::string raw_path = arguments.value("path", std::string());
        if (raw_path.empty()) {
            raw_path = working_directory.empty() ? cwd_ : working_directory;
        }
        std::string path;
        std::string resolve_error;
        if (!ResolvePath(raw_path, working_directory, context.policy, &path,
                         &resolve_error)) {
            result.error = resolve_error;
            return result;
        }
        aiagent::DefaultLogger().Log(aiagent::LogLevel::kDebug, "tools",
                                     "tool=" + name + " path=" + path + " event=start");
        ToolExecutionResult search_result;
        if (name == "ls") {
            search_result = RunLs(path, arguments);
        } else if (name == "glob") {
            search_result =
                    RunGlob(path, arguments.value("pattern", std::string()), arguments);
        } else {
            search_result =
                    RunGrep(path, arguments.value("pattern", std::string()), arguments);
        }
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kInfo, "tools",
                "tool=" + name + " path=" + path +
                        " event=end ok=" + (search_result.ok ? std::string("true")
                                                            : std::string("false")) +
                        " bytes=" + std::to_string(search_result.text.size()));
        return search_result;
    }

    result.error = "unknown tool: " + name;
    return result;
}

}  // namespace aiagent::tools
