#include "aiagent/tools/local_tools.hpp"

#include <json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "aiagent/tools/run_command.hpp"

namespace aiagent::tools {
namespace {

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

}  // namespace

LocalCodingTools::LocalCodingTools(std::string working_directory)
    : cwd_(std::move(working_directory)) {}

std::string LocalCodingTools::ResolvePath(const std::string& path,
                                          const std::string& working_directory) const {
    const std::filesystem::path target(path);
    if (target.is_absolute()) {
        return path;
    }
    const std::string base = working_directory.empty() ? cwd_ : working_directory;
    return (std::filesystem::path(base) / target).string();
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
            "Replace a unique occurrence of oldText with newText in a file. The oldText must "
            "match exactly once; otherwise the edit is rejected.";
    edit.parameters = {{"type", "object"},
                       {"properties",
                        {{"path", {{"type", "string"},
                                   {"description", "Path to the file to edit (relative or absolute)"}}},
                         {"oldText", {{"type", "string"},
                                      {"description", "Exact text to replace (must be unique)"}}},
                         {"newText", {{"type", "string"},
                                      {"description", "Replacement text"}}}}},
                       {"required", json::array({"path", "oldText", "newText"})}};

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
                                              const std::string& working_directory) {
    ToolExecutionResult result;

    if (name == "read") {
        const std::string path =
                ResolvePath(arguments.value("path", std::string()), working_directory);
        if (path.empty()) {
            result.error = "read requires a path";
            return result;
        }
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
        return result;
    }

    if (name == "write") {
        const std::string path =
                ResolvePath(arguments.value("path", std::string()), working_directory);
        const std::string content = arguments.value("content", std::string());
        if (path.empty()) {
            result.error = "write requires a path";
            return result;
        }
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
        return result;
    }

    if (name == "edit") {
        const std::string path =
                ResolvePath(arguments.value("path", std::string()), working_directory);
        const std::string old_text = arguments.value("oldText", std::string());
        const std::string new_text = arguments.value("newText", std::string());
        if (path.empty() || old_text.empty()) {
            result.error = "edit requires path and non-empty oldText";
            return result;
        }
        bool ok = false;
        std::string error;
        const std::string content = ReadFile(path, &ok, &error);
        if (!ok) {
            result.error = error;
            return result;
        }
        const size_t first = content.find(old_text);
        if (first == std::string::npos) {
            result.error = "oldText not found in file: " + path;
            return result;
        }
        if (content.find(old_text, first + old_text.size()) != std::string::npos) {
            result.error = "oldText is not unique in file: " + path;
            return result;
        }
        std::string updated = content;
        updated.replace(first, old_text.size(), new_text);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            result.error = "cannot open file for write: " + path;
            return result;
        }
        output << updated;
        output.close();
        result.ok = true;
        result.text = "edited " + path;
        return result;
    }

    if (name == "bash") {
        const std::string command = arguments.value("command", std::string());
        if (command.empty()) {
            result.error = "bash requires a command";
            return result;
        }
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
