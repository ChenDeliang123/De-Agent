#include <ctime>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>

#include "aiagent/review/project_verify_config.hpp"
#include "aiagent/review/review_pack.hpp"
#include "aiagent/review/review_tools.hpp"
#include "aiagent/tools/tool_context.hpp"

#include "review_test_support.hpp"

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
        return nlohmann::json{{"__error", "invalid json output"}};
    }
}

aiagent::tools::ToolContext MakeContext(const std::string& repo) {
    aiagent::tools::ToolContext context;
    context.working_directory = repo;
    context.policy.allow_read = true;
    context.policy.allow_write = true;
    context.policy.allow_command = true;
    context.session_id = "session-review-tools";
    return context;
}

std::set<std::string> ToolNames(const std::vector<aiagent::llm::ToolSpec>& tools) {
    std::set<std::string> names;
    for (const auto& tool : tools) {
        names.insert(tool.name);
    }
    return names;
}

}  // namespace

int main() {
    namespace review = aiagent::review;
    namespace tools = aiagent::tools;
    try {
        const std::string repo = review_test_support::UniqueDir("review_tools_repo_");
        Expect(review_test_support::InitGitRepo(repo), "init git repo");
        review_test_support::WriteFile(repo + "/README.md", "# base\n");
        review_test_support::WriteFile(repo + "/src/foo.cpp", "int foo() { return 1; }\n");
        review_test_support::WriteFile(
                repo + "/CMakeLists.txt",
                "cmake_minimum_required(VERSION 3.21)\n"
                "project(x NONE)\n"
                "add_custom_target(mutate COMMAND ${CMAKE_COMMAND} -P "
                "${CMAKE_SOURCE_DIR}/mutate.cmake)\n");
        review_test_support::WriteFile(
                repo + "/mutate.cmake",
                "file(WRITE \"${CMAKE_CURRENT_LIST_DIR}/src/foo.cpp\" "
                "\"int foo() { return 77; }\\n\")\n");
        review_test_support::WriteFile(repo + "/.gitignore", "build/\n");
        Expect(review_test_support::CommitAll(repo, "base"), "commit base");

        review::ReviewToolOptions options;
        options.config = aiagent::ReviewConfig{};
        options.data_dir = repo + "/.data";
        review::ReviewTools reviewer(options, review::ReviewTools::Mode::kReviewer);
        review::ReviewTools coding(options, review::ReviewTools::Mode::kCodingHandoff);

        const auto reviewer_names = ToolNames(reviewer.ListTools());
        for (const char* expected : {"review_context", "review_read", "review_verify",
                                     "review_test_write", "review_setup", "review_report"}) {
            Expect(reviewer_names.count(expected) == 1,
                   std::string("reviewer tool missing: ") + expected);
        }
        Expect(reviewer_names.count("review_handoff") == 0,
               "reviewer must not expose review_handoff");
        const auto coding_names = ToolNames(coding.ListTools());
        Expect(coding_names.size() == 1 && coding_names.count("review_handoff") == 1,
               "coding handoff provider exposes only review_handoff");
        Expect(!reviewer.Execute("write", nlohmann::json::object(), MakeContext(repo)).ok,
               "reviewer must reject generic write");

        const auto context = MakeContext(repo);

        // setup detect/save/show。
        auto detected = Parse(reviewer.Execute("review_setup", {{"action", "detect"}}, context));
        Expect(detected.value("found", false) &&
                       detected.value("language", std::string()) == "cpp",
               "setup detect cpp: " + detected.dump());
        auto shown = Parse(reviewer.Execute("review_setup", {{"action", "show"}}, context));
        Expect(!shown.value("exists", true), "setup show before save");
        const nlohmann::json saved = Parse(reviewer.Execute(
                "review_setup",
                {{"action", "save"},
                 {"language", "cpp"},
                 {"commands",
                  nlohmann::json::array(
                          {{{"id", "test"},
                            {"kind", "test"},
                            {"argv", {"ctest", "--test-dir", "build"}},
                            {"cwd", "."},
                            {"timeout_s", 60}}})},
                 {"test_globs", {"tests/**"}}},
                context));
        Expect(saved.value("consent", std::string()) == "valid",
               "setup save records consent: " + saved.dump());
        shown = Parse(reviewer.Execute("review_setup", {{"action", "show"}}, context));
        Expect(shown.value("exists", false) &&
                       shown.value("consent", std::string()) == "valid",
               "setup show after save");

        // 生成 pack：改源码 + untracked 测试 + 声明 ctest 命令。
        review_test_support::WriteFile(repo + "/src/foo.cpp",
                                       "int foo() { return 2; }\n");
        review_test_support::WriteFile(repo + "/tests/original_test.cpp",
                                       "TEST(Original, Case) {}\n");
        review::ReviewPackServiceOptions pack_options;
        pack_options.config = options.config;
        pack_options.data_dir = options.data_dir;
        const review::ReviewPackService service(pack_options);
        review::ReviewPackRequest request;
        request.cwd = repo;
        request.feature = "tools";
        request.request_text = "## 验收标准\n- foo returns 2\n";
        request.verification = {
                {"v1", "pack", {"cmake", "-S", ".", "-B", "build"}, ".", 60, 0,
                 "configure"},
                {"v2", "pack", {"cmake", "--build", "build", "--target", "mutate"}, ".", 60,
                 0, "mutates source"}};
        const auto created = service.CreatePack(request);
        Expect(created.ok, "create pack: " + created.error);
        const std::string pack_path = repo + "/" + created.pack_path;

        auto manifest = Parse(reviewer.Execute(
                "review_context", {{"pack", pack_path}, {"mode", "manifest"}}, context));
        Expect(!manifest.value("stale", true), "manifest fresh: " + manifest.dump());
        Expect(manifest["review_id"] == created.review_id, "manifest review id");
        Expect(manifest["session"].is_null(), "manifest external session null");
        Expect(manifest["intent_source"] == "request-file", "manifest intent");
        bool saw_pack_command = false;
        bool saw_project_command = false;
        for (const auto& command : manifest["verification"]) {
            if (command["command_id"] == "pack:v1") {
                saw_pack_command = true;
            }
            if (command["source"] == "project") {
                saw_project_command = true;
            }
        }
        Expect(saw_pack_command && saw_project_command,
               "manifest lists pack and consented project commands");
        Expect(manifest["project_verify_consent"] == "valid",
               "manifest project consent valid");

        auto diff = Parse(reviewer.Execute(
                "review_context",
                {{"pack", pack_path}, {"mode", "diff"}, {"path", "src/foo.cpp"}}, context));
        Expect(diff["content"].get<std::string>().find("return 2") != std::string::npos,
               "diff slice: " + diff.dump());
        auto untracked = Parse(reviewer.Execute(
                "review_context",
                {{"pack", pack_path}, {"mode", "untracked"},
                 {"path", "tests/original_test.cpp"}},
                context));
        Expect(untracked["content"].get<std::string>().find("TEST(Original") !=
                       std::string::npos,
               "untracked slice");
        auto session = Parse(reviewer.Execute(
                "review_context", {{"pack", pack_path}, {"mode", "session"}}, context));
        Expect(session["request_text"].get<std::string>().find("foo returns 2") !=
                       std::string::npos,
               "session request text");

        // review_read：批量 + 行范围 + 越界拒绝。
        auto read = Parse(reviewer.Execute(
                "review_read",
                {{"files", nlohmann::json::array(
                                   {{{"path", "src/foo.cpp"}, {"start", 1}, {"limit", 10}}})},
                 {"max_bytes", 4096}},
                context));
        Expect(read["files"].size() == 1 &&
                       read["files"][0]["content"].get<std::string>().find("return 2") !=
                               std::string::npos,
               "review_read content: " + read.dump());
        auto read_again = Parse(reviewer.Execute(
                "review_read",
                {{"files", nlohmann::json::array(
                                   {{{"path", "src/foo.cpp"}, {"start", 1}, {"limit", 10}}})},
                 {"max_bytes", 4096}},
                context));
        Expect(read_again["files"][0].value("cache_hit", false),
               "second review_read should hit the cache: " + read_again.dump());
        Expect(!reviewer.Execute("review_read",
                                 {{"files", nlohmann::json::array(
                                                     {{{"path", "../outside.txt"}}})}},
                                 context)
                        .ok,
               "review_read must reject path escape");

        // review_verify：list + 执行 + 状态记录。
        auto list = Parse(reviewer.Execute(
                "review_verify", {{"pack", pack_path}, {"source", "pack"}, {"list", true}},
                context));
        bool listed_v1 = false;
        bool listed_v2 = false;
        for (const auto& command : list["commands"]) {
            listed_v1 = listed_v1 || command["command_id"] == "pack:v1";
            listed_v2 = listed_v2 || command["command_id"] == "pack:v2";
        }
        Expect(listed_v1 && listed_v2, "verify list: " + list.dump());
        auto verified = reviewer.Execute(
                "review_verify",
                {{"pack", pack_path}, {"source", "pack"}, {"command_id", "pack:v1"}},
                context);
        auto verified_json = Parse(verified);
        Expect(verified.ok, "verify executed: " + verified.error);
        Expect(verified_json.value("source_modified", true) == false,
               "declared verify should not modify source: " + verified_json.dump());
        Expect(verified_json.contains("exit_code"), "verify returns exit code");

        // review_test_write：create + append + 拒绝源码/覆盖。
        const nlohmann::json created_test = Parse(reviewer.Execute(
                "review_test_write",
                {{"pack", pack_path},
                 {"path", "tests/review_owned_test.cpp"},
                 {"content", "TEST(Owned, Case) {}\n"},
                 {"mode", "create"},
                 {"reason", "repro"}},
                context));
        Expect(created_test.contains("sha256"), "test create: " + created_test.dump());
        auto appended = Parse(reviewer.Execute(
                "review_test_write",
                {{"pack", pack_path},
                 {"path", "tests/review_owned_test.cpp"},
                 {"content", "TEST(Owned, Second) {}\n"},
                 {"mode", "append"},
                 {"reason", "harness owner approval"}},
                context));
        Expect(appended.contains("sha256"), "test append: " + appended.dump());
        Expect(!reviewer.Execute("review_test_write",
                                 {{"pack", pack_path},
                                  {"path", "src/foo.cpp"},
                                  {"content", "x"},
                                  {"mode", "append"}},
                                 context)
                        .ok,
               "source path rejected");
        Expect(!reviewer.Execute("review_test_write",
                                 {{"pack", pack_path},
                                  {"path", "tests/original_test.cpp"},
                                  {"content", "x"},
                                  {"mode", "create"}},
                                 context)
                        .ok,
               "create on existing file rejected");

        // 报告：draft -> final；伪造验证被拒绝；stale 只能 blocked。
        auto draft = reviewer.Execute(
                "review_report",
                {{"pack", pack_path},
                 {"status", "draft"},
                 {"summary", "draft"},
                 {"checked", {"manifest", "diff"}}},
                context);
        Expect(draft.ok, "draft report: " + draft.error);

        auto fabricated = reviewer.Execute(
                "review_report",
                {{"pack", pack_path},
                 {"status", "final"},
                 {"verdict", "changes_requested"},
                 {"summary", "fabricated"},
                 {"verification", {"project:test"}},
                 {"test_files", {"tests/review_owned_test.cpp"}}},
                context);
        Expect(!fabricated.ok &&
                       fabricated.error.find("not executed") != std::string::npos,
               "fabricated verification rejected: " + fabricated.error);

        const nlohmann::json finding = {
                {"id", "R1"},
                {"severity", "major"},
                {"title", "foo returns the wrong value"},
                {"file", "src/foo.cpp"},
                {"line", 1},
                {"evidence", "return 2"},
                {"impact", "caller sees 2"},
                {"trigger", "foo()"},
                {"suggestion", "return 1"},
                {"confidence", "high"},
                {"status", "open"}};
        auto final = reviewer.Execute(
                "review_report",
                {{"pack", pack_path},
                 {"status", "final"},
                 {"verdict", "changes_requested"},
                 {"summary", "one major issue"},
                 {"findings", nlohmann::json::array({finding})},
                 {"checked", {"manifest", "diff"}},
                 {"verification", {"pack:v1"}},
                 {"test_files", {"tests/review_owned_test.cpp"}},
                 {"unverified", {"project test command not run"}}},
                context);
        Expect(final.ok, "final report: " + final.error);
        Expect(final.text.find("docs/review/tools-") != std::string::npos,
               "final report path under docs/review");
        const std::string report_path = repo + "/" + Parse(final)["path"].get<std::string>();
        const std::string report = review_test_support::ReadFile(report_path);
        Expect(report.find("verdict: changes_requested") != std::string::npos,
               "report frontmatter verdict");
        Expect(report.find("### R1 [major]") != std::string::npos, "report finding");
        Expect(report.find("pack:v1") != std::string::npos, "report verification");
        Expect(report.find("tests/review_owned_test.cpp") != std::string::npos,
               "report test files");

        // reviewer-owned 测试不触发 stale；源码变更触发 stale；stale 只能 blocked。
        auto fresh = Parse(reviewer.Execute(
                "review_context", {{"pack", pack_path}, {"mode", "manifest"}}, context));
        Expect(!fresh.value("stale", true),
               "owned test must not stale the pack: " + fresh.dump());

#ifndef _WIN32
        // 配置阶段已在 v1 完成；v2 通过 CMake 自定义目标修改源码，
        // 用来验证 source_modified/invalid。此时 pack 仍然 fresh。
        auto mutated = Parse(reviewer.Execute(
                "review_verify",
                {{"pack", pack_path}, {"source", "pack"}, {"command_id", "pack:v2"}},
                context));
        Expect(mutated.value("source_modified", false),
               "verify must detect source modification: " + mutated.dump());
        Expect(mutated.value("invalid", false), "mutating verification is invalid");
#endif

        auto stale = Parse(reviewer.Execute(
                "review_context", {{"pack", pack_path}, {"mode", "manifest"}}, context));
        Expect(stale.value("stale", false), "source change marks stale");
        auto blocked = reviewer.Execute(
                "review_report",
                {{"pack", pack_path},
                 {"status", "final"},
                 {"verdict", "approved"},
                 {"summary", "should fail"},
                 {"test_files", {"tests/review_owned_test.cpp"}}},
                context);
        Expect(!blocked.ok && blocked.error.find("stale") != std::string::npos,
               "stale approved rejected: " + blocked.error);
        auto blocked_ok = reviewer.Execute(
                "review_report",
                {{"pack", pack_path},
                 {"status", "final"},
                 {"verdict", "blocked"},
                 {"summary", "pack is stale"},
                 {"unverified", {"source changed"}},
                 {"test_files", {"tests/review_owned_test.cpp"}}},
                context);
        Expect(blocked_ok.ok, "stale blocked report: " + blocked_ok.error);
        Expect(Parse(blocked_ok).value("stale", false), "blocked report marks stale");

        // 总时限：把 checkpoint 的起点挪到过去，后续工具拒绝继续读/验证，
        // 但 review_report 仍可写 partial blocked。
        {
            review::ReviewState timed;
            std::string state_error;
            Expect(review::LoadReviewState(options.config, repo, created.review_id, &timed,
                                           &state_error),
                   "load state for timeout: " + state_error);
            timed.started_at_epoch_s = static_cast<long long>(std::time(nullptr)) - 10000;
            timed.timed_out = false;
            Expect(review::SaveReviewState(options.config, repo, timed, &state_error),
                   "save timeout state: " + state_error);
            auto expired = Parse(reviewer.Execute(
                    "review_context", {{"pack", pack_path}, {"mode", "manifest"}}, context));
            Expect(expired["review_state"].value("timed_out", false),
                   "manifest should report timeout: " + expired.dump());
            const auto denied = reviewer.Execute(
                    "review_read",
                    {{"files", nlohmann::json::array({{{"path", "README.md"}}})}},
                    context);
            Expect(!denied.ok && denied.error.find("review_timeout_s") != std::string::npos,
                   "read should stop after timeout: " + denied.error);
            auto partial = reviewer.Execute(
                    "review_report",
                    {{"pack", pack_path},
                     {"status", "draft"},
                     {"summary", ""},
                     {"unverified", {"timeout"}}},
                    context);
            Expect(partial.ok, "timeout partial report: " + partial.error);
            Expect(Parse(partial).value("verdict", std::string()) == "blocked" &&
                           Parse(partial).value("status", std::string()) == "final",
                   "timeout should force final blocked: " + partial.text);
        }

        // coding handoff：带本回合用户消息。
        std::string history_title;
        int history_count = 0;
        review::ReviewTools handoff(
                options, review::ReviewTools::Mode::kCodingHandoff,
                [&](const std::string&, std::string* title, std::vector<std::string>* messages,
                    int* count) {
                    *title = "history title";
                    *messages = {"first request"};
                    *count = 1;
                    return true;
                });
        auto handoff_context = MakeContext(repo);
        handoff_context.incoming_text = "implement and prepare review";
        const auto handoff_result = Parse(handoff.Execute(
                "review_handoff",
                {{"feature", "handoff"}, {"base", "HEAD"}, {"context_paths", {"README.md"}}},
                handoff_context));
        Expect(!handoff_result.contains("__error"),
               "handoff result: " + handoff_result.dump());
        Expect(handoff_result["intent_source"] == "de-session", "handoff intent");
        review::ReviewPack handoff_pack;
        std::string load_error;
        Expect(review::LoadReviewPack(repo + "/" + handoff_result["pack_path"].get<std::string>(),
                                      &handoff_pack, &load_error),
               "load handoff pack: " + load_error);
        Expect(handoff_pack.session.has_value() &&
                       handoff_pack.session->user_messages.size() == 2 &&
                       handoff_pack.session->user_messages.back() ==
                               "implement and prepare review",
               "handoff includes current turn message");

        std::filesystem::remove_all(repo);
    } catch (const std::exception& e) {
        std::cerr << "review tools test failed: " << e.what() << '\n';
        return 1;
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " review tools test(s) failed\n";
        return 1;
    }
    std::cout << "review tools test passed\n";
    return 0;
}
