#include "aiagent/tools/local_tools.hpp"

#include <json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "aiagent/tools/run_command.hpp"
#include "aiagent/log.hpp"

namespace aiagent::tools {
namespace {

namespace fs = std::filesystem;

constexpr size_t kReadMaxBytes = 50 * 1024;
constexpr int kReadMaxLines = 2000;

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
    if (text.size() > kReadMaxBytes) {
        size_t cut = kReadMaxBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        text = text.substr(0, cut);
        *truncated = true;
    }
    return text;
}

std::string ReadFile(const std::string& path, bool* ok, std::string* error) {
    std::ifstream input(path, std::ios::binary);
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
bool IsWithin(const fs::path& child, const fs::path& root) {
    if (root.empty()) {
        return false;
    }
    auto root_it = root.begin();
    auto child_it = child.begin();
    for (; root_it != root.end(); ++root_it, ++child_it) {
        if (child_it == child.end() || *child_it != *root_it) {
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

    const fs::path base =
            working_directory.empty() ? fs::path(cwd_) : fs::path(working_directory);
    fs::path target(path);
    if (!target.is_absolute()) {
        target = base / target;
    }
    const fs::path effective = CanonicalOrNormal(target);

    if (policy.allow_outside) {
        *resolved = effective.string();
        return true;
    }

    std::vector<fs::path> roots;
    roots.push_back(CanonicalOrNormal(base));
    for (const std::string& extra : extra_roots_) {
        if (extra.empty()) {
            continue;
        }
        fs::path root(extra);
        if (!root.is_absolute()) {
            root = base / root;
        }
        roots.push_back(CanonicalOrNormal(root));
    }

    for (const fs::path& root : roots) {
        if (IsWithin(effective, root)) {
            *resolved = effective.string();
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
            "Execute a bash command in the working directory. Returns combined stdout and "
            "stderr, truncated to the last 2000 lines or 50KB. Optionally provide a timeout "
            "in seconds (default 60).";
    bash.parameters = {{"type", "object"},
                       {"properties",
                        {{"command", {{"type", "string"},
                                      {"description", "Bash command to execute"}}},
                         {"timeout", {{"type", "integer"},
                                      {"description", "Timeout in seconds (optional, default 60)"}}}}},
                       {"required", json::array({"command"})}};

    return {read, write, edit, bash};
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
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
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
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
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

    result.error = "unknown tool: " + name;
    return result;
}

}  // namespace aiagent::tools
