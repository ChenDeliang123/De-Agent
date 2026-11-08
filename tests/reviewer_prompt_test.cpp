#include <iostream>
#include <string>
#include <vector>

#include "aiagent/agent/system_prompt.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

aiagent::llm::ToolSpec Tool(const std::string& name,
                            const std::string& snippet = "does a thing",
                            const std::string& guideline = {}) {
    aiagent::llm::ToolSpec tool;
    tool.name = name;
    tool.prompt_snippet = snippet;
    if (!guideline.empty()) {
        tool.prompt_guidelines.push_back(guideline);
    }
    return tool;
}

}  // namespace

int main() {
    {
        const std::vector<aiagent::llm::ToolSpec> tools = {
                Tool("read"), Tool("review_context"), Tool("review_read"), Tool("review_git"),
                Tool("review_scout"), Tool("review_verify"), Tool("review_test_write"),
                Tool("review_setup"), Tool("review_report"),
        };
        const std::string prompt = aiagent::agent::BuildReviewerSystemPrompt(tools);
        Expect(prompt.find("reviewer role") != std::string::npos,
               "reviewer identity should be explicit");
        Expect(prompt.find("<hard_rules>") != std::string::npos &&
                       prompt.find("MUST NOT modify") != std::string::npos,
               "hard write boundary should be present");
        Expect(prompt.find("stale") != std::string::npos &&
                       prompt.find("blocked") != std::string::npos,
               "staleness rule should be present");
        Expect(prompt.find("path:line") != std::string::npos,
               "evidence requirement should be present");
        Expect(prompt.find("Simplified Chinese") != std::string::npos &&
                       prompt.find("report prose") != std::string::npos,
               "report prose language should default to Simplified Chinese");
        Expect(prompt.find("review_setup(action=detect)") != std::string::npos &&
                       prompt.find("returns no commands") != std::string::npos,
               "empty verification list should force review_setup(detect)");
        Expect(prompt.find("<report_contract>") != std::string::npos &&
                       prompt.find("approved_with_notes") != std::string::npos,
               "report contract should be present");
        Expect(prompt.find("<review_method>") != std::string::npos &&
                       prompt.find("consumer") != std::string::npos,
               "review method must require consumer-side tracing");
        Expect(prompt.find("review_context(mode=plan)") != std::string::npos &&
                       prompt.find("uncovered_files") != std::string::npos,
               "review method must require plan coverage");
        Expect(prompt.find("find bugs the author wants fixed before merge") !=
                       std::string::npos,
               "review goal should be merge-gate focused");
        Expect(prompt.find("review_scout") != std::string::npos &&
                       prompt.find("mode=main_review") != std::string::npos &&
                       prompt.find("max 4 slices") != std::string::npos &&
                       prompt.find("parent_verified=true") != std::string::npos,
               "review_scout guidance should describe delegated primary review");
        for (const auto& tool : tools) {
            Expect(prompt.find("- " + tool.name + ":") != std::string::npos,
                   "active tool should be listed: " + tool.name);
        }
        Expect(prompt.find("Implement when asked") == std::string::npos,
               "reviewer prompt must not inherit the coding implement rule");
        Expect(prompt.find("generic shell") != std::string::npos,
               "reviewer prompt should say it has no generic shell");
    }

    // 工具集变化时工具清单与条件规则同步变化。
    {
        const std::vector<aiagent::llm::ToolSpec> tools = {
                Tool("read", "Read one file"),
        };
        const std::string prompt = aiagent::agent::BuildReviewerSystemPrompt(tools);
        Expect(prompt.find("- read: Read one file") != std::string::npos,
               "tool snippet should be rendered");
        Expect(prompt.find("review_verify(list=true)") == std::string::npos,
               "verify guidance should not appear when review_verify is absent");
        Expect(prompt.find("review_setup(action=save)") == std::string::npos,
               "setup guidance should not appear when review_setup is absent");
        Expect(prompt.find("- write:") == std::string::npos,
               "inactive write tool must not be listed");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " reviewer prompt test(s) failed\n";
        return 1;
    }
    std::cout << "reviewer prompt test passed\n";
    return 0;
}
