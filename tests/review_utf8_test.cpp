#include <filesystem>
#include <iostream>
#include <string>

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
        return nlohmann::json{{"__error", "invalid json"}};
    }
}

}  // namespace

int main() {
    namespace review = aiagent::review;
    try {
        const std::string repo = review_test_support::UniqueDir("review_utf8_repo_");
        Expect(review_test_support::InitGitRepo(repo), "init git repo");
        // 中文路径 + 中文内容，确保 diff 页边界会落在 UTF-8 续字节上。
        review_test_support::WriteFile(repo + "/src/中文.txt", "你好\n");
        Expect(review_test_support::CommitAll(repo, "base"), "commit base");
        std::string chinese;
        for (int i = 0; i < 300; ++i) {
            chinese += "审核分页边界测试，中文内容。\n";
        }
        review_test_support::WriteFile(repo + "/src/中文.txt", chinese);

        aiagent::ReviewConfig config;
        review::ReviewPackServiceOptions options{config, repo + "/.data"};
        const review::ReviewPackService service(options);
        review::ReviewPackRequest request;
        request.cwd = repo;
        request.feature = "utf8";
        request.request_text = "utf8 slicing";
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

        // 以 1 字节为步长遍历前 80 个偏移；旧实现会在续字节上 dump 失败。
        int failures = 0;
        for (int offset = 0; offset < 80; ++offset) {
            auto page = tools.Execute(
                    "review_context",
                    {{"pack", pack_path},
                     {"mode", "diff"},
                     {"path", "src/中文.txt"},
                     {"offset", offset},
                     {"limit", 1}},
                    context);
            if (!page.ok) {
                ++failures;
                if (failures == 1) {
                    std::cerr << "first failing offset " << offset << ": " << page.error
                              << '\n';
                }
            }
        }
        Expect(failures == 0, "UTF-8 diff pagination must not fail on continuation bytes");

        // 非法 UTF-8 文本也要能作为替换字符返回，而不是把整个工具打挂。
        std::string invalid = "line-";
        invalid.push_back(static_cast<char>(0xFF));
        invalid += "\n";
        review_test_support::WriteFile(repo + "/src/invalid.txt", invalid);
        auto read = tools.Execute(
                "review_read",
                {{"files", nlohmann::json::array(
                                   {{{"path", "src/invalid.txt"}, {"start", 1}, {"limit", 10}}})}},
                context);
        Expect(read.ok, "review_read should tolerate invalid UTF-8: " + read.error);
        Expect(read.text.find("line-") != std::string::npos,
               "invalid UTF-8 content should be replaced, not dropped");

        std::filesystem::remove_all(repo);
    } catch (const std::exception& e) {
        std::cerr << "review utf8 test failed: " << e.what() << '\n';
        return 1;
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " review utf8 test(s) failed\n";
        return 1;
    }
    std::cout << "review utf8 test passed\n";
    return 0;
}
