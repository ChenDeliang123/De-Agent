// 系统提示词的关键规则回归测试。
//
// 提示词现在由“基础行为规则 + 本次激活工具的贡献”组成，所以测试不再锁死整段
// 文案，而是锁三件事：
//   1) 每条事故规则仍有可判定的语义；
//   2) 未激活的工具不会出现在工具清单/规则里；
//   3) 规则不重复，且正文保持在预算内。

#include <algorithm>
#include <initializer_list>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "aiagent/agent/system_prompt.hpp"
#include "aiagent/llm/deepseek_types.hpp"
#include "aiagent/tools/local_tools.hpp"
#include "aiagent/tools/sub_agent_tool.hpp"
#include "aiagent/version.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

void ExpectContains(const std::string& haystack, const std::string& needle,
                    const std::string& message) {
    if (haystack.find(needle) == std::string::npos) {
        std::cerr << "FAILED: " << message << " (missing: " << needle << ")\n";
        ++g_failures;
    }
}

void ExpectNotContains(const std::string& haystack, const std::string& needle,
                       const std::string& message) {
    if (haystack.find(needle) != std::string::npos) {
        std::cerr << "FAILED: " << message << " (unexpected: " << needle << ")\n";
        ++g_failures;
    }
}

std::size_t CountOccurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

std::vector<aiagent::llm::ToolSpec> BuiltinTools() {
    aiagent::tools::LocalCodingTools tools(".");
    return tools.ListTools();
}

std::vector<aiagent::llm::ToolSpec> SelectTools(
        const std::vector<aiagent::llm::ToolSpec>& all,
        std::initializer_list<const char*> names) {
    std::vector<aiagent::llm::ToolSpec> selected;
    for (const auto& spec : all) {
        for (const char* name : names) {
            if (spec.name == name) {
                selected.push_back(spec);
                break;
            }
        }
    }
    return selected;
}

void ExpectUniqueRules(const std::string& prompt) {
    std::set<std::string> seen;
    std::istringstream input(prompt);
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("- ", 0) != 0) {
            continue;
        }
        Expect(seen.insert(line).second, "duplicate rule/tool line: " + line);
    }
}

void TestFullToolSet() {
    const std::string prompt =
            aiagent::agent::BuildCodingSystemPrompt(BuiltinTools());

    // 身份与平台。
    ExpectContains(prompt, aiagent::kAgentName, "prompt should name the agent");
    ExpectContains(prompt, aiagent::kAgentAuthor, "prompt should name the author");
    ExpectContains(prompt, "Platform:", "prompt should state the platform");

    // 基础提示词按 section 分块：preamble 无标签，tools/rules 有边界。
    ExpectContains(prompt, "<tools>\n", "prompt should open a tools section");
    ExpectContains(prompt, "</tools>", "prompt should close the tools section");
    ExpectContains(prompt, "<rules>\n", "prompt should open a rules section");
    ExpectContains(prompt, "</rules>", "prompt should close the rules section");
    ExpectNotContains(prompt, "<harness_docs>",
                      "harness docs should stay off unless configured");

    // 工具清单只列本次激活的内置工具。
    for (const char* name : {"read", "write", "edit", "bash", "ls", "glob", "grep"}) {
        ExpectContains(prompt, std::string("- ") + name + ":",
                       "tool list should include " + std::string(name));
    }
    ExpectContains(prompt, "Additional tools may be available",
                   "prompt should defer other tools to the request tool list");

    // 实现 vs 分析的分支（历史事故：只 review 却改文件 / 只给计划不落盘）。
    ExpectContains(prompt, "Implement when asked to implement",
                   "prompt should require implementation when asked");
    ExpectContains(prompt, "If the user asks only for analysis",
                   "prompt should keep analysis/review requests read-only");
    ExpectContains(prompt, "do not modify files",
                   "prompt should forbid edits for analysis-only requests");

    // 最小实现与迭代节奏。
    ExpectContains(prompt, "Make the smallest complete change",
                   "prompt should require a minimal implementation");
    ExpectContains(prompt, "no features the user did not ask for",
                   "prompt should forbid unrequested features");
    ExpectContains(prompt, "smallest reasonable version",
                   "prompt should ship an assumption-driven version when ambiguous");
    ExpectContains(prompt, "thin end-to-end slice",
                   "prompt should prefer a thin end-to-end slice");
    ExpectContains(prompt, "treat new feedback as the new plan",
                   "prompt should treat feedback as the new plan");
    ExpectContains(prompt, "keep the tree runnable",
                   "prompt should require a runnable tree");
    ExpectContains(prompt, "Batch independent work",
                   "prompt should batch independent calls");
    ExpectContains(prompt, "keep code-changing slices sequential",
                   "prompt should keep code-changing slices sequential");

    // 工具边界。
    ExpectContains(prompt, "Use read to examine files instead of cat or sed",
                   "read guideline should prefer read over shell inspection");
    ExpectContains(prompt, "Use write only for new files or complete rewrites",
                   "write guideline should scope write");
    ExpectContains(prompt, "batch changes to one file in a single call",
                   "edit guideline should require batching");
    ExpectContains(prompt, "Read the target file before editing it",
                   "edit guideline should require reading first");
    ExpectContains(prompt, "Use bash for builds, tests, and one-shot aggregation",
                   "bash guideline should route aggregation to one pipeline");
    ExpectContains(prompt, "For source changes use write/edit",
                   "prompt should forbid shell edits for source changes");
    ExpectContains(prompt, "Prefer ls/glob/grep over bash",
                   "prompt should prefer structured search tools");

    // 验证尺度与收尾。
    ExpectContains(prompt, "Verify in proportion",
                   "prompt should scope verification effort");
    ExpectContains(prompt, "two focused fixes",
                   "prompt should cap bug-chasing with an escalation rule");
    ExpectContains(prompt, "report the symptom", "prompt should report blockers");
    ExpectContains(prompt, "Be concise and show file paths",
                   "prompt should require concise, path-bearing output");

    ExpectUniqueRules(prompt);
    Expect(prompt.size() < 4096, "base prompt should stay under 4 KB");
}

void TestSubsetToolSet() {
    const auto subset =
            SelectTools(BuiltinTools(), {"read", "grep", "glob", "ls"});
    const std::string prompt =
            aiagent::agent::BuildCodingSystemPrompt(subset);

    ExpectContains(prompt, "- read:", "active read tool should be listed");
    ExpectContains(prompt, "Use read to examine files",
                   "active read guideline should be included");
    ExpectContains(prompt, "Prefer ls/glob/grep over shell pipelines",
                   "search-only rule should avoid naming bash");
    ExpectNotContains(prompt, "- write:", "inactive write tool must not be listed");
    ExpectNotContains(prompt, "- edit:", "inactive edit tool must not be listed");
    ExpectNotContains(prompt, "- bash:", "inactive bash tool must not be listed");
    ExpectNotContains(prompt, "Use write only", "inactive write rule must not appear");
    ExpectNotContains(prompt, "edits[]", "inactive edit rule must not appear");
    ExpectNotContains(prompt, "Use bash for builds",
                      "inactive bash rule must not appear");
    ExpectNotContains(prompt, "For source changes use write/edit",
                      "source-change rule needs both bash and an edit tool");

    ExpectUniqueRules(prompt);
}

void TestTaskMetadata() {
    aiagent::tools::SubAgentToolProvider provider({}, 0);
    const auto specs = provider.ListTools();

    const auto task = std::find_if(specs.begin(), specs.end(), [](const auto& spec) {
        return spec.name == "task";
    });
    Expect(task != specs.end(), "task provider should expose task");
    if (task != specs.end()) {
        Expect(!task->prompt_snippet.empty(),
               "task should contribute a prompt snippet");
    }
    const auto task_result =
            std::find_if(specs.begin(), specs.end(), [](const auto& spec) {
                return spec.name == "task_result";
            });
    Expect(task_result != specs.end(), "task provider should expose task_result");
    if (task_result != specs.end()) {
        Expect(!task_result->prompt_snippet.empty(),
               "task_result should contribute a prompt snippet");
    }

    const std::string prompt =
            aiagent::agent::BuildCodingSystemPrompt(specs);
    ExpectContains(prompt, "- task:", "task should be listed from its snippet");
    ExpectContains(prompt, "- task_result:",
                   "task_result should be listed from its snippet");
    // 派发准则是运行时按“工具列表含 task”追加的，不应在基础提示词里重复。
    ExpectNotContains(prompt, "Delegation:",
                      "delegation guidance belongs to the runtime append");
}

void TestOverrideAndAppend() {
    aiagent::agent::SystemPromptOptions options;
    options.tools = BuiltinTools();
    options.override_prompt = "CUSTOM SYSTEM PROMPT";
    options.append_prompt = "APPENDED RULE";

    const std::string prompt = aiagent::agent::BuildCodingSystemPrompt(options);
    ExpectContains(prompt, "CUSTOM SYSTEM PROMPT",
                   "override prompt should replace the preamble");
    ExpectNotContains(prompt, "<tools>", "override should suppress the tools section");
    ExpectNotContains(prompt, "<rules>", "override should suppress the rules section");
    ExpectNotContains(prompt, "Implement when asked to implement",
                      "override should suppress the built-in rules");
    ExpectContains(prompt, "<addendum>\nAPPENDED RULE\n</addendum>",
                   "append prompt should be rendered as an addendum section");

    options.override_prompt.clear();
    const std::string appended = aiagent::agent::BuildCodingSystemPrompt(options);
    ExpectContains(appended, "<tools>", "append-only should keep the tools section");
    ExpectContains(appended, "<rules>", "append-only should keep the rules section");
    ExpectContains(appended, "<addendum>\nAPPENDED RULE\n</addendum>",
                   "append-only should still add the addendum");
}

void TestHarnessDocs() {
    aiagent::agent::SystemPromptOptions options;
    options.tools = BuiltinTools();
    options.harness_docs =
            "Read these only when the user asks about the harness itself:\n"
            "- Main documentation: /docs/README.md\n"
            "- Additional docs: /docs";

    const std::string prompt = aiagent::agent::BuildCodingSystemPrompt(options);
    ExpectContains(prompt, "<harness_docs>\nRead these only when",
                   "harness docs should be rendered as a section");
    ExpectContains(prompt, "</harness_docs>", "harness docs section should be closed");
    ExpectContains(prompt, "- Main documentation: /docs/README.md",
                   "harness docs should include the main documentation path");
    Expect(prompt.find("<rules>") < prompt.find("<harness_docs>"),
           "harness docs should follow the rules section");
}

void TestRuleDedup() {
    auto tools = BuiltinTools();
    aiagent::llm::ToolSpec extra;
    extra.name = "extra";
    extra.prompt_snippet = "Extra tool";
    extra.prompt_guidelines = {"Use read to examine files instead of cat or sed."};
    tools.push_back(std::move(extra));

    const std::string prompt =
            aiagent::agent::BuildCodingSystemPrompt(tools);
    Expect(CountOccurrences(prompt, "Use read to examine files instead of cat or sed.") == 1,
           "duplicate tool guideline should be deduplicated");
}

}  // namespace

int main() {
    TestFullToolSet();
    TestSubsetToolSet();
    TestTaskMetadata();
    TestOverrideAndAppend();
    TestHarnessDocs();
    TestRuleDedup();

    if (g_failures != 0) {
        std::cerr << "system prompt test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "system prompt test passed\n";
    return 0;
}
