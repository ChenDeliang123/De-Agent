#include <filesystem>
#include <iostream>
#include <string>

#include "aiagent/review/project_verify_config.hpp"
#include "aiagent/review/review_pack.hpp"
#include "aiagent/review/review_state.hpp"
#include "aiagent/util/sha256.hpp"

#include "review_test_support.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

bool ContainsPath(const std::vector<aiagent::review::ReviewChangedFile>& files,
                  const std::string& path) {
    for (const auto& file : files) {
        if (file.path == path) {
            return true;
        }
    }
    return false;
}

const aiagent::review::ReviewOmittedFile* FindOmitted(
        const std::vector<aiagent::review::ReviewOmittedFile>& omitted,
        const std::string& path) {
    for (const auto& item : omitted) {
        if (item.path == path) {
            return &item;
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    namespace review = aiagent::review;
    try {
        const std::string repo = review_test_support::UniqueDir("review_pack_repo_");
        Expect(review_test_support::InitGitRepo(repo), "init git repo");
        review_test_support::WriteFile(repo + "/README.md", "# base\n");
        review_test_support::WriteFile(repo + "/src/foo.cpp", "int foo() { return 1; }\n");
        review_test_support::WriteFile(repo + "/docs/plan.md", "# plan\n");
        review_test_support::WriteFile(repo + "/docs/review/tracked.txt", "v1\n");
        Expect(review_test_support::CommitAll(repo, "base"), "commit base");
        // review 目录里被跟踪的文件也属于审核产物：改动它不应让 pack 过期。
        review_test_support::WriteFile(repo + "/docs/review/tracked.txt", "v2\n");

        review::ReviewPackServiceOptions options;
        options.config = aiagent::ReviewConfig{};
        options.data_dir = repo + "/.data";
        const review::ReviewPackService service(options);

        review::ReviewPackRequest empty_request;
        empty_request.cwd = repo;
        empty_request.feature = "empty";
        const auto empty = service.CreatePack(empty_request);
        Expect(!empty.ok && empty.error.find("no changes") != std::string::npos,
               "clean repo should not produce an empty pack: " + empty.error);

        // 改动源码 + untracked 测试/敏感文件/二进制。
        review_test_support::WriteFile(repo + "/src/foo.cpp",
                                       "int foo() { return 2; }\nint bar() { return 3; }\n");
        review_test_support::WriteFile(repo + "/tests/review_new_test.cpp",
                                       "TEST(New, Case) {}\n");
        review_test_support::WriteFile(repo + "/.env", "SECRET=value\n");
        std::string binary = "bin";
        binary.push_back('\0');
        binary += "ary";
        review_test_support::WriteFile(repo + "/asset.bin", binary);
        review_test_support::WriteFile(repo + "/.de/review/request.md",
                                       "# Review request\n\n## 验收标准\n- foo returns 2\n");

        // 项目验证配置 + 用户同意。
        review::ProjectVerifyConfig project_config;
        project_config.language = "cpp";
        project_config.commands = {
                {"test", "test", {"ctest", "--test-dir", "build"}, ".", 900}};
        project_config.test_globs = {"tests/**"};
        std::string error;
        Expect(review::WriteProjectVerifyConfig(repo, project_config, &error),
               "write project review config: " + error);
        review::ProjectVerifyConfig loaded_project;
        Expect(review::LoadProjectVerifyConfig(repo, &loaded_project, &error),
               "load project review config: " + error);
        review::ReviewConsentStore consent(options.data_dir + "/review_consent.json");
        Expect(consent.Approve(repo, loaded_project.sha256, &error),
               "approve project config: " + error);

        review::ReviewPackRequest request;
        request.cwd = repo;
        request.feature = "Skill Loader";
        request.base = "HEAD";
        request.request_file = ".de/review/request.md";
        request.context_paths = {"docs/plan.md"};
        request.notes = "encoder note";
        request.producer = {"external", "claude-code", "", ""};
        const auto created = service.CreatePack(request);
        Expect(created.ok, "create pack: " + created.error);
        Expect(created.intent_source == "request-file", "request file intent");
        Expect(created.changed_files >= 1, "changed files counted");
        Expect(created.report_path_hint.find("docs/review/Skill-Loader-") == 0,
               "report hint uses normalized feature: " + created.report_path_hint);
        Expect(created.pack_path.find("docs/review/inbox/Skill-Loader-") == 0,
               "pack path uses normalized feature: " + created.pack_path);

        review::ReviewPack pack;
        Expect(review::LoadReviewPack(repo + "/" + created.pack_path, &pack, &error),
               "load pack: " + error);
        Expect(pack.schema == 1 && !pack.review_id.empty(), "pack schema/id");
        Expect(pack.repo_root == repo, "pack records repo root");
        Expect(pack.feature == "Skill-Loader", "feature normalized");
        Expect(pack.producer.kind == "external" && pack.producer.agent == "claude-code",
               "producer identity");
        Expect(pack.producer.request_file == ".de/review/request.md",
               "request file recorded");
        Expect(!pack.producer.request_sha256.empty(), "request hash recorded");
        Expect(pack.request_text.find("foo returns 2") != std::string::npos,
               "request text inlined");
        Expect(!pack.context_missing, "request file means context is present");
        Expect(pack.notes == "encoder note", "notes preserved");
        Expect(pack.context_paths.size() == 1 &&
                       pack.context_paths[0].path == "docs/plan.md" &&
                       !pack.context_paths[0].sha256.empty(),
               "context path hash");
        Expect(ContainsPath(pack.changed_files, "src/foo.cpp"),
               "modified tracked file listed");
        Expect(pack.diff.find("return 2") != std::string::npos, "diff contains change");

        bool untracked_test = false;
        bool untracked_secret = false;
        for (const auto& file : pack.untracked) {
            if (file.path == "tests/review_new_test.cpp") {
                untracked_test = true;
                Expect(file.content.find("TEST(New") != std::string::npos,
                       "untracked test content inlined");
            }
            if (file.path == ".env") {
                untracked_secret = true;
                Expect(file.redacted && file.content.empty(), "secret redacted");
            }
        }
        Expect(untracked_test, "untracked test listed");
        Expect(untracked_secret, "untracked secret listed");
        Expect(FindOmitted(pack.omitted, "asset.bin") != nullptr,
               "untracked binary listed in omitted");
        Expect(FindOmitted(pack.omitted, ".env") != nullptr &&
                       FindOmitted(pack.omitted, ".env")->reason == "secret",
               "secret omitted reason");
        Expect(FindOmitted(pack.omitted, "asset.bin") != nullptr &&
                       FindOmitted(pack.omitted, "asset.bin")->reason == "binary",
               "binary omitted reason");
        Expect(pack.diff.find("SECRET=value") == std::string::npos,
               "secret content must not enter the diff");
        Expect(pack.diff.find("docs/review/") == std::string::npos,
               "review artifacts must not enter the diff");
        Expect(pack.diff.find("tracked.txt") == std::string::npos,
               "tracked review artifacts must not enter the diff");
        review_test_support::WriteFile(repo + "/docs/review/tracked.txt", "v3\n");
        review::ReviewStaleResult artifact_stale =
                review::CheckReviewStale(pack, options.config, repo, {});
        Expect(!artifact_stale.stale,
               "changing tracked review artifacts must not stale the pack: " +
                       (artifact_stale.reasons.empty() ? std::string()
                                                       : artifact_stale.reasons.front()));

        bool project_command = false;
        for (const auto& command : pack.verification) {
            if (command.source == "project" && command.id == "test") {
                project_command = true;
            }
        }
        Expect(project_command, "consented project verification merged");
        Expect(!pack.project_verify_config_sha256.empty(),
               "project config hash recorded");

        // 时效：未变 -> 不 stale；新增 reviewer-owned 测试不 stale；改源码 -> stale。
        review::ReviewStaleResult stale =
                review::CheckReviewStale(pack, options.config, repo, {});
        Expect(!stale.stale, "fresh pack should not be stale: " +
                                     (stale.reasons.empty() ? std::string()
                                                            : stale.reasons.front()));

        const std::string owned_path = repo + "/tests/review_owned_test.cpp";
        review_test_support::WriteFile(owned_path, "TEST(Owned, Case) {}\n");
        review::ReviewState state;
        state.review_id = pack.review_id;
        state.reviewer_tests.push_back(
                {"tests/review_owned_test.cpp",
                 aiagent::util::Sha256HexFile(owned_path), "create", "repro"});
        const auto owned_paths = review::ReviewerOwnedTestPaths(state, repo);
        Expect(owned_paths.size() == 1 &&
                       owned_paths[0] == "tests/review_owned_test.cpp",
               "owned test hash matches");
        stale = review::CheckReviewStale(pack, options.config, repo, owned_paths);
        Expect(!stale.stale,
               "reviewer-owned test should not make the pack stale: " +
                       (stale.reasons.empty() ? std::string() : stale.reasons.front()));

        review_test_support::WriteFile(repo + "/src/foo.cpp", "int foo() { return 99; }\n");
        stale = review::CheckReviewStale(pack, options.config, repo, owned_paths);
        Expect(stale.stale, "source change after pack must be stale");
        Expect(!stale.reasons.empty(), "stale reasons recorded");

        // 无 request 时弱意图 -> git-log + context_missing。
        review_test_support::WriteFile(repo + "/src/foo.cpp",
                                       "int foo() { return 3; }\n");
        review::ReviewPackRequest weak;
        weak.cwd = repo;
        weak.feature = "weak-intent";
        const auto weak_created = service.CreatePack(weak);
        Expect(weak_created.ok, "weak intent pack: " + weak_created.error);
        review::ReviewPack weak_pack;
        Expect(review::LoadReviewPack(repo + "/" + weak_created.pack_path, &weak_pack,
                                      &error),
               "load weak pack: " + error);
        Expect(weak_pack.intent_source == "git-log", "weak intent source");
        Expect(weak_pack.context_missing, "git-log intent marks context missing");
        Expect(weak_pack.request_text.find("Recent commits") != std::string::npos,
               "git log excerpt present");

        // 文件名安全化。
        std::string feature_error;
        Expect(review::NormalizeFeature("a b\tc") == "a-b-c", "whitespace collapse");
        Expect(review::NormalizeFeature("skill-loader") == "skill-loader", "safe feature");
        Expect(review::NormalizeFeature("a/b", &feature_error).empty() && !feature_error.empty(),
               "slash rejected");
        Expect(review::NormalizeFeature("CON", &feature_error).empty(),
               "windows reserved name rejected");
        Expect(review::NormalizeFeature("..", &feature_error).empty(), "dot path rejected");
        Expect(review::NormalizeFeature(std::string(81, 'a'), &feature_error).empty(),
               "over-long feature rejected");

        std::filesystem::remove_all(repo);
    } catch (const std::exception& e) {
        std::cerr << "review pack test failed: " << e.what() << '\n';
        return 1;
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " review pack test(s) failed\n";
        return 1;
    }
    std::cout << "review pack test passed\n";
    return 0;
}
