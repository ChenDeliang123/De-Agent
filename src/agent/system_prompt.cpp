#include "aiagent/agent/system_prompt.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "aiagent/version.hpp"

namespace aiagent::agent {
namespace {

std::string Trim(std::string_view value) {
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
    return std::string(value.substr(begin, end - begin));
}

bool HasTool(const std::vector<llm::ToolSpec>& tools, std::string_view name) {
    const std::string target(name);
    return std::any_of(tools.begin(), tools.end(), [&target](const llm::ToolSpec& tool) {
        return tool.name == target;
    });
}

std::string PlatformLine(bool has_bash) {
#ifdef _WIN32
    std::string line = "Platform: Windows x64.";
    if (has_bash) {
        line +=
                " The bash tool runs commands via `cmd.exe /d /s /c`; use Windows commands "
                "(dir, type, findstr, where, copy, move) or invoke PowerShell explicitly when "
                "needed.";
    }
#else
    std::string line = "Platform: POSIX (Linux).";
    if (has_bash) {
        line += " The bash tool runs commands via `/bin/sh -c`.";
    }
#endif
    return line;
}

std::string RenderTools(const std::vector<llm::ToolSpec>& tools) {
    std::ostringstream output;
    bool any = false;
    for (const auto& tool : tools) {
        if (tool.name.empty() || tool.prompt_snippet.empty()) {
            continue;
        }
        output << "- " << tool.name << ": " << tool.prompt_snippet << '\n';
        any = true;
    }
    if (!any) {
        output << "(none)\n";
    }
    output << "Additional tools may be available in the request's tool list.";
    return output.str();
}

// 有序、去重的规则集合；工具贡献与基础规则可能重复，按 trim 后文本去重。
class RuleList {
public:
    void Add(std::string rule) {
        rule = Trim(rule);
        if (rule.empty() || seen_.count(rule) > 0) {
            return;
        }
        seen_.insert(rule);
        rules_.push_back(std::move(rule));
    }

    std::string Render() const {
        std::ostringstream output;
        for (const auto& rule : rules_) {
            output << "- " << rule << '\n';
        }
        return output.str();
    }

private:
    std::vector<std::string> rules_;
    std::set<std::string> seen_;
};

std::string RenderRules(const std::vector<llm::ToolSpec>& tools) {
    const bool has_bash = HasTool(tools, "bash");
    const bool has_write = HasTool(tools, "write");
    const bool has_edit = HasTool(tools, "edit");
    const bool has_search =
            HasTool(tools, "ls") || HasTool(tools, "glob") || HasTool(tools, "grep");
    const bool has_file_change = has_write || has_edit;

    RuleList rules;
    rules.Add(
            "Implement when asked to implement. If the user asks for code or file changes, "
            "make them in this turn and keep going until the request is satisfied; do not hand "
            "back a plan. If the user asks only for analysis, a review, or a plan, answer it and "
            "do not modify files.");
    rules.Add(
            "Make the smallest complete change: no features the user did not ask for, no extra "
            "files, no test harnesses or screenshot/pixel tooling unless requested. When the "
            "request is ambiguous, ship the smallest reasonable version and state your "
            "assumptions. Prefer a thin end-to-end slice that runs, then extend it in small "
            "steps; treat new feedback as the new plan, keep the tree runnable, and end with the "
            "shortest command or path to see the result.");
    rules.Add(
            "Batch independent work: emit independent read-only tool calls in the same turn; keep "
            "code-changing slices sequential.");

    if (has_search && has_bash) {
        rules.Add(
                "Prefer ls/glob/grep over bash for listing and searching files; use bash when "
                "you need hidden files or git-aware filtering.");
    } else if (has_search) {
        rules.Add("Prefer ls/glob/grep over shell pipelines for listing and searching files.");
    } else if (has_bash) {
        rules.Add("Use bash for listing, searching, and finding files.");
    }

    // 工具贡献的规则：只在该工具本次真实激活时出现。顺序与工具声明一致。
    for (const auto& tool : tools) {
        for (const auto& guideline : tool.prompt_guidelines) {
            rules.Add(guideline);
        }
    }

    if (has_bash && has_file_change) {
        rules.Add(
                "For source changes use write/edit instead of bash redirection or sed -i.");
    }

    rules.Add(
            "Verify in proportion: one run or smoke check is enough for a code change. If a bug "
            "survives two focused fixes, report the symptom, your best hypothesis, what you "
            "tried, and the blocker instead of building more tooling.");
    rules.Add(
            "Be concise and show file paths. After changes, list the files created or modified "
            "and any assumptions you made.");
    return rules.Render();
}

struct PromptSection {
    std::string name;
    std::string content;
    bool tagged = true;
};

std::string RenderSections(const std::vector<PromptSection>& sections) {
    std::ostringstream output;
    bool first = true;
    for (const auto& section : sections) {
        if (section.content.empty()) {
            continue;
        }
        if (!first) {
            output << "\n\n";
        }
        first = false;
        if (!section.tagged) {
            output << section.content;
            continue;
        }
        output << '<' << section.name << ">\n" << section.content;
        if (section.content.back() != '\n') {
            output << '\n';
        }
        output << "</" << section.name << '>';
    }
    if (!first) {
        output << '\n';
    }
    return output.str();
}

std::string BuildPreamble(bool has_bash) {
    std::ostringstream output;
    output << "You are " << aiagent::kAgentName
           << ", a general-purpose coding agent created by " << aiagent::kAgentAuthor << ".\n";
    output << "You work on software projects in any language: reading, editing and writing "
              "files, running shell commands, and calling tools.\n\n";
    output << PlatformLine(has_bash);
    return output.str();
}

}  // namespace

std::string BuildCodingSystemPrompt(const SystemPromptOptions& options) {
    std::vector<PromptSection> sections;
    const std::string override_prompt = Trim(options.override_prompt);
    if (!override_prompt.empty()) {
        // 用户级 SYSTEM.md：替换 preamble/tools/rules，保留运行时追加的
        // 项目指令、技能、环境与派发准则。
        sections.push_back({"preamble", override_prompt, false});
    } else {
        sections.push_back(
                {"preamble", BuildPreamble(HasTool(options.tools, "bash")), false});
        sections.push_back({"tools", RenderTools(options.tools), true});
        sections.push_back({"rules", RenderRules(options.tools), true});
    }

    const std::string append_prompt = Trim(options.append_prompt);
    if (!append_prompt.empty()) {
        sections.push_back({"addendum", append_prompt, true});
    }
    return RenderSections(sections);
}

std::string BuildCodingSystemPrompt(const std::vector<llm::ToolSpec>& tools) {
    SystemPromptOptions options;
    options.tools = tools;
    return BuildCodingSystemPrompt(options);
}

}  // namespace aiagent::agent
