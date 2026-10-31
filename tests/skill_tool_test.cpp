#include "aiagent/tools/skill_tool.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "aiagent/config.hpp"

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

class TempDir {
public:
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("aiagent-skill-tool-" + std::to_string(stamp));
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

void WriteSkill(const fs::path& root, const std::string& directory, const std::string& content) {
    WriteFile(root / directory / "SKILL.md", content);
}

std::string SkillText(const std::string& name, const std::string& description,
                      const std::string& body = "# Steps\n\nRun the steps.\n") {
    return "---\nname: " + name + "\ndescription: " + description + "\n---\n\n" + body;
}

void TestToolSpec() {
    aiagent::tools::SkillToolProvider provider(nullptr);
    const auto tools = provider.ListTools();
    Expect(tools.size() == 1, "skill provider should expose one tool");
    if (tools.size() == 1) {
        Expect(tools[0].name == aiagent::tools::kSkillToolName, "tool name should be skill");
        Expect(tools[0].parameters.contains("properties") &&
                       tools[0].parameters["properties"].contains("name") &&
                       tools[0].parameters["properties"].contains("file"),
               "skill tool should accept name and optional file");
        Expect(tools[0].parameters["required"] == nlohmann::json::array({"name"}),
               "name should be the only required field");
    }
    Expect(aiagent::tools::MakeSkillToolProvider(nullptr) != nullptr,
           "factory should return a provider");
}

void TestExecuteBodyAndResource() {
    TempDir temp;
    const fs::path root = temp.path() / "skills";
    const fs::path skill = root / "alpha";
    WriteSkill(root, "alpha", SkillText("alpha", "alpha skill", "# Alpha\n\nBody text.\n"));
    WriteFile(skill / "reference.md", "reference body\n");
    WriteFile(skill / "scripts" / "run.sh", "echo hi\n");

    aiagent::SkillsConfig config;
    config.project = false;
    auto loader = std::make_shared<aiagent::skills::SkillLoader>(
            config, std::vector<std::string>{root.string()}, std::vector<std::string>{});
    aiagent::tools::SkillToolProvider provider(loader);

    aiagent::tools::ToolContext context;
    context.working_directory = temp.path().string();

    const nlohmann::json body_args = {{"name", "alpha"}};
    const auto body = provider.Execute("skill", body_args, context);
    Expect(body.ok, "skill body should load");
    Expect(body.text.find("Skill: alpha") != std::string::npos, "result should identify the skill");
    Expect(body.text.find("Source: user") != std::string::npos, "result should include the source");
    Expect(body.text.find("Files: reference.md, scripts/") != std::string::npos,
           "result should list resources in sorted order");
    Expect(body.text.find("# Alpha") != std::string::npos, "result should include the skill body");

    const nlohmann::json resource_args = {{"name", "alpha"}, {"file", "reference.md"}};
    const auto resource = provider.Execute("skill", resource_args, context);
    Expect(resource.ok, "skill resource should load");
    Expect(resource.text.find("File: reference.md") != std::string::npos,
           "resource result should identify the file");
    Expect(resource.text.find("reference body") != std::string::npos,
           "resource result should include the file body");
    Expect(resource.text.find("Files:") == std::string::npos,
           "resource result should not print the resource list twice");

    const nlohmann::json nested_args = {{"name", "alpha"}, {"file", "scripts/run.sh"}};
    const auto nested = provider.Execute("skill", nested_args, context);
    Expect(nested.ok && nested.text.find("echo hi") != std::string::npos,
           "nested resource should load");
}

void TestExecuteErrors() {
    TempDir temp;
    const fs::path root = temp.path() / "skills";
    WriteSkill(root, "alpha", SkillText("alpha", "alpha skill"));

    aiagent::SkillsConfig config;
    config.project = false;
    auto loader = std::make_shared<aiagent::skills::SkillLoader>(
            config, std::vector<std::string>{root.string()}, std::vector<std::string>{});
    aiagent::tools::SkillToolProvider provider(loader);

    aiagent::tools::ToolContext context;
    context.working_directory = temp.path().string();

    const auto unknown = provider.Execute("skill", {{"name", "missing"}}, context);
    Expect(!unknown.ok, "unknown skill should fail");
    Expect(unknown.error.find("available:") != std::string::npos,
           "unknown skill error should list available skills");

    const auto missing = provider.Execute("skill", nlohmann::json::object(), context);
    Expect(!missing.ok, "missing name should fail");

    const auto wrong_name = provider.Execute("skill", {{"name", 1}}, context);
    Expect(!wrong_name.ok, "non-string name should fail");

    const auto empty_name = provider.Execute("skill", {{"name", ""}}, context);
    Expect(!empty_name.ok, "empty name should fail");

    const auto wrong_file = provider.Execute("skill", {{"name", "alpha"}, {"file", 1}}, context);
    Expect(!wrong_file.ok, "non-string file should fail");

    const auto wrong_tool = provider.Execute("other", {{"name", "alpha"}}, context);
    Expect(!wrong_tool.ok, "unknown tool name should fail");

    aiagent::tools::SkillToolProvider no_loader(nullptr);
    const auto unavailable = no_loader.Execute("skill", {{"name", "alpha"}}, context);
    Expect(!unavailable.ok && unavailable.error.find("not available") != std::string::npos,
           "missing loader should fail explicitly");
}

void TestFallbackAndTruncation() {
    TempDir temp;
    const fs::path root = temp.path() / "skills";
    WriteSkill(root, "long", SkillText("long", "long skill", std::string(400, 'x')));

    aiagent::SkillsConfig config;
    config.project = false;
    config.max_skill_bytes = 80;
    auto loader = std::make_shared<aiagent::skills::SkillLoader>(
            config, std::vector<std::string>{root.string()}, std::vector<std::string>{});

    // context.working_directory 为空时使用 provider 的 fallback。
    aiagent::tools::SkillToolProvider provider(loader, temp.path().string());
    aiagent::tools::ToolContext context;
    const auto result = provider.Execute("skill", {{"name", "long"}}, context);
    Expect(result.ok, "fallback working directory should be used");
    Expect(result.text.find("truncated") != std::string::npos,
           "truncated skill body should carry a marker");

    // 显式 working_directory 优先于 fallback。
    aiagent::tools::SkillToolProvider other(loader, (temp.path() / "missing").string());
    context.working_directory = temp.path().string();
    const auto explicit_cwd = other.Execute("skill", {{"name", "long"}}, context);
    Expect(explicit_cwd.ok, "explicit working directory should override fallback");
}

}  // namespace

int main() {
    TestToolSpec();
    TestExecuteBodyAndResource();
    TestExecuteErrors();
    TestFallbackAndTruncation();

    if (g_failures == 0) {
        std::cout << "skill tool tests passed\n";
        return 0;
    }
    std::cerr << "skill tool tests failed with " << g_failures << " failure(s)\n";
    return 1;
}
