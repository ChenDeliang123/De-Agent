// 系统提示词的关键规则回归测试。
//
// 提示词是行为的关键输入面：这几条规则各自对应一次真实的"跑偏"事故，
// 所以用测试锁住，避免以后改文案时被顺手删掉。

#include <iostream>
#include <string>

#include "aiagent/agent/system_prompt.hpp"
#include "aiagent/version.hpp"

namespace {

int g_failures = 0;

void ExpectContains(const std::string& haystack, const std::string& needle,
                    const std::string& message) {
    if (haystack.find(needle) == std::string::npos) {
        std::cerr << "FAILED: " << message << " (missing: " << needle << ")\n";
        ++g_failures;
    }
}

}  // namespace

int main() {
    const std::string prompt = aiagent::agent::BuildCodingSystemPrompt();

    // 1) 身份与工具清单：自我认知与可用工具必须写清楚。
    ExpectContains(prompt, aiagent::kAgentName, "prompt should name the agent");
    ExpectContains(prompt, aiagent::kAgentAuthor, "prompt should name the author");
    ExpectContains(prompt, "read, write, edit, bash", "prompt should list the local tools");

    // 2) 交付纪律：不许只给计划（历史事故：只输出计划就收工）。
    ExpectContains(prompt, "Do NOT stop after presenting a plan",
                   "prompt should forbid planning-only replies");

    // 3) 批量策略（历史事故：同一文件改多处被拆成多次 edit，轮次放大 10 倍）。
    ExpectContains(prompt, "ONE edit call with an edits[] array",
                   "prompt should require batching edits");
    ExpectContains(prompt, "emit them in the SAME turn",
                   "prompt should require same-turn independent tool calls");

    // 4) 最小实现与验证尺度（历史事故：一句"写个坦克大战"落成 27 个文件 + 截图/像素分析工具链）。
    ExpectContains(prompt, "Keep it minimal", "prompt should require a minimal implementation");
    ExpectContains(prompt, "Do not add features the user did not ask for",
                   "prompt should forbid unrequested features");
    ExpectContains(prompt, "Verify in proportion",
                   "prompt should scope verification effort");
    ExpectContains(prompt, "Do NOT build test frameworks",
                   "prompt should forbid building test harnesses unprompted");
    ExpectContains(prompt, "If a bug survives two focused fix attempts",
                   "prompt should cap bug-chasing with an escalation rule");

    // 5) 敏捷迭代（历史事故：把“实现一个小游戏”当成一次性大工程，先造设计再写代码）。
    ExpectContains(prompt, "Work in agile iterations",
                   "prompt should require agile, iterative delivery");
    ExpectContains(prompt, "Favor working software over an elaborate upfront design",
                   "prompt should prefer working software over big upfront design");
    ExpectContains(prompt, "When the request is ambiguous",
                   "prompt should tell the agent to ship a minimal assumption-driven version");
    ExpectContains(prompt, "leave the project in a runnable state",
                   "prompt should require a runnable state at each increment");
    ExpectContains(prompt, "Treat new feedback as the new plan",
                   "prompt should tell the agent to respond to change");
    ExpectContains(prompt, "Work on one slice at a time",
                   "prompt should limit work in progress to one slice");

    if (g_failures != 0) {
        std::cerr << "system prompt test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "system prompt test passed\n";
    return 0;
}
