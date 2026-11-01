// 审核测试共用：临时 git 仓库、文件写入、argv 断言的薄封装。
#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "aiagent/platform/paths.hpp"
#include "aiagent/tools/run_command.hpp"

#include "test_support.hpp"

namespace review_test_support {

inline std::string UniqueDir(const std::string& prefix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path dir =
            aiagent::platform::TempDir() /
            aiagent::platform::FromUtf8(prefix + std::to_string(test_support::Pid()) + "_" +
                                        std::to_string(stamp));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return aiagent::platform::ToUtf8(dir);
}

inline bool WriteFile(const std::string& path, const std::string& content) {
    const std::filesystem::path target = aiagent::platform::FromUtf8(path);
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output << content;
    return output.good();
}

inline std::string ReadFile(const std::string& path) {
    std::ifstream input(aiagent::platform::FromUtf8(path), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    return content;
}

inline bool Run(const std::string& command, const std::string& cwd) {
    const aiagent::tools::CommandResult result =
            aiagent::tools::RunCommand(command, cwd, 60);
    return result.ok;
}

inline bool InitGitRepo(const std::string& repo) {
    return Run("git init -q", repo) && Run("git config user.email review@test.local", repo) &&
           Run("git config user.name review-test", repo) &&
           Run("git config commit.gpgsign false", repo);
}

inline bool CommitAll(const std::string& repo, const std::string& message) {
    return Run("git add -A", repo) && Run("git commit -qm \"" + message + "\"", repo);
}

}  // namespace review_test_support
