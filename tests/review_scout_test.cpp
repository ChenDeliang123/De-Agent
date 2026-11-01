#include <iostream>
#include <string>
#include <vector>

#include "aiagent/tools/review_scout_tool.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

nlohmann::json Parse(const aiagent::tools::ToolExecutionResult& result) {
    if (!result.ok) {
        return nlohmann::json{{"__error", result.error}};
    }
    try {
        return nlohmann::json::parse(result.text);
    } catch (const nlohmann::json::exception&) {
        return nlohmann::json{{"__error", "invalid json"}};
    }
}

aiagent::tools::ToolContext Context() {
    aiagent::tools::ToolContext context;
    context.working_directory = "/tmp/repo";
    context.policy.allow_read = true;
    context.session_id = "review-session";
    context.tool_call_id = "call-1";
    return context;
}

nlohmann::json Task(const std::string& name, const std::string& file,
                    const std::string& focus = "lifetime") {
    return {{"name", name}, {"files", {file}}, {"focus", {focus}}};
}

}  // namespace

int main() {
    namespace tools = aiagent::tools;
    namespace agent = aiagent::agent;

    std::vector<agent::SubAgentRequest> captured;
    int captured_depth = -1;
    auto runner = [&](const std::vector<agent::SubAgentRequest>& requests, int depth) {
        captured = requests;
        captured_depth = depth;
        std::vector<agent::SubAgentResult> results;
        for (const auto& request : requests) {
            agent::SubAgentResult result;
            result.ok = true;
            result.agent_id = "review-scout#1";
            result.duration_ms = 12;
            result.tool_calls = 3;
            result.text =
                    "{\"findings\":[{\"severity\":\"major\",\"file\":\"src/foo.cpp\","
                    "\"line\":7,\"title\":\"bad\",\"evidence\":\"x\",\"impact\":\"y\","
                    "\"trigger\":\"z\",\"suggestion\":\"fix\",\"confidence\":\"high\"}],"
                    "\"unverified\":[],\"coverage\":{\"covered\":1,\"total\":1}}";
            results.push_back(std::move(result));
        }
        return results;
    };

    tools::ReviewScoutToolProvider provider(runner, 4);
    const auto listed = provider.ListTools();
    Expect(listed.size() == 1 && listed[0].name == "review_scout",
           "review_scout tool should be listed");

    auto result = provider.Execute(
            "review_scout",
            {{"tasks", nlohmann::json::array(
                                {Task("review-tools", "src/review/review_tools.cpp"),
                                 Task("review-pack", "src/review/review_pack.cpp")})},
             {"max_findings_per_task", 4}},
            Context());
    Expect(result.ok, "valid scout call: " + result.error);
    auto parsed = Parse(result);
    Expect(parsed["tasks"].size() == 2 &&
                   parsed["tasks"][0]["findings"].size() == 1 &&
                   parsed["total_findings"] == 2,
           "scout should aggregate findings: " + parsed.dump());
    Expect(captured.size() == 2 && captured_depth == 0,
           "runner should receive two tasks at depth 0");
    Expect(captured[0].agent_type == "review-scout" &&
                   captured[0].prompt.find("src/review/review_tools.cpp") != std::string::npos &&
                   captured[0].prompt.find("lifetime") != std::string::npos,
           "scout assignment should include files and focus");
    Expect(captured[0].parent_session_id == "review-session" &&
                   captured[0].parent_tool_call_id == "call-1",
           "scout should carry parent session/tool identity");

    auto too_many = provider.Execute(
            "review_scout",
            {{"tasks", nlohmann::json::array({Task("a", "a.cpp"), Task("b", "b.cpp"),
                                              Task("c", "c.cpp"), Task("d", "d.cpp"),
                                              Task("e", "e.cpp")})}},
            Context());
    Expect(!too_many.ok && too_many.error.find("too many") != std::string::npos,
           "too many scout tasks must be rejected");
    auto escape = provider.Execute(
            "review_scout",
            {{"tasks", nlohmann::json::array({Task("a", "../outside.cpp")})}},
            Context());
    Expect(!escape.ok && escape.error.find("repo-relative") != std::string::npos,
           "path escape must be rejected");
    auto bad_focus = provider.Execute(
            "review_scout",
            {{"tasks", nlohmann::json::array(
                                {{{"name", "a"}, {"files", {"a.cpp"}}, {"focus", {"line\nbreak"}}}})}},
            Context());
    Expect(!bad_focus.ok, "newline in focus must be rejected");
    auto bad_findings = provider.Execute(
            "review_scout",
            {{"tasks", nlohmann::json::array({Task("a", "a.cpp")})},
             {"max_findings_per_task", 9}},
            Context());
    Expect(!bad_findings.ok, "max_findings_per_task should be bounded");

    tools::ReviewScoutToolProvider disabled(runner, 0);
    auto unavailable = disabled.Execute(
            "review_scout",
            {{"tasks", nlohmann::json::array({Task("a", "a.cpp")})}}, Context());
    Expect(!unavailable.ok && unavailable.error.find("disabled") != std::string::npos,
           "scout_max=0 should disable the tool");

    if (g_failures != 0) {
        std::cerr << g_failures << " review scout test(s) failed\n";
        return 1;
    }
    std::cout << "review scout test passed\n";
    return 0;
}
