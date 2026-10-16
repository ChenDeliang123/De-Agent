// LocalCodingTools / CompositeToolProvider / RunCommand 的单元测试。
// 所有文件操作在独立临时目录内进行，测试后清理。

#include <json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "aiagent/llm/deepseek_types.hpp"
#include "aiagent/permissions/permission_policy.hpp"
#include "aiagent/tools/composite_tool_provider.hpp"
#include "aiagent/tools/local_tools.hpp"
#include "aiagent/tools/tool_context.hpp"
#include "aiagent/tools/tool_provider.hpp"

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
                      ("aiagent_local_tools_" + std::to_string(::getpid()));
    std::filesystem::create_directories(path);
    return path;
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

aiagent::tools::ToolContext ContextFor(aiagent::permissions::PermissionMode mode,
                                       const std::string& cwd = {}) {
    aiagent::tools::ToolContext context;
    context.working_directory = cwd;
    context.policy = aiagent::permissions::PolicyForMode(mode);
    return context;
}

aiagent::tools::ToolContext FullContext(const std::string& cwd = {}) {
    return ContextFor(aiagent::permissions::PermissionMode::kFull, cwd);
}

class NamedProvider final : public aiagent::tools::IToolProvider {
public:
    explicit NamedProvider(std::string name) : name_(std::move(name)) {}

    std::vector<aiagent::llm::ToolSpec> ListTools() override {
        aiagent::llm::ToolSpec spec;
        spec.name = name_;
        spec.description = "fake tool";
        spec.parameters = nlohmann::json::object();
        return {spec};
    }

    aiagent::tools::ToolExecutionResult Execute(
            const std::string& name,
            const nlohmann::json&,
            const aiagent::tools::ToolContext&) override {
        aiagent::tools::ToolExecutionResult result;
        result.ok = (name == name_);
        result.text = "executed-" + name_;
        if (!result.ok) {
            result.error = "unknown tool";
        }
        return result;
    }

private:
    std::string name_;
};

void TestReadWriteEdit() {
    const auto dir = TempDir();
    aiagent::tools::LocalCodingTools tools(dir.string());
    const auto context = FullContext();

    WriteFile(dir / "hello.txt", "line1\nline2\nline3\n");

    // read 全文件
    auto result = tools.Execute("read", {{"path", "hello.txt"}}, context);
    Expect(result.ok, "read should succeed");
    Expect(result.text == "line1\nline2\nline3", "read should return full content");

    // read offset/limit
    result = tools.Execute("read", {{"path", "hello.txt"}, {"offset", 2}, {"limit", 1}},
                           context);
    Expect(result.ok, "read with offset should succeed");
    Expect(result.text == "line2", "read should honor offset/limit");

    // write
    result = tools.Execute("write", {{"path", "out.txt"}, {"content", "hello world"}},
                           context);
    Expect(result.ok, "write should succeed");
    Expect(ReadAll(dir / "out.txt") == "hello world", "write should persist content");

    // edit 唯一替换
    WriteFile(dir / "edit.txt", "foo bar baz");
    result = tools.Execute("edit",
                           {{"path", "edit.txt"}, {"oldText", "bar"}, {"newText", "qux"}},
                           context);
    Expect(result.ok, "edit should succeed");
    Expect(ReadAll(dir / "edit.txt") == "foo qux baz", "edit should replace oldText");

    // edit oldText 不存在
    result = tools.Execute("edit",
                           {{"path", "edit.txt"}, {"oldText", "missing"}, {"newText", "x"}},
                           context);
    Expect(!result.ok, "edit should fail when oldText is absent");

    // edit oldText 不唯一
    WriteFile(dir / "dup.txt", "bar foo bar");
    result = tools.Execute("edit",
                           {{"path", "dup.txt"}, {"oldText", "bar"}, {"newText", "x"}},
                           context);
    Expect(!result.ok, "edit should fail when oldText is not unique");

    // edit 多块：一次调用替换多处，且按原文位置应用（与数组顺序无关）。
    WriteFile(dir / "multi.txt", "alpha\nbeta\ngamma\ndelta\n");
    result = tools.Execute("edit",
                           {{"path", "multi.txt"},
                            {"edits", nlohmann::json::array(
                                              {{{"oldText", "delta"}, {"newText", "DELTA"}},
                                               {{"oldText", "alpha"}, {"newText", "ALPHA"}}})}},
                           context);
    Expect(result.ok, "multi-block edit should succeed");
    Expect(ReadAll(dir / "multi.txt") == "ALPHA\nbeta\ngamma\nDELTA\n",
           "multi-block edit should apply every block by position");
    Expect(result.text.find("2 replacement(s)") != std::string::npos,
           "multi-block edit should report how many blocks were applied");

    // 原子性：其中一块不匹配时整个调用失败，文件保持原样。
    const std::string before_failure = ReadAll(dir / "multi.txt");
    result = tools.Execute("edit",
                           {{"path", "multi.txt"},
                            {"edits", nlohmann::json::array(
                                              {{{"oldText", "beta"}, {"newText", "BETA"}},
                                               {{"oldText", "missing"}, {"newText", "X"}}})}},
                           context);
    Expect(!result.ok, "multi-block edit should fail when one block does not match");
    Expect(ReadAll(dir / "multi.txt") == before_failure,
           "a failing multi-block edit must not modify the file");
    Expect(result.error.find("edits[1]") != std::string::npos,
           "error message should point at the failing block index");

    // 不唯一的块要拒绝。
    WriteFile(dir / "multi.txt", "dup dup\n");
    result = tools.Execute("edit",
                           {{"path", "multi.txt"},
                            {"edits", nlohmann::json::array(
                                              {{{"oldText", "dup"}, {"newText", "x"}}})}},
                           context);
    Expect(!result.ok, "multi-block edit should reject a non-unique oldText");

    // 区间重叠要拒绝，且不落盘。
    WriteFile(dir / "multi.txt", "abcdef\n");
    result = tools.Execute("edit",
                           {{"path", "multi.txt"},
                            {"edits", nlohmann::json::array(
                                              {{{"oldText", "abcd"}, {"newText", "A"}},
                                               {{"oldText", "cdef"}, {"newText", "B"}}})}},
                           context);
    Expect(!result.ok, "multi-block edit should reject overlapping blocks");
    Expect(ReadAll(dir / "multi.txt") == "abcdef\n",
           "overlapping blocks must leave the file untouched");

    // 参数形态错误：空数组、以及与 oldText 混用。
    result = tools.Execute("edit",
                           {{"path", "multi.txt"}, {"edits", nlohmann::json::array()}},
                           context);
    Expect(!result.ok, "empty edits array should be rejected");
    result = tools.Execute("edit",
                           {{"path", "multi.txt"},
                            {"oldText", "abc"},
                            {"edits", nlohmann::json::array(
                                              {{{"oldText", "abc"}, {"newText", "x"}}})}},
                           context);
    Expect(!result.ok, "mixing edits[] with oldText should be rejected");

    std::filesystem::remove_all(dir);
}

void TestBash() {
    const auto dir = TempDir();
    aiagent::tools::LocalCodingTools tools(dir.string());
    const auto context = FullContext();

    auto result = tools.Execute("bash", {{"command", "echo hello"}}, context);
    Expect(result.ok, "bash echo should succeed");
    Expect(result.text.find("hello") != std::string::npos, "bash should capture stdout");

    result = tools.Execute("bash", {{"command", "exit 3"}}, context);
    Expect(!result.ok, "bash non-zero exit should fail");
    Expect(result.error.find("code 3") != std::string::npos,
           "bash should report the exit code");

    // 超时：sleep 5s，timeout 1s。
    result = tools.Execute("bash", {{"command", "sleep 5"}, {"timeout", 1}}, context);
    Expect(!result.ok, "bash timeout should fail");
    Expect(result.error.find("timed out") != std::string::npos,
           "bash should report timeout");

    std::filesystem::remove_all(dir);
}

void TestComposite() {
    auto composite = std::make_shared<aiagent::tools::CompositeToolProvider>();
    composite->Add(std::make_shared<NamedProvider>("fake_a"));
    composite->Add(std::make_shared<NamedProvider>("fake_b"));
    composite->Add(std::make_shared<NamedProvider>("fake_a"));  // 重名，应被去重

    const auto tools = composite->ListTools();
    Expect(tools.size() == 2, "composite should dedupe by tool name");
    Expect(tools[0].name == "fake_a" && tools[1].name == "fake_b",
           "composite should keep first provider's tools in order");

    const auto context = FullContext();
    const auto result = composite->Execute("fake_b", nlohmann::json::object(), context);
    Expect(result.ok && result.text == "executed-fake_b",
           "composite should route execution by name");

    const auto unknown = composite->Execute("nope", nlohmann::json::object(), context);
    Expect(!unknown.ok, "composite should fail on unknown tool");
}

void TestSandbox() {
    const auto parent = TempDir();
    const auto work = parent / "work";
    const auto outside = parent / "outside";
    std::filesystem::create_directories(work);
    std::filesystem::create_directories(outside);
    WriteFile(work / "inside.txt", "inside");
    WriteFile(outside / "secret.txt", "secret");

    aiagent::tools::LocalCodingTools guarded(work.string());
    const auto restricted = ContextFor(aiagent::permissions::PermissionMode::kWorkspace);

    // 工作目录内正常读写。
    auto result = guarded.Execute("read", {{"path", "inside.txt"}}, restricted);
    Expect(result.ok && result.text == "inside",
           "sandbox should allow files inside cwd");

    // `..` 规范化后仍在工作目录内，应允许。
    result = guarded.Execute("read", {{"path", "../work/inside.txt"}}, restricted);
    Expect(result.ok && result.text == "inside",
           "sandbox should allow paths that normalize back inside cwd");

    // 相对路径逃逸。
    result = guarded.Execute("read", {{"path", "../outside/secret.txt"}}, restricted);
    Expect(!result.ok, "sandbox should block ../ escape");
    Expect(result.error.find("sandbox") != std::string::npos,
           "escape error should mention sandbox");

    // 绝对路径逃逸（write 不得创建文件）。
    result = guarded.Execute("write",
                             {{"path", (outside / "hack.txt").string()}, {"content", "x"}},
                             restricted);
    Expect(!result.ok, "sandbox should block absolute path outside");
    Expect(!std::filesystem::exists(outside / "hack.txt"),
           "blocked write must not create file");

    // edit 逃逸。
    result = guarded.Execute("edit",
                             {{"path", (outside / "secret.txt").string()},
                              {"oldText", "secret"},
                              {"newText", "x"}},
                             restricted);
    Expect(!result.ok, "sandbox should block edit outside");

    // 符号链接逃逸：工作目录内的链接指向外部，canonical 化后应被拒绝。
    std::error_code ec;
    std::filesystem::create_symlink(outside / "secret.txt", work / "link.txt", ec);
    if (!ec) {
        result = guarded.Execute("read", {{"path", "link.txt"}}, restricted);
        Expect(!result.ok, "sandbox should block symlink escape");
    }

    // full 策略不限制范围（allow_outside）。
    aiagent::tools::LocalCodingTools unrestricted(work.string());
    result = unrestricted.Execute("read", {{"path", (outside / "secret.txt").string()}},
                                  FullContext());
    Expect(result.ok && result.text == "secret",
           "full policy should allow outside path");

    // 额外放行的根目录：构造时传入 extra_roots。
    aiagent::tools::LocalCodingTools allowed(work.string(), {outside.string()});
    result = allowed.Execute("read", {{"path", (outside / "secret.txt").string()}},
                             restricted);
    Expect(result.ok && result.text == "secret",
           "extra_roots should allow extra root under restricted policy");

    std::filesystem::remove_all(parent);
}

void TestWorkingDirectoryOverride() {
    const auto parent = TempDir();
    const auto dir_a = parent / "dir_a";
    const auto dir_b = parent / "dir_b";
    std::filesystem::create_directories(dir_a);
    std::filesystem::create_directories(dir_b);
    WriteFile(dir_a / "hello.txt", "from-a");
    WriteFile(dir_b / "hello.txt", "from-b");

    aiagent::tools::LocalCodingTools tools(dir_a.string());
    const auto restricted = aiagent::permissions::PermissionMode::kWorkspace;

    // 默认 cwd 为 dir_a。
    auto result = tools.Execute("read", {{"path", "hello.txt"}}, ContextFor(restricted));
    Expect(result.ok && result.text == "from-a", "default cwd should be dir_a");

    // 显式 per-request cwd 后应改为 dir_b（沙箱根随之变为 dir_b）。
    result = tools.Execute("read", {{"path", "hello.txt"}},
                           ContextFor(restricted, dir_b.string()));
    Expect(result.ok && result.text == "from-b",
           "per-request working_directory should override default cwd");

    result = tools.Execute("bash", {{"command", "pwd"}}, FullContext(dir_b.string()));
    Expect(result.ok && result.text.find(dir_b.string()) != std::string::npos,
           "bash should run in per-request working_directory");

    std::filesystem::remove_all(parent);
}

// ls / glob / grep 三个只读检索工具。
void TestLsGlobGrep() {
    const auto parent = TempDir();
    const auto root = parent / "search_root";
    const auto outside = parent / "outside";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(outside);
    WriteFile(outside / "secret.txt", "FIND_ME outside\n");

    // 目录树：根下若干子目录，含隐藏条目、深层嵌套、二进制与大文件。
    for (const char* dir : {".hidden", "docs/nested", "src/sub", "lv1/lv2/lv3/lv4/lv5",
                            "misc/hits", "misc/many", "misc/re", "misc/long", "misc/wide",
                            "misc/special", "empty"}) {
        std::filesystem::create_directories(root / dir);
    }
    WriteFile(root / "alpha.txt", "alpha\n");
    WriteFile(root / ".hiddenfile.txt", "hidden head\n");
    WriteFile(root / ".hidden" / "secret.txt", "hidden secret\n");
    WriteFile(root / "docs" / "readme.md", "FIND_ME in docs\n");
    WriteFile(root / "docs" / ".dot.md", "hidden md\n");
    WriteFile(root / "docs" / "nested" / "x.txt", "nested FIND_ME x\n");
    WriteFile(root / "src" / "main.cpp", "int main() {}\n");
    WriteFile(root / "src" / "util.hpp", "#pragma once\n#define FIND_ME 1\n");
    WriteFile(root / "src" / "sub" / "deep.cpp", "// FIND_ME too\n");
    WriteFile(root / "src" / "sub" / "nested.hpp", "#pragma once\n");
    WriteFile(root / "lv1" / "lv2" / "lv3" / "lv4" / "lv5" / "file.txt", "deep\n");
    WriteFile(root / "misc" / "big.txt", "BIGTOKEN\n" + std::string(1100 * 1024, 'x'));
    std::string binary("head\0", 5);
    binary += "BINARYTOKEN";
    WriteFile(root / "misc" / "binary.bin", binary);
    WriteFile(root / "misc" / "hits" / "lines.txt",
              "HIT one\nHIT two\nHIT three\nHIT four\n");
    for (int i = 1; i <= 10; ++i) {
        const std::string index = i < 10 ? "0" + std::to_string(i) : std::to_string(i);
        WriteFile(root / "misc" / "many" / ("f" + index + ".txt"), "x\n");
    }
    WriteFile(root / "misc" / "re" / "sample.txt", "axb\na.b\ncrlf line\r\n");
    WriteFile(root / "misc" / "special" / "a+b[1].txt", "x\n");
    WriteFile(root / "misc" / "long" / "line.txt",
              "LONGTOKEN" + std::string(590, 'y') + "\n");
    for (int i = 0; i < 300; ++i) {
        WriteFile(root / "misc" / "wide" /
                          (std::string(170, 'w') + std::to_string(1000 + i) + ".txt"),
                  "x\n");
    }
    // 目录符号链接：不进入、也不按目录列出来。
    std::error_code ec;
    std::filesystem::create_directory_symlink(root / "docs", root / "misc" / "link", ec);

    aiagent::tools::LocalCodingTools tools(root.string());
    const auto context = FullContext();
    const auto restricted = ContextFor(aiagent::permissions::PermissionMode::kWorkspace);

    // 工具注册。
    const auto specs = tools.ListTools();
    Expect(specs.size() == 7, "ListTools should register seven tools");
    Expect(specs[4].name == "ls" && specs[5].name == "glob" && specs[6].name == "grep",
           "ListTools should register ls / glob / grep");

    // ---- ls ----
    auto result = tools.Execute("ls", {{"path", "."}, {"depth", 1}}, context);
    Expect(result.ok, "ls should succeed");
    Expect(result.text == "alpha.txt\ndocs/\nempty/\nlv1/\nmisc/\nsrc/",
           "ls depth 1 should list sorted entries with '/' on directories");

    // 默认 depth=2：看得到两级，隐藏条目与目录符号链接的子树不可见。
    result = tools.Execute("ls", {{"path", "."}}, context);
    Expect(result.ok && result.text.find("docs/nested/\n") != std::string::npos &&
                   result.text.find("docs/readme.md") != std::string::npos &&
                   result.text.find("src/sub/") != std::string::npos &&
                   result.text.find("src/main.cpp") != std::string::npos,
           "ls should recurse to depth 2 by default");
    Expect(result.text.find("docs/nested/x.txt") == std::string::npos &&
                   result.text.find("src/sub/deep.cpp") == std::string::npos,
           "ls should not recurse past depth 2");
    Expect(result.text.find(".hidden") == std::string::npos &&
                   result.text.find("link/") == std::string::npos,
           "ls should skip hidden entries and not descend into directory symlinks");

    // 目录条目带 '/'，同层字典序、目录与文件混排。
    result = tools.Execute("ls", {{"path", "docs"}}, context);
    Expect(result.ok && result.text == "nested/\nnested/x.txt\nreadme.md",
           "ls should list depth 2 entries in order and skip hidden files");
    result = tools.Execute("ls", {{"path", "docs"}, {"depth", 1}}, context);
    Expect(result.text == "nested/\nreadme.md",
           "ls depth 1 should stop after the direct children");

    // depth 夹取到 1..4。
    result = tools.Execute("ls", {{"path", "lv1"}, {"depth", 0}}, context);
    Expect(result.text == "lv2/", "ls should clamp depth below 1 to 1");
    result = tools.Execute("ls", {{"path", "lv1"}, {"depth", 99}}, context);
    Expect(result.text == tools.Execute("ls", {{"path", "lv1"}, {"depth", 4}}, context).text,
           "ls should clamp depth above 4 to 4");
    Expect(result.text.find("lv2/lv3/lv4/lv5/") != std::string::npos &&
                   result.text.find("file.txt") == std::string::npos,
           "ls should list exactly four levels when depth is clamped");

    // limit 截断标记。
    result = tools.Execute("ls", {{"path", "."}, {"depth", 1}, {"limit", 3}}, context);
    Expect(result.text == "alpha.txt\ndocs/\nempty/\n... (truncated)",
           "ls should append a truncation marker when limit is exceeded");
    result = tools.Execute("ls", {{"path", "."}, {"depth", 1}, {"limit", 6}}, context);
    Expect(result.text == "alpha.txt\ndocs/\nempty/\nlv1/\nmisc/\nsrc/",
           "ls should not mark truncation when exactly limit entries exist");

    // 空目录 / 不存在的路径 / 非目录。
    result = tools.Execute("ls", {{"path", "empty"}}, context);
    Expect(result.ok && result.text == "(empty)",
           "ls on an empty directory should report (empty)");
    result = tools.Execute("ls", {{"path", "missing"}}, context);
    Expect(!result.ok, "ls on a missing path should fail");
    result = tools.Execute("ls", {{"path", "alpha.txt"}}, context);
    Expect(!result.ok, "ls on a file should fail");
    result = tools.Execute("ls", {{"path", "../outside"}}, restricted);
    Expect(!result.ok && result.error.find("sandbox") != std::string::npos,
           "ls should reject a path outside the sandbox");

    // path 省略时用 working_directory（空则构造时的 cwd）。
    result = tools.Execute("ls", nlohmann::json::object(), context);
    Expect(result.text.find("alpha.txt") != std::string::npos,
           "ls should default to the constructor working directory");
    result = tools.Execute("ls", nlohmann::json::object(),
                           FullContext((root / "misc").string()));
    Expect(result.text.find("big.txt") != std::string::npos,
           "ls should use the per-request working directory");

    // ---- glob ----
    result = tools.Execute("glob", {{"pattern", "**/*.hpp"}, {"path", "."}}, context);
    Expect(result.ok && result.text == "src/sub/nested.hpp\nsrc/util.hpp",
           "glob **/*.hpp should return matching files sorted by path");

    result = tools.Execute("glob", {{"pattern", "src/*.hpp"}, {"path", "."}}, context);
    Expect(result.text == "src/util.hpp", "glob '*' should not cross path separators");

    result = tools.Execute("glob", {{"pattern", "*.txt"}, {"path", "."}}, context);
    Expect(result.text == "alpha.txt", "glob '*.txt' should match root-level files only");

    result = tools.Execute("glob", {{"pattern", "**/alpha.txt"}, {"path", "."}}, context);
    Expect(result.text == "alpha.txt", "glob '**' should also match zero path segments");

    result = tools.Execute("glob", {{"pattern", "**/*.txt"}, {"path", "docs"}}, context);
    Expect(result.text == "nested/x.txt", "glob should match files at any depth");

    // pattern 里的正则元字符按字面处理。
    result = tools.Execute("glob", {{"pattern", "a+b[1].txt"}, {"path", "misc/special"}},
                           context);
    Expect(result.text == "a+b[1].txt",
           "glob should treat regex metacharacters in a pattern as literals");

    result = tools.Execute("glob", {{"pattern", "**"}, {"path", "docs"}}, context);
    Expect(result.text == "nested/x.txt\nreadme.md",
           "glob should return only files, sorted, and skip hidden entries");

    result = tools.Execute("glob", {{"pattern", "*.txt"}, {"path", "misc/many"}}, context);
    Expect(result.text ==
                   "f01.txt\nf02.txt\nf03.txt\nf04.txt\nf05.txt\nf06.txt\nf07.txt\nf08.txt\n"
                   "f09.txt\nf10.txt",
           "glob should sort matches lexicographically");

    result = tools.Execute("glob",
                           {{"pattern", "*.txt"}, {"path", "misc/many"}, {"limit", 4}},
                           context);
    Expect(result.text == "f01.txt\nf02.txt\nf03.txt\nf04.txt\n... (truncated)",
           "glob should append a truncation marker when limit is exceeded");

    result = tools.Execute("glob", {{"pattern", "**/*.zzz"}, {"path", "."}}, context);
    Expect(result.ok && result.text == "no matches",
           "glob without matches should return ok and 'no matches'");
    result = tools.Execute("glob", {{"pattern", "*"}, {"path", "empty"}}, context);
    Expect(result.ok && result.text == "no matches",
           "glob on an empty directory should return ok and 'no matches'");
    result = tools.Execute("glob", {{"pattern", ""}, {"path", "."}}, context);
    Expect(!result.ok, "glob should reject an empty pattern");

    // 目录符号链接不进入（misc/link -> docs）。
    result = tools.Execute("glob", {{"pattern", "**/*.md"}, {"path", "misc"}}, context);
    Expect(result.ok && result.text == "no matches",
           "glob should not follow directory symlinks");

    // 50KB 字节截断。
    result = tools.Execute("glob",
                           {{"pattern", "*.txt"}, {"path", "misc/wide"}, {"limit", 500}},
                           context);
    Expect(result.ok && result.text.size() > 50 * 1024 &&
                   result.text.size() <= 50 * 1024 + 16,
           "glob output should be truncated to 50KB");
    Expect(result.text.size() >= 16 &&
                   result.text.substr(result.text.size() - 16) == "\n... (truncated)",
           "glob should mark byte-level truncation");

    // 沙箱：越界 path 被拒（full 策略下同样的 path 允许）。
    result = tools.Execute("glob", {{"pattern", "*"}, {"path", "../outside"}}, restricted);
    Expect(!result.ok && result.error.find("sandbox") != std::string::npos,
           "glob should reject a path outside the sandbox");

    // ---- grep ----
    const std::string find_me =
            "docs/nested/x.txt:1: nested FIND_ME x\n"
            "docs/readme.md:1: FIND_ME in docs\n"
            "src/sub/deep.cpp:1: // FIND_ME too\n"
            "src/util.hpp:2: #define FIND_ME 1";
    result = tools.Execute("grep", {{"pattern", "FIND_ME"}}, context);
    Expect(result.ok && result.text == find_me,
           "grep should report 'path:line: content' and skip hidden entries");

    result = tools.Execute("grep", {{"pattern", "FIND_ME"}, {"include", "*.hpp"}}, context);
    Expect(result.text == "src/util.hpp:2: #define FIND_ME 1",
           "grep include '*.hpp' should filter by file name");
    result = tools.Execute("grep", {{"pattern", "FIND_ME"}, {"include", "**/*.md"}}, context);
    Expect(result.text == "docs/readme.md:1: FIND_ME in docs",
           "grep include should support ** globs");
    result = tools.Execute("grep", {{"pattern", "FIND_ME"}, {"include", "src/**"}}, context);
    Expect(result.text == "src/sub/deep.cpp:1: // FIND_ME too\n"
                          "src/util.hpp:2: #define FIND_ME 1",
           "grep include with '/' should match relative paths");

    result = tools.Execute("grep", {{"pattern", "find_me"}, {"ignore_case", true}}, context);
    Expect(result.text == find_me, "grep ignore_case should match regardless of case");
    result = tools.Execute("grep", {{"pattern", "find_me"}}, context);
    Expect(result.ok && result.text == "no matches", "grep should be case-sensitive by default");

    // literal 与 regex 的差异：'a.b'。
    result = tools.Execute("grep", {{"pattern", "a.b"}, {"path", "misc/re"}}, context);
    Expect(result.text == "sample.txt:1: axb\nsample.txt:2: a.b",
           "grep should treat pattern as a regex by default");
    result = tools.Execute("grep",
                           {{"pattern", "a.b"}, {"path", "misc/re"}, {"literal", true}},
                           context);
    Expect(result.text == "sample.txt:2: a.b",
           "grep literal should not interpret regex metacharacters");

    // 行尾 \r 去掉、单行截断到 500 字符。
    result = tools.Execute("grep", {{"pattern", "crlf"}, {"path", "misc/re"}}, context);
    Expect(result.text == "sample.txt:3: crlf line" &&
                   result.text.find('\r') == std::string::npos,
           "grep should strip carriage returns");
    result = tools.Execute("grep", {{"pattern", "LONGTOKEN"}, {"path", "misc/long"}}, context);
    Expect(result.text ==
                   "line.txt:1: " + std::string("LONGTOKEN") + std::string(491, 'y') + "...",
           "grep should truncate long lines to 500 characters");

    // max_results 截断。
    result = tools.Execute("grep",
                           {{"pattern", "HIT"}, {"path", "misc/hits"}, {"max_results", 2}},
                           context);
    Expect(result.text == "lines.txt:1: HIT one\nlines.txt:2: HIT two\n... (truncated)",
           "grep should append a truncation marker at max_results");

    // 二进制文件与超过 1MiB 的文件跳过。
    result = tools.Execute("grep", {{"pattern", "BINARYTOKEN"}}, context);
    Expect(result.ok && result.text == "no matches", "grep should skip binary files");
    result = tools.Execute("grep", {{"pattern", "BIGTOKEN"}}, context);
    Expect(result.ok && result.text == "no matches", "grep should skip files larger than 1MB");

    // 无匹配 / 空目录 / 空 pattern / 非法正则。
    result = tools.Execute("grep", {{"pattern", "NOTHING_THERE"}}, context);
    Expect(result.ok && result.text == "no matches", "grep without matches should return ok");
    result = tools.Execute("grep", {{"pattern", "x"}, {"path", "empty"}}, context);
    Expect(result.ok && result.text == "no matches",
           "grep on an empty directory should return ok");
    result = tools.Execute("grep", {{"pattern", ""}}, context);
    Expect(!result.ok, "grep should reject an empty pattern");
    result = tools.Execute("grep", {{"pattern", "["}}, context);
    Expect(!result.ok && result.error.find("pattern") != std::string::npos,
           "grep should reject an invalid regex");

    // 目录符号链接不进入。
    result = tools.Execute("grep", {{"pattern", "FIND_ME"}, {"path", "misc"}}, context);
    Expect(result.ok && result.text == "no matches",
           "grep should not follow directory symlinks");

    // 沙箱：越界 path 被拒。
    result = tools.Execute("grep",
                           {{"pattern", "FIND_ME"}, {"path", "../outside"}}, restricted);
    Expect(!result.ok && result.error.find("sandbox") != std::string::npos,
           "grep should reject a path outside the sandbox");

    std::filesystem::remove_all(parent);
}

}  // namespace

int main() {
    TestReadWriteEdit();
    TestBash();
    TestComposite();
    TestSandbox();
    TestWorkingDirectoryOverride();
    TestLsGlobGrep();

    if (g_failures != 0) {
        std::cerr << "local tools test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "local tools test passed\n";
    return 0;
}
