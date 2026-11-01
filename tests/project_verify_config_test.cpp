#include <filesystem>
#include <iostream>
#include <string>

#include "aiagent/review/project_verify_config.hpp"
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

aiagent::review::ProjectVerifyCommand Cmd(std::string id, std::string kind,
                                          std::vector<std::string> argv,
                                          std::string cwd = ".",
                                          long timeout_s = 600) {
    aiagent::review::ProjectVerifyCommand command;
    command.id = std::move(id);
    command.kind = std::move(kind);
    command.argv = std::move(argv);
    command.cwd = std::move(cwd);
    command.timeout_s = timeout_s;
    return command;
}

}  // namespace

int main() {
    namespace review = aiagent::review;
    try {
        const std::string repo = review_test_support::UniqueDir("review_config_repo_");

        // CMake 检测。
        Expect(review_test_support::WriteFile(repo + "/CMakeLists.txt", "project(x)\n"),
               "write CMakeLists");
        review::VerifyDetectResult detected = review::DetectProjectVerifyConfig(repo);
        Expect(detected.found && detected.language == "cpp", "detect cpp");
        Expect(detected.commands.size() == 3, "cpp candidates: build/config/test");
        Expect(!detected.test_globs.empty(), "cpp test globs");

        // Node 检测：pnpm lockfile + scripts。
        const std::string node_repo = review_test_support::UniqueDir("review_config_node_");
        review_test_support::WriteFile(node_repo + "/package.json",
                                       R"({"scripts":{"build":"tsc","test":"node --test"}})");
        review_test_support::WriteFile(node_repo + "/pnpm-lock.yaml", "lockfileVersion: 9\n");
        detected = review::DetectProjectVerifyConfig(node_repo);
        Expect(detected.found && detected.language == "node", "detect node");
        Expect(detected.commands.size() == 2, "node build+test candidates");
        Expect(detected.commands[0].argv[0] == "pnpm", "pnpm lockfile selects pnpm");
        Expect(detected.commands[1].argv == std::vector<std::string>({"pnpm", "test"}),
               "node test command");

        // Rust / Go / Python 基本检测。
        const std::string rust_repo = review_test_support::UniqueDir("review_config_rust_");
        review_test_support::WriteFile(rust_repo + "/Cargo.toml", "[package]\n");
        detected = review::DetectProjectVerifyConfig(rust_repo);
        Expect(detected.found && detected.language == "rust" && detected.commands.size() == 2,
               "detect rust");
        const std::string go_repo = review_test_support::UniqueDir("review_config_go_");
        review_test_support::WriteFile(go_repo + "/go.mod", "module x\n");
        detected = review::DetectProjectVerifyConfig(go_repo);
        Expect(detected.found && detected.language == "go", "detect go");
        const std::string py_repo = review_test_support::UniqueDir("review_config_py_");
        review_test_support::WriteFile(py_repo + "/pyproject.toml", "[project]\nname='x'\n");
        detected = review::DetectProjectVerifyConfig(py_repo);
        Expect(detected.found && detected.language == "python", "detect python");

        // schema 校验。
        review::ProjectVerifyConfig config;
        config.language = "cpp";
        config.commands = {Cmd("build", "build", {"cmake", "--build", "build"}),
                           Cmd("test", "test", {"ctest", "--test-dir", "build"})};
        config.test_globs = {"tests/**", "src/**/*_test.cpp"};
        std::string error;
        Expect(review::ValidateProjectVerifyConfig(config, repo, &error),
               "valid config should pass: " + error);

        {
            auto bad = config;
            bad.commands[0].cwd = "../outside";
            Expect(!review::ValidateProjectVerifyConfig(bad, repo, &error),
                   "cwd traversal must be rejected");
        }
        {
            auto bad = config;
            bad.commands[0].argv = {"npm", "install"};
            Expect(!review::ValidateProjectVerifyConfig(bad, repo, &error),
                   "install subcommand must be rejected");
        }
        {
            auto bad = config;
            bad.commands.push_back(Cmd("build", "build", {"ctest"}));
            Expect(!review::ValidateProjectVerifyConfig(bad, repo, &error),
                   "duplicate ids must be rejected");
        }
        {
            auto bad = config;
            bad.commands[0].timeout_s = 0;
            Expect(!review::ValidateProjectVerifyConfig(bad, repo, &error),
                   "timeout out of range must be rejected");
        }
        {
            auto bad = config;
            bad.test_globs = {"**/*"};
            Expect(!review::ValidateProjectVerifyConfig(bad, repo, &error),
                   "broad test glob must be rejected");
        }
        {
            auto bad = config;
            bad.commands.clear();
            for (int i = 0; i < 9; ++i) {
                bad.commands.push_back(Cmd("c" + std::to_string(i), "test", {"ctest"}));
            }
            Expect(!review::ValidateProjectVerifyConfig(bad, repo, &error),
                   "more than 8 commands must be rejected");
        }

        // 写入 + 读回 + .de/.gitignore。
        Expect(review::WriteProjectVerifyConfig(repo, config, &error),
               "write config: " + error);
        Expect(std::filesystem::exists(repo + "/.de/review.json"), "review.json exists");
        Expect(review_test_support::ReadFile(repo + "/.de/.gitignore").find("*") !=
                       std::string::npos,
               "review.json is ignored");
        review::ProjectVerifyConfig loaded;
        Expect(review::LoadProjectVerifyConfig(repo, &loaded, &error),
               "load config: " + error);
        Expect(loaded.commands.size() == 2 && loaded.test_globs.size() == 2,
               "config round-trips");
        Expect(!loaded.sha256.empty(), "loaded config has sha256");

        // 同意记录：缺失 -> 批准 -> 有效 -> 改配置失效 -> 重新加载仍失效。
        const std::string consent_path = repo + "/consent.json";
        review::ReviewConsentStore consent(consent_path);
        Expect(consent.Check(repo, loaded.sha256).status == "missing",
               "consent missing by default");
        Expect(consent.Approve(repo, loaded.sha256, &error), "approve consent: " + error);
        Expect(consent.Check(repo, loaded.sha256).status == "valid",
               "approved consent valid");
        review::ReviewConsentStore reloaded(consent_path);
        Expect(reloaded.Check(repo, loaded.sha256).status == "valid",
               "consent persists");
        const std::string other_sha = aiagent::util::Sha256Hex("changed");
        Expect(reloaded.Check(repo, other_sha).status == "invalid",
               "changed config invalidates consent");

        // 坏配置读取失败但不抛异常。
        review_test_support::WriteFile(repo + "/.de/review.json", "{ not json");
        Expect(!review::LoadProjectVerifyConfig(repo, &loaded, &error) && !error.empty(),
               "invalid review.json reports an error");

        // 测试路径分类。
        Expect(review::IsTestPath("tests/foo.cpp", {}), "tests dir");
        Expect(review::IsTestPath("src/foo_test.cpp", {}), "suffix");
        Expect(review::IsTestPath("src/__tests__/x.ts", {}), "dunder tests");
        Expect(!review::IsTestPath("src/foo.cpp", {}), "source is not a test path");
        Expect(review::IsTestPath("spec/foo.rb", {}), "spec dir");
        Expect(review::IsTestPath("src/foo.cpp", {"src/**/*_test.cpp"}) == false,
               "glob must match");
        Expect(review::IsTestPath("src/foo_test.cpp", {"src/**/*_test.cpp"}),
               "custom test glob matches");

        // pack allowlist 与项目命令黑名单。
        Expect(review::IsPackCommandAllowed({"cmake", "--build", "build"}),
               "cmake build allowed");
        Expect(!review::IsPackCommandAllowed({"cmake", "--install", "build"}),
               "cmake install rejected");
        Expect(review::IsPackCommandAllowed({"node", "--test"}),
               "node --test allowed");
        Expect(!review::IsPackCommandAllowed({"node", "script.js"}),
               "node script rejected");
        Expect(review::IsPackCommandAllowed({"go", "test", "./..."}), "go test allowed");
        Expect(!review::IsPackCommandAllowed({"go", "install"}), "go install rejected");
        Expect(review::IsForbiddenProjectCommand({"cargo", "publish"}),
               "cargo publish forbidden");
        Expect(!review::IsForbiddenProjectCommand({"custom-tool", "run", "tests"}),
               "custom tool allowed for project config");
    } catch (const std::exception& e) {
        std::cerr << "project verify config test failed: " << e.what() << '\n';
        return 1;
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " project verify config test(s) failed\n";
        return 1;
    }
    std::cout << "project verify config test passed\n";
    return 0;
}
