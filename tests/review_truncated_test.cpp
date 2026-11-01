#include <filesystem>
#include <iostream>
#include <string>

#include "aiagent/review/review_pack.hpp"

#include "review_test_support.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

}  // namespace

int main() {
    namespace review = aiagent::review;
    try {
        const std::string repo = review_test_support::UniqueDir("review_truncated_repo_");
        Expect(review_test_support::InitGitRepo(repo), "init git repo");
        std::string base;
        for (int i = 0; i < 4000; ++i) {
            base += "base line " + std::to_string(i) + "\n";
        }
        review_test_support::WriteFile(repo + "/big.txt", base);
        Expect(review_test_support::CommitAll(repo, "base"), "commit base");
        std::string changed;
        for (int i = 0; i < 4000; ++i) {
            changed += "changed line " + std::to_string(i) + "\n";
        }
        review_test_support::WriteFile(repo + "/big.txt", changed);

        aiagent::ReviewConfig config;
        config.max_diff_bytes = 65536;  // 允许的最小值，确保 diff 被截断
        review::ReviewPackServiceOptions options{config, repo + "/.data"};
        const review::ReviewPackService service(options);
        review::ReviewPackRequest request;
        request.cwd = repo;
        request.feature = "truncated";
        request.request_text = "truncated diff";
        const auto created = service.CreatePack(request);
        Expect(created.ok, "create pack: " + created.error);
        Expect(created.truncated, "pack should report truncated diff");
        review::ReviewPack pack;
        std::string error;
        Expect(review::LoadReviewPack(repo + "/" + created.pack_path, &pack, &error),
               "load pack: " + error);
        Expect(pack.diff_truncated, "pack records diff_truncated");
        const review::ReviewStaleResult stale =
                review::CheckReviewStale(pack, config, repo, {});
        Expect(stale.stale, "truncated pack must be stale for safety");
        bool reason = false;
        for (const auto& item : stale.reasons) {
            if (item.find("max_diff_bytes") != std::string::npos) {
                reason = true;
            }
        }
        Expect(reason, "truncated stale reason should mention max_diff_bytes");
        std::filesystem::remove_all(repo);
    } catch (const std::exception& e) {
        std::cerr << "review truncated test failed: " << e.what() << '\n';
        return 1;
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " review truncated test(s) failed\n";
        return 1;
    }
    std::cout << "review truncated test passed\n";
    return 0;
}
