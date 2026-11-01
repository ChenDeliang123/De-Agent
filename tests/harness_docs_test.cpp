// [prompt] harness_docs_root 的解析与渲染测试。
//
// 这里锁住四条契约：
//   1) 空配置 = 默认关闭，不产生任何提示词内容；
//   2) 相对路径相对配置文件目录解析，不依赖启动目录；
//   3) 只注入 README.md / docs 的路径指针，绝不读取正文；
//   4) 目录不存在或没有可用文档时，启动期明确报错而不是静默失效。

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "aiagent/agent/harness_docs.hpp"
#include "aiagent/platform/paths.hpp"

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

void ExpectContains(const std::string& haystack, const std::string& needle,
                    const std::string& message) {
    if (haystack.find(needle) == std::string::npos) {
        std::cerr << "FAILED: " << message << " (missing: " << needle << ")\n";
        ++g_failures;
    }
}

void ExpectNotContains(const std::string& haystack, const std::string& needle,
                       const std::string& message) {
    if (haystack.find(needle) != std::string::npos) {
        std::cerr << "FAILED: " << message << " (unexpected: " << needle << ")\n";
        ++g_failures;
    }
}

std::string GenericPath(const fs::path& path) {
    std::string text = aiagent::platform::ToUtf8(path);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

class TempDir {
public:
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("aiagent-harness-docs-" + std::to_string(stamp));
        fs::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void WriteFile(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

void TestDisabledByDefault() {
    std::string output = "stale";
    std::string error;
    Expect(aiagent::agent::BuildHarnessDocsInfo("", "/does/not/matter.ini", &output,
                                                &error),
           "empty harness_docs_root should be accepted");
    Expect(output.empty(), "empty harness_docs_root should clear output");
    Expect(error.empty(), "empty harness_docs_root should not report an error");
}

void TestRendersPointersWithoutReadingContent() {
    TempDir temp;
    const fs::path root = temp.path() / "repo";
    const fs::path readme = root / "README.md";
    const fs::path docs = root / "docs";
    WriteFile(readme, "SECRET README CONTENT");
    WriteFile(docs / "guide.md", "SECRET GUIDE CONTENT");

    std::string output;
    std::string error;
    Expect(aiagent::agent::BuildHarnessDocsInfo(GenericPath(root), "",
                                                &output, &error),
           "existing README.md + docs should be accepted");
    Expect(error.empty(), "valid harness_docs_root should not report an error");
    ExpectContains(output, "Read these only when the user asks about the harness",
                   "rendered content should constrain when to read the docs");
    ExpectContains(output, "Main documentation: " + GenericPath(readme),
                   "rendered content should point at README.md");
    ExpectContains(output, "Additional docs: " + GenericPath(docs),
                   "rendered content should point at docs/");
    ExpectNotContains(output, "SECRET README CONTENT",
                      "README.md body must not be inlined");
    ExpectNotContains(output, "SECRET GUIDE CONTENT",
                      "docs body must not be inlined");
}

void TestRelativeRootUsesConfigDirectory() {
    TempDir temp;
    const fs::path config = temp.path() / "conf" / "app.ini";
    const fs::path root = temp.path() / "repo";
    WriteFile(root / "README.md", "readme");
    WriteFile(config, "[prompt]\n");

    std::string output;
    std::string error;
    Expect(aiagent::agent::BuildHarnessDocsInfo("../repo", GenericPath(config),
                                                &output, &error),
           "relative harness_docs_root should resolve against the config directory");
    ExpectContains(output, "Main documentation: " + GenericPath(root / "README.md"),
                   "relative root should resolve to the expected README.md");
    ExpectNotContains(output, "Additional docs:",
                      "missing docs/ should not be advertised");
}

void TestOnlyOneArtifact() {
    TempDir temp;
    const fs::path docs_only = temp.path() / "docs-only";
    WriteFile(docs_only / "docs" / "a.md", "a");

    std::string output;
    std::string error;
    Expect(aiagent::agent::BuildHarnessDocsInfo(GenericPath(docs_only), "",
                                                &output, &error),
           "docs/ without README.md should be accepted");
    ExpectContains(output, "Additional docs: " + GenericPath(docs_only / "docs"),
                   "docs-only root should advertise docs/");
    ExpectNotContains(output, "Main documentation:",
                      "docs-only root should not advertise README.md");

    const fs::path readme_only = temp.path() / "readme-only";
    WriteFile(readme_only / "README.md", "readme");
    output.clear();
    error.clear();
    Expect(aiagent::agent::BuildHarnessDocsInfo(GenericPath(readme_only), "",
                                                &output, &error),
           "README.md without docs/ should be accepted");
    ExpectContains(output,
                   "Main documentation: " + GenericPath(readme_only / "README.md"),
                   "README-only root should advertise README.md");
    ExpectNotContains(output, "Additional docs:",
                      "README-only root should not advertise docs/");
}

void TestRejectsUnusableRoot() {
    TempDir temp;
    const fs::path missing = temp.path() / "missing";
    std::string output = "stale";
    std::string error;
    Expect(!aiagent::agent::BuildHarnessDocsInfo(GenericPath(missing), "",
                                                 &output, &error),
           "missing harness_docs_root should be rejected");
    Expect(output.empty(), "rejected root should not leave stale output");
    ExpectContains(error, "harness_docs_root is not a directory",
                   "missing root should report a clear error");

    const fs::path empty = temp.path() / "empty";
    fs::create_directories(empty);
    output = "stale";
    error.clear();
    Expect(!aiagent::agent::BuildHarnessDocsInfo(GenericPath(empty), "",
                                                 &output, &error),
           "directory without README.md/docs should be rejected");
    Expect(output.empty(), "unusable root should not leave stale output");
    ExpectContains(error, "contains neither README.md nor docs/",
                   "unusable root should report the missing artifacts");

    const fs::path file_as_root = temp.path() / "not-a-dir";
    WriteFile(file_as_root, "file");
    output = "stale";
    error.clear();
    Expect(!aiagent::agent::BuildHarnessDocsInfo(GenericPath(file_as_root), "",
                                                 &output, &error),
           "a file used as harness_docs_root should be rejected");
    ExpectContains(error, "harness_docs_root is not a directory",
                   "file root should report a clear error");
}

}  // namespace

int main() {
    TestDisabledByDefault();
    TestRendersPointersWithoutReadingContent();
    TestRelativeRootUsesConfigDirectory();
    TestOnlyOneArtifact();
    TestRejectsUnusableRoot();

    if (g_failures != 0) {
        std::cerr << "harness docs test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "harness docs test passed\n";
    return 0;
}
