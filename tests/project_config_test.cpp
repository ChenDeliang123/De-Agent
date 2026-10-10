// 项目级 `.de/` 只读分层配置的单元测试：项目根探测、加载与缓存。

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "aiagent/permissions/project_config.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

std::filesystem::path TempDir() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("aiagent_project_cfg_" + std::to_string(::getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

using aiagent::permissions::BashDecision;
using aiagent::permissions::EvaluateBashPolicy;
using aiagent::permissions::FindProjectRoot;
using aiagent::permissions::PermissionMode;
using aiagent::permissions::ProjectConfigCache;

void TestFindProjectRoot() {
    const auto dir = TempDir();
    const auto proj = dir / "proj";
    std::filesystem::create_directories(proj / ".git");
    std::filesystem::create_directories(proj / "src" / "sub");
    const std::string root = std::filesystem::weakly_canonical(proj).string();
    Expect(FindProjectRoot((proj / "src" / "sub").string()) == root,
           "nearest .git ancestor is the project root");

    const auto plain = dir / "plain";
    std::filesystem::create_directories(plain / "a");
    Expect(FindProjectRoot((plain / "a").string()).empty(),
           "no .git and no .de -> empty root");

    std::filesystem::create_directories(plain / ".de");
    Expect(FindProjectRoot(plain.string()) ==
                   std::filesystem::weakly_canonical(plain).string(),
           "fallback: cwd with .de is a project root");

    std::filesystem::remove_all(dir);
}

void TestCacheLoadAndTighten() {
    const auto dir = TempDir();
    const auto proj = dir / "proj";
    std::filesystem::create_directories(proj / ".git");
    std::filesystem::create_directories(proj / ".de");
    const std::string root = std::filesystem::weakly_canonical(proj).string();

    WriteFile(proj / ".de" / "permissions.json",
              "{\"version\":1,\"paths\":{\"" + root +
                      "\":{\"mode\":\"confirm\"}}}");
    WriteFile(proj / ".de" / "bash_policy.rules",
              "deny badcmd\nallow git status\n");

    ProjectConfigCache cache;
    const auto config = cache.Get((proj / "src").string());
    Expect(config != nullptr, "project config is loaded from a subdirectory");
    Expect(config->root == root, "root recorded");
    Expect(!config->trusted, "P0 project is untrusted");
    Expect(config->permissions != nullptr, "project permissions loaded");
    if (config->permissions != nullptr) {
        const auto entry = config->permissions->Resolve(root);
        Expect(entry.has_value() && entry->mode == PermissionMode::kConfirm,
               "project mode is confirm");
    }
    Expect(config->bash_policy != nullptr, "project bash policy loaded");
    if (config->bash_policy != nullptr) {
        Expect(EvaluateBashPolicy("badcmd x", *config->bash_policy) ==
                       BashDecision::kDeny,
               "project deny rule applies");
    }

    Expect(cache.Get(proj.string()) == config, "cache returns the same instance");

    const auto no_de = dir / "no_de";
    std::filesystem::create_directories(no_de / ".git");
    Expect(cache.Get(no_de.string()) == nullptr, "no .de -> nullptr");

    std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
    TestFindProjectRoot();
    TestCacheLoadAndTighten();

    if (g_failures != 0) {
        std::cerr << "project config test failed with " << g_failures
                  << " failure(s)\n";
        return 1;
    }
    std::cout << "project config test passed\n";
    return 0;
}
