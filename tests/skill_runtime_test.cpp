#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "a2a/models/message.hpp"
#include "aiagent/agent/agent_runtime.hpp"
#include "aiagent/config.hpp"
#include "aiagent/skills/skill_loader.hpp"
#include "aiagent/tools/skill_tool.hpp"

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
        path_ = fs::temp_directory_path() / ("aiagent-skill-runtime-" + std::to_string(stamp));
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

void WriteSkill(const fs::path& root, const std::string& directory, const std::string& description,
                const std::string& body) {
    WriteFile(root / directory / "SKILL.md",
              "---\nname: " + directory + "\ndescription: " + description + "\n---\n\n" + body);
}

class CapturingModel final : public aiagent::llm::IModelClient {
public:
    std::string system_content;
    std::vector<aiagent::llm::ToolSpec> last_tools;
    int calls = 0;

    aiagent::llm::ChatResult Chat(const std::vector<aiagent::llm::ChatMessage>& messages,
                                  const std::vector<aiagent::llm::ToolSpec>& tools,
                                  const std::optional<std::string>&) override {
        ++calls;
        Capture(messages);
        last_tools = tools;
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "plain";
        choice.finish_reason = "stop";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(
            const std::vector<aiagent::llm::ChatMessage>& messages,
            const std::vector<aiagent::llm::ToolSpec>& tools,
            const std::function<void(const aiagent::llm::StreamEvent&)>& on_event) override {
        ++calls;
        Capture(messages);
        last_tools = tools;
        aiagent::llm::StreamEvent event;
        event.content = "plain";
        event.finish_reason = "stop";
        if (on_event) {
            on_event(event);
        }
    }

private:
    void Capture(const std::vector<aiagent::llm::ChatMessage>& messages) {
        for (const auto& message : messages) {
            if (message.role == "system") {
                system_content = message.content;
            }
        }
    }
};

class SkillCallingModel final : public aiagent::llm::IModelClient {
public:
    std::string system_content;
    std::string tool_result;
    int calls = 0;

    aiagent::llm::ChatResult Chat(const std::vector<aiagent::llm::ChatMessage>& messages,
                                  const std::vector<aiagent::llm::ToolSpec>&,
                                  const std::optional<std::string>&) override {
        ++calls;
        Capture(messages);
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        if (calls == 1) {
            choice.tool_calls.push_back(aiagent::llm::ToolCall{
                    "call-skill", aiagent::tools::kSkillToolName, R"({"name":"alpha"})"});
        } else {
            choice.content = "done";
            choice.finish_reason = "stop";
        }
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(
            const std::vector<aiagent::llm::ChatMessage>& messages,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::function<void(const aiagent::llm::StreamEvent&)>& on_event) override {
        ++calls;
        Capture(messages);
        aiagent::llm::StreamEvent event;
        if (calls == 1) {
            event.finish_reason = "tool_calls";
            event.tool_calls.push_back(aiagent::llm::ToolCall{
                    "call-skill", aiagent::tools::kSkillToolName, R"({"name":"alpha"})"});
        } else {
            event.content = "done";
            event.finish_reason = "stop";
        }
        if (on_event) {
            on_event(event);
        }
    }

private:
    void Capture(const std::vector<aiagent::llm::ChatMessage>& messages) {
        for (const auto& message : messages) {
            if (message.role == "system") {
                system_content = message.content;
            }
            if (message.role == "tool") {
                tool_result = message.content;
            }
        }
    }
};

class PlainToolProvider final : public aiagent::tools::IToolProvider {
public:
    std::vector<aiagent::llm::ToolSpec> ListTools() override {
        aiagent::llm::ToolSpec spec;
        spec.name = "ok_tool";
        return {spec};
    }

    aiagent::tools::ToolExecutionResult Execute(const std::string&, const nlohmann::json&,
                                                const aiagent::tools::ToolContext&) override {
        aiagent::tools::ToolExecutionResult result;
        result.ok = true;
        result.text = "ok";
        return result;
    }
};

std::shared_ptr<aiagent::skills::SkillLoader> MakeLoader(const fs::path& root) {
    aiagent::SkillsConfig config;
    config.project = false;
    return std::make_shared<aiagent::skills::SkillLoader>(
            config, std::vector<std::string>{root.string()}, std::vector<std::string>{});
}

bool HasSkillTool(const std::vector<aiagent::llm::ToolSpec>& tools) {
    for (const auto& tool : tools) {
        if (tool.name == aiagent::tools::kSkillToolName) {
            return true;
        }
    }
    return false;
}

void TestCatalogInjection() {
    TempDir temp;
    const fs::path root = temp.path() / "skills";
    WriteSkill(root, "alpha", "alpha skill", "# Alpha\n\nDO NOT SHOW IN CATALOG\n");
    auto loader = MakeLoader(root);

    auto model = std::make_shared<CapturingModel>();
    aiagent::agent::AgentRuntime runtime(model);
    runtime.SetSkillLoader(loader);
    runtime.SetToolProvider(aiagent::tools::MakeSkillToolProvider(loader));
    runtime.SetFallbackWorkingDirectory(temp.path().string());

    const auto user = a2a::models::MakeTextMessage(a2a::models::Role::kUser, "hello", "text/plain");
    runtime.HandleMessage(user, {user}, "ctx-catalog-sync");
    Expect(model->system_content.find("<skills>") != std::string::npos,
           "sync path should inject the skill catalog");
    Expect(model->system_content.find("- alpha: alpha skill") != std::string::npos,
           "catalog should include name and description");
    Expect(model->system_content.find("DO NOT SHOW IN CATALOG") == std::string::npos,
           "catalog must not include the skill body");
    Expect(HasSkillTool(model->last_tools),
           "sync path should expose the skill tool when the cwd has skills");

    auto stream_model = std::make_shared<CapturingModel>();
    aiagent::agent::AgentRuntime stream_runtime(stream_model);
    stream_runtime.SetSkillLoader(loader);
    stream_runtime.SetToolProvider(aiagent::tools::MakeSkillToolProvider(loader));
    stream_runtime.SetFallbackWorkingDirectory(temp.path().string());
    stream_runtime.HandleMessageStreamingEvents(user, {user}, "ctx-catalog-stream", nullptr,
                                                temp.path().string());
    Expect(stream_model->system_content.find("<skills>") != std::string::npos,
           "streaming path should inject the skill catalog");
    Expect(HasSkillTool(stream_model->last_tools),
           "streaming path should expose the skill tool when the cwd has skills");
}

void TestCatalogGuards() {
    TempDir temp;
    const fs::path root = temp.path() / "skills";
    WriteSkill(root, "alpha", "alpha skill", "# Alpha\n");
    auto loader = MakeLoader(root);
    const auto user = a2a::models::MakeTextMessage(a2a::models::Role::kUser, "hello", "text/plain");

    {
        auto model = std::make_shared<CapturingModel>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetSkillLoader(loader);
        runtime.SetToolProvider(std::make_shared<PlainToolProvider>());
        runtime.SetFallbackWorkingDirectory(temp.path().string());
        runtime.HandleMessage(user, {user}, "ctx-no-skill-tool");
        Expect(model->system_content.find("<skills>") == std::string::npos,
               "catalog must stay hidden when the tool list lacks skill");
        Expect(!HasSkillTool(model->last_tools),
               "skill tool must stay absent when the provider lacks it");
    }

    {
        auto model = std::make_shared<CapturingModel>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetToolProvider(aiagent::tools::MakeSkillToolProvider(nullptr));
        runtime.SetFallbackWorkingDirectory(temp.path().string());
        runtime.HandleMessage(user, {user}, "ctx-no-loader");
        Expect(model->system_content.find("<skills>") == std::string::npos,
               "catalog must stay hidden without a skill loader");
        Expect(!HasSkillTool(model->last_tools), "skill tool must stay absent without a loader");
    }

    {
        const fs::path empty = temp.path() / "empty-skills";
        fs::create_directories(empty);
        auto empty_loader = MakeLoader(empty);
        auto model = std::make_shared<CapturingModel>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetSkillLoader(empty_loader);
        runtime.SetToolProvider(aiagent::tools::MakeSkillToolProvider(empty_loader));
        runtime.SetFallbackWorkingDirectory(temp.path().string());
        runtime.HandleMessage(user, {user}, "ctx-empty-catalog");
        Expect(model->system_content.find("<skills>") == std::string::npos,
               "empty catalog should not add an empty block");
        Expect(!HasSkillTool(model->last_tools), "empty catalog should not expose the skill tool");
    }
}

void TestSkillToolExecution() {
    TempDir temp;
    const fs::path root = temp.path() / "skills";
    WriteSkill(root, "alpha", "alpha skill", "# Alpha\n\nBody text from skill.\n");
    auto loader = MakeLoader(root);
    const auto user =
            a2a::models::MakeTextMessage(a2a::models::Role::kUser, "use alpha", "text/plain");

    {
        auto model = std::make_shared<SkillCallingModel>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetSkillLoader(loader);
        runtime.SetToolProvider(
                aiagent::tools::MakeSkillToolProvider(loader, temp.path().string()));
        runtime.SetFallbackWorkingDirectory(temp.path().string());
        runtime.SetApprovalHandler([](const aiagent::agent::ApprovalRequest&) {
            return aiagent::agent::ApprovalDecision::kDeny;
        });
        runtime.HandleMessage(user, {user}, "ctx-skill-sync");
        Expect(model->calls == 2, "sync run should call the model twice");
        Expect(model->tool_result.find("Skill: alpha") != std::string::npos,
               "sync run should return the skill header");
        Expect(model->tool_result.find("Body text from skill.") != std::string::npos,
               "sync run should return the skill body");
        Expect(model->system_content.find("<skills>") != std::string::npos,
               "sync run should expose the catalog to the model");
    }

    {
        auto model = std::make_shared<SkillCallingModel>();
        aiagent::agent::AgentRuntime runtime(model);
        runtime.SetSkillLoader(loader);
        runtime.SetToolProvider(
                aiagent::tools::MakeSkillToolProvider(loader, temp.path().string()));
        runtime.SetFallbackWorkingDirectory(temp.path().string());
        int approvals = 0;
        runtime.SetApprovalHandler([&approvals](const aiagent::agent::ApprovalRequest&) {
            ++approvals;
            return aiagent::agent::ApprovalDecision::kDeny;
        });
        runtime.HandleMessageStreamingEvents(user, {user}, "ctx-skill-stream", nullptr,
                                             temp.path().string());
        Expect(model->calls == 2, "streaming run should call the model twice");
        Expect(model->tool_result.find("Body text from skill.") != std::string::npos,
               "streaming run should return the skill body");
        Expect(approvals == 0, "skill is read-only and must not request approval");
    }
}

}  // namespace

int main() {
    TestCatalogInjection();
    TestCatalogGuards();
    TestSkillToolExecution();

    if (g_failures == 0) {
        std::cout << "skill runtime tests passed\n";
        return 0;
    }
    std::cerr << "skill runtime tests failed with " << g_failures << " failure(s)\n";
    return 1;
}
