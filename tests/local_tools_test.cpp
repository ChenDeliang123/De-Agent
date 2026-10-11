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

}  // namespace

int main() {
    TestReadWriteEdit();
    TestBash();
    TestComposite();
    TestSandbox();
    TestWorkingDirectoryOverride();

    if (g_failures != 0) {
        std::cerr << "local tools test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "local tools test passed\n";
    return 0;
}
