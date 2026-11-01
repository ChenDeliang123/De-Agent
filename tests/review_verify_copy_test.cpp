#include <filesystem>
#include <iostream>
#include <string>

#include "aiagent/platform/paths.hpp"
#include "aiagent/review/review_pack.hpp"
#include "aiagent/review/review_state.hpp"
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

}  // namespace

int main() {
    namespace review = aiagent::review;
    try {
        const std::string repo = review_test_support::UniqueDir("review_copy_repo_");
        Expect(review_test_support::InitGitRepo(repo), "init git repo");
        review_test_support::WriteFile(
                repo + "/CMakeLists.txt",
                "cmake_minimum_required(VERSION 3.21)\n"
                "project(copy_test NONE)\n"
                "add_custom_target(mutate COMMAND ${CMAKE_COMMAND} -P "
                "${CMAKE_SOURCE_DIR}/mutate.cmake)\n");
        review_test_support::WriteFile(
                repo + "/mutate.cmake",
                "file(WRITE \"${CMAKE_CURRENT_LIST_DIR}/src/foo.cpp\" "
                "\"int foo() { return 77; }\\n\")\n");
        review_test_support::WriteFile(repo + "/.gitignore", "build/\n");
        review_test_support::WriteFile(repo + "/src/foo.cpp", "int foo() { return 1; }\n");
        Expect(review_test_support::CommitAll(repo, "base"), "commit base");
        review_test_support::WriteFile(repo + "/src/foo.cpp", "int foo() { return 2; }\n");

        aiagent::ReviewConfig config;
        config.verify_mode = "copy";
        review::ReviewPackServiceOptions options{config, repo + "/.data"};
        const review::ReviewPackService service(options);
        review::ReviewPackRequest request;
        request.cwd = repo;
        request.feature = "copy";
        request.request_text = "copy verification";
        request.verification = {
                {"v1", "pack", {"cmake", "-S", ".", "-B", "build"}, ".", 60, 0, "configure"},
                {"v2", "pack", {"cmake", "--build", "build", "--target", "mutate"}, ".",
                 60, 0, "mutates the copy"}};
        const auto created = service.CreatePack(request);
        Expect(created.ok, "create pack: " + created.error);
        const std::string pack_path = repo + "/" + created.pack_path;

        review::ReviewToolOptions tool_options{config, repo + "/.data"};
        review::ReviewTools tools(tool_options, review::ReviewTools::Mode::kReviewer);
        aiagent::tools::ToolContext context;
        context.working_directory = repo;
        context.policy.allow_read = true;
        context.policy.allow_write = true;
        context.policy.allow_command = true;

        auto manifest = Parse(tools.Execute(
                "review_context", {{"pack", pack_path}, {"mode", "manifest"}}, context));
        Expect(!manifest.value("stale", true), "manifest fresh: " + manifest.dump());

        auto verify1 = Parse(tools.Execute(
                "review_verify",
                {{"pack", pack_path}, {"source", "pack"}, {"command_id", "pack:v1"}},
                context));
        Expect(verify1.value("exit_code", -1) == 0,
               "copy configure should succeed: " + verify1.dump());
        Expect(verify1.value("verify_mode", std::string()) == "copy",
               "verify_mode reported");
        Expect(!verify1.value("source_modified", true),
               "copy configure must not modify the original");

        auto verify2 = Parse(tools.Execute(
                "review_verify",
                {{"pack", pack_path}, {"source", "pack"}, {"command_id", "pack:v2"}},
                context));
        Expect(verify2.value("exit_code", -1) == 0,
               "copy mutation should succeed: " + verify2.dump());
        Expect(verify2.value("verify_mode", std::string()) == "copy",
               "copy mode reported for mutation");
        Expect(!verify2.value("source_modified", true),
               "mutation inside the copy must not invalidate the original");
        Expect(review_test_support::ReadFile(repo + "/src/foo.cpp").find("return 2") !=
                       std::string::npos,
               "original source must remain unchanged");

        review::ReviewState state;
        std::string state_error;
        Expect(review::LoadReviewState(config, repo, created.review_id, &state,
                                       &state_error),
               "load state: " + state_error);
        bool found = false;
        for (const auto& record : state.verification) {
            if (record.command_id == "v2") {
                found = true;
                Expect(!record.source_modified && !record.invalid,
                       "copy-mode verification recorded as valid");
            }
        }
        Expect(found, "copy-mode verification recorded");

        std::filesystem::remove_all(repo);
    } catch (const std::exception& e) {
        std::cerr << "review verify copy test failed: " << e.what() << '\n';
        return 1;
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " review verify copy test(s) failed\n";
        return 1;
    }
    std::cout << "review verify copy test passed\n";
    return 0;
}
