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
#include "aiagent/tools/composite_tool_provider.hpp"
#include "aiagent/tools/local_tools.hpp"
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

    aiagent::tools::ToolExecutionResult Execute(const std::string& name,
                                                const nlohmann::json&,
                                                const std::string&) override {
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

    WriteFile(dir / "hello.txt", "line1\nline2\nline3\n");

    // read 全文件
    auto result = tools.Execute("read", {{"path", "hello.txt"}});
    Expect(result.ok, "read should succeed");
    Expect(result.text == "line1\nline2\nline3", "read should return full content");

    // read offset/limit
    result = tools.Execute("read", {{"path", "hello.txt"}, {"offset", 2}, {"limit", 1}});
    Expect(result.ok, "read with offset should succeed");
    Expect(result.text == "line2", "read should honor offset/limit");

    // write
    result = tools.Execute("write", {{"path", "out.txt"}, {"content", "hello world"}});
    Expect(result.ok, "write should succeed");
    Expect(ReadAll(dir / "out.txt") == "hello world", "write should persist content");

    // edit 唯一替换
    WriteFile(dir / "edit.txt", "foo bar baz");
    result = tools.Execute("edit", {{"path", "edit.txt"},
                                    {"oldText", "bar"},
                                    {"newText", "qux"}});
    Expect(result.ok, "edit should succeed");
    Expect(ReadAll(dir / "edit.txt") == "foo qux baz", "edit should replace oldText");

    // edit oldText 不存在
    result = tools.Execute("edit", {{"path", "edit.txt"},
                                    {"oldText", "missing"},
                                    {"newText", "x"}});
    Expect(!result.ok, "edit should fail when oldText is absent");

    // edit oldText 不唯一
    WriteFile(dir / "dup.txt", "bar foo bar");
    result = tools.Execute("edit", {{"path", "dup.txt"},
                                    {"oldText", "bar"},
                                    {"newText", "x"}});
    Expect(!result.ok, "edit should fail when oldText is not unique");

    std::filesystem::remove_all(dir);
}

void TestBash() {
    const auto dir = TempDir();
    aiagent::tools::LocalCodingTools tools(dir.string());

    auto result = tools.Execute("bash", {{"command", "echo hello"}});
    Expect(result.ok, "bash echo should succeed");
    Expect(result.text.find("hello") != std::string::npos, "bash should capture stdout");

    result = tools.Execute("bash", {{"command", "exit 3"}});
    Expect(!result.ok, "bash non-zero exit should fail");
    Expect(result.error.find("code 3") != std::string::npos,
           "bash should report the exit code");

    // 超时：sleep 5s，timeout 1s。
    result = tools.Execute("bash", {{"command", "sleep 5"}, {"timeout", 1}});
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

    const auto result = composite->Execute("fake_b", nlohmann::json::object());
    Expect(result.ok && result.text == "executed-fake_b",
           "composite should route execution by name");

    const auto unknown = composite->Execute("nope", nlohmann::json::object());
    Expect(!unknown.ok, "composite should fail on unknown tool");
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

    // 默认 cwd 为 dir_a。
    auto result = tools.Execute("read", {{"path", "hello.txt"}});
    Expect(result.ok && result.text == "from-a", "default cwd should be dir_a");

    // 显式传入 per-request cwd 后应改为 dir_b。
    result = tools.Execute("read", {{"path", "hello.txt"}}, dir_b.string());
    Expect(result.ok && result.text == "from-b",
           "per-request working_directory should override default cwd");

    result = tools.Execute("bash", {{"command", "pwd"}}, dir_b.string());
    Expect(result.ok && result.text.find(dir_b.string()) != std::string::npos,
           "bash should run in per-request working_directory");

    std::filesystem::remove_all(parent);
}

}  // namespace

int main() {
    TestReadWriteEdit();
    TestBash();
    TestComposite();
    TestWorkingDirectoryOverride();

    if (g_failures != 0) {
        std::cerr << "local tools test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "local tools test passed\n";
    return 0;
}
