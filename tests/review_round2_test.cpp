#include <filesystem>
#include <iostream>
#include <string>

#include "aiagent/review/review_pack.hpp"
#include "aiagent/review/review_report.hpp"
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
        return nlohmann::json{{"__error", "invalid json"}};
    }
}

nlohmann::json Finding(std::string id, std::string severity, std::string status,
                       std::string title) {
    return {{"id", std::move(id)},
            {"severity", std::move(severity)},
            {"title", std::move(title)},
            {"file", "src/foo.cpp"},
            {"line", 1},
            {"evidence", "return 2"},
            {"impact", "caller sees the wrong value"},
            {"trigger", "foo()"},
            {"suggestion", "return the expected value"},
            {"confidence", "high"},
            {"status", std::move(status)}};
}

}  // namespace

int main() {
    namespace review = aiagent::review;
    try {
        const std::string repo = review_test_support::UniqueDir("review_round2_repo_");
        Expect(review_test_support::InitGitRepo(repo), "init git repo");
        review_test_support::WriteFile(repo + "/src/foo.cpp", "int foo() { return 1; }\n");
        Expect(review_test_support::CommitAll(repo, "base"), "commit base");

        aiagent::ReviewConfig config;
        review::ReviewPackServiceOptions options{config, repo + "/.data"};
        const review::ReviewPackService service(options);
        review::ReviewToolOptions tool_options{config, repo + "/.data"};
        review::ReviewTools tools(tool_options, review::ReviewTools::Mode::kReviewer);
        aiagent::tools::ToolContext context;
        context.working_directory = repo;
        context.policy.allow_read = true;
        context.policy.allow_write = true;
        context.policy.allow_command = true;

        // 第一轮：R1 major -> changes_requested。
        review_test_support::WriteFile(repo + "/src/foo.cpp", "int foo() { return 2; }\n");
        review::ReviewPackRequest round1_request;
        round1_request.cwd = repo;
        round1_request.feature = "round1";
        round1_request.request_text = "round one";
        const auto round1 = service.CreatePack(round1_request);
        Expect(round1.ok, "round1 pack: " + round1.error);
        const std::string round1_pack = repo + "/" + round1.pack_path;
        auto manifest1 = Parse(tools.Execute(
                "review_context", {{"pack", round1_pack}, {"mode", "manifest"}}, context));
        Expect(!manifest1.value("stale", true), "round1 manifest fresh");
        auto report1 = tools.Execute(
                "review_report",
                {{"pack", round1_pack},
                 {"status", "final"},
                 {"verdict", "changes_requested"},
                 {"summary", "round one"},
                 {"findings", nlohmann::json::array(
                                      {Finding("R1", "major", "open", "wrong value")})}},
                context);
        Expect(report1.ok, "round1 report: " + report1.error);
        const std::string report1_path = Parse(report1)["path"].get<std::string>();
        Expect(review_test_support::ReadFile(repo + "/" + report1_path)
                       .find("verdict: changes_requested") != std::string::npos,
               "round1 report verdict");

        // 第二轮 pack 引用上一份报告。
        review_test_support::WriteFile(repo + "/src/foo.cpp", "int foo() { return 3; }\n");
        review::ReviewPackRequest round2_request;
        round2_request.cwd = repo;
        round2_request.feature = "round2";
        round2_request.request_text = "round two";
        round2_request.previous_report = report1_path;
        const auto round2 = service.CreatePack(round2_request);
        Expect(round2.ok, "round2 pack: " + round2.error);
        const std::string round2_pack = repo + "/" + round2.pack_path;
        review::ReviewPack pack2;
        std::string load_error;
        Expect(review::LoadReviewPack(round2_pack, &pack2, &load_error),
               "load round2 pack: " + load_error);
        Expect(pack2.previous_report == report1_path, "round2 records previous report");

        auto delta = Parse(tools.Execute(
                "review_context", {{"pack", round2_pack}, {"mode", "delta"}}, context));
        Expect(!delta.contains("__error"), "delta output: " + delta.dump());
        Expect(delta.value("previous_review_id", std::string()) == round1.review_id,
               "delta previous review id");
        Expect(delta.value("previous_verdict", std::string()) == "changes_requested",
               "delta previous verdict");
        Expect(delta["previous_finding_ids"].size() == 1 &&
                       delta["previous_finding_ids"][0] == "R1",
               "delta previous finding ids");
        bool modified = false;
        for (const auto& file : delta["files"]) {
            if (file.value("path", std::string()) == "src/foo.cpp" &&
                file.value("change", std::string()) == "modified") {
                modified = true;
            }
        }
        Expect(modified, "delta marks src/foo.cpp modified: " + delta.dump());

        // 第二轮必须沿用 R1 并标记 resolved/still_open/regressed。
        auto invalid_status = tools.Execute(
                "review_report",
                {{"pack", round2_pack},
                 {"status", "final"},
                 {"verdict", "changes_requested"},
                 {"summary", "round two"},
                 {"findings", nlohmann::json::array(
                                      {Finding("R1", "major", "open", "wrong value")})}},
                context);
        Expect(!invalid_status.ok &&
                       invalid_status.error.find("resolved/still_open/regressed") !=
                               std::string::npos,
               "reused finding must use a second-round status: " + invalid_status.error);

        auto missing_old = tools.Execute(
                "review_report",
                {{"pack", round2_pack},
                 {"status", "final"},
                 {"verdict", "changes_requested"},
                 {"summary", "round two"},
                 {"findings", nlohmann::json::array(
                                      {Finding("R2", "major", "open", "new problem")})}},
                context);
        Expect(!missing_old.ok &&
                       missing_old.error.find("missing previous finding") != std::string::npos,
               "final report must include previous finding: " + missing_old.error);

        auto report2 = tools.Execute(
                "review_report",
                {{"pack", round2_pack},
                 {"status", "final"},
                 {"verdict", "changes_requested"},
                 {"summary", "round two"},
                 {"findings", nlohmann::json::array(
                                      {Finding("R1", "major", "still_open", "wrong value"),
                                       Finding("R2", "minor", "open", "new nit")})}},
                context);
        Expect(report2.ok, "round2 report: " + report2.error);
        const std::string report2_text =
                review_test_support::ReadFile(repo + "/" + Parse(report2)["path"].get<std::string>());
        Expect(report2_text.find("round: 2") != std::string::npos,
               "round2 frontmatter round");
        Expect(report2_text.find("previous_report: " + report1_path) != std::string::npos,
               "round2 frontmatter previous_report");
        Expect(report2_text.find("### R1 [major] wrong value") != std::string::npos &&
                       report2_text.find("- 状态：still_open") != std::string::npos,
               "round2 preserves R1 status");

        // 新 finding 不能复用旧 id 空间（R1 之后应从 R2 开始；R2 合法）。
        auto reused_new = tools.Execute(
                "review_report",
                {{"pack", round2_pack},
                 {"status", "final"},
                 {"verdict", "changes_requested"},
                 {"summary", "round two"},
                 {"findings",
                  nlohmann::json::array(
                          {Finding("R1", "major", "resolved", "wrong value"),
                           Finding("R1", "minor", "open", "duplicate id")})}},
                context);
        Expect(!reused_new.ok, "duplicate finding id must be rejected");

        std::filesystem::remove_all(repo);
    } catch (const std::exception& e) {
        std::cerr << "review round2 test failed: " << e.what() << '\n';
        return 1;
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " review round2 test(s) failed\n";
        return 1;
    }
    std::cout << "review round2 test passed\n";
    return 0;
}
