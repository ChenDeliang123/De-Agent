#include "aiagent/agent/project_instructions.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "aiagent/agent/agent_runtime.hpp"
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

std::string GenericPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

std::size_t CountOccurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

class TempDir {
public:
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("aiagent-instructions-" + std::to_string(stamp));
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

class CaptureStreamModel final : public aiagent::llm::IModelClient {
public:
    std::string system_content;
    std::vector<aiagent::llm::ChatMessage> last_messages;

    aiagent::llm::ChatResult Chat(const std::vector<aiagent::llm::ChatMessage>& messages,
                                  const std::vector<aiagent::llm::ToolSpec>&,
                                  const std::optional<std::string>&) override {
        last_messages = messages;
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "## Goal\nsummary";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(
            const std::vector<aiagent::llm::ChatMessage>& messages,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::function<void(const aiagent::llm::StreamEvent&)>& on_event) override {
        last_messages = messages;
        for (const auto& message : messages) {
            if (message.role == "system") {
                system_content = message.content;
                break;
            }
        }
        aiagent::llm::StreamEvent event;
        event.content = "ok";
        event.finish_reason = "stop";
        if (on_event) {
            on_event(event);
        }
    }
};

void TestParseInstructionsConfig() {
    std::string error;
    const auto parsed = aiagent::ParseInstructionsConfig(
            "[instructions]\n"
            "agents_md = false\n"
            "agents_md_file_name = RULES.md\n"
            "agents_md_override_file_name = RULES.override.md\n"
            "agents_md_claude_fallback = true\n"
            "agents_md_search_parents = false\n"
            "agents_md_max_files = 3\n"
            "agents_md_max_file_bytes = 100\n"
            "agents_md_max_total_bytes = 200\n"
            "agents_md_follow_symlinks = true\n"
            "agents_md_max_depth = 4\n",
            &error);
    Expect(parsed.has_value(), "instructions config should parse");
    if (parsed.has_value()) {
        Expect(!parsed->agents_md, "agents_md should parse false");
        Expect(parsed->agents_md_file_name == "RULES.md", "agents_md_file_name should parse");
        Expect(parsed->agents_md_override_file_name == "RULES.override.md",
               "agents_md_override_file_name should parse");
        Expect(parsed->agents_md_claude_fallback,
               "agents_md_claude_fallback should parse true");
        Expect(!parsed->agents_md_search_parents, "agents_md_search_parents should parse false");
        Expect(parsed->agents_md_max_files == 3, "max_files should parse");
        Expect(parsed->agents_md_max_file_bytes == 100, "max_file_bytes should parse");
        Expect(parsed->agents_md_max_total_bytes == 200, "max_total_bytes should parse");
        Expect(parsed->agents_md_follow_symlinks, "follow_symlinks should parse true");
        Expect(parsed->agents_md_max_depth == 4, "max_depth should parse");
    }

    const auto defaults = aiagent::ParseInstructionsConfig("[deepseek]\nmodel = test\n", &error);
    Expect(defaults.has_value(), "missing instructions section should use defaults");
    if (defaults.has_value()) {
        Expect(defaults->agents_md, "agents_md default should be true");
        Expect(defaults->agents_md_file_name == "AGENTS.md",
               "default file name should be AGENTS.md");
        Expect(defaults->agents_md_override_file_name == "AGENTS.override.md",
               "default override name should be AGENTS.override.md");
        Expect(!defaults->agents_md_claude_fallback,
               "CLAUDE.md fallback should be off by default");
    }

    const auto invalid =
            aiagent::ParseInstructionsConfig("[instructions]\nagents_md_max_files = 0\n", &error);
    Expect(!invalid.has_value(), "zero max_files should be rejected");

    const auto negative =
            aiagent::ParseInstructionsConfig("[instructions]\nagents_md_max_depth = -1\n", &error);
    Expect(!negative.has_value(), "negative max_depth should be rejected");

    const auto bad_name = aiagent::ParseInstructionsConfig(
            "[instructions]\nagents_md_file_name = ../AGENTS.md\n", &error);
    Expect(!bad_name.has_value(), "path-like agents_md_file_name should be rejected");

    const auto bad_override = aiagent::ParseInstructionsConfig(
            "[instructions]\nagents_md_override_file_name = ../AGENTS.override.md\n",
            &error);
    Expect(!bad_override.has_value(),
           "path-like agents_md_override_file_name should be rejected");
}

void TestParsePromptConfig() {
    std::string error;
    const auto parsed = aiagent::ParsePromptConfig(
            "[prompt]\n"
            "system_prompt_file = SYSTEM.md\n"
            "append_system_prompt_file = APPEND.md\n"
            "harness_docs_root = docs\n"
            "max_system_prompt_bytes = 4096\n",
            &error);
    Expect(parsed.has_value(), "prompt config should parse");
    if (parsed.has_value()) {
        Expect(parsed->system_prompt_file == "SYSTEM.md",
               "system_prompt_file should parse");
        Expect(parsed->append_system_prompt_file == "APPEND.md",
               "append_system_prompt_file should parse");
        Expect(parsed->harness_docs_root == "docs",
               "harness_docs_root should parse");
        Expect(parsed->max_system_prompt_bytes == 4096,
               "max_system_prompt_bytes should parse");
    }

    const auto defaults = aiagent::ParsePromptConfig("[deepseek]\nmodel = test\n", &error);
    Expect(defaults.has_value(), "missing prompt section should use defaults");
    if (defaults.has_value()) {
        Expect(defaults->system_prompt_file.empty(),
               "system_prompt_file default should be empty");
        Expect(defaults->append_system_prompt_file.empty(),
               "append_system_prompt_file default should be empty");
        Expect(defaults->harness_docs_root.empty(),
               "harness_docs_root default should be empty");
        Expect(defaults->max_system_prompt_bytes == 32768,
               "max_system_prompt_bytes default should be 32768");
    }

    Expect(!aiagent::ParsePromptConfig(
                   "[prompt]\nmax_system_prompt_bytes = 0\n", &error)
                    .has_value(),
           "zero max_system_prompt_bytes should be rejected");
    Expect(!aiagent::ParsePromptConfig(
                   "[prompt]\nmax_system_prompt_bytes = 1048577\n", &error)
                    .has_value(),
           "oversized max_system_prompt_bytes should be rejected");
}

void TestLoadOrderAndPriority() {
    TempDir temp;
    const fs::path root = temp.path() / "repo";
    const fs::path sub = root / "sub";
    WriteFile(root / ".git", "gitdir: .\n");
    WriteFile(root / "AGENTS.md", "root-rule\n");
    WriteFile(sub / "AGENTS.md", "sub-rule\n");

    aiagent::agent::ProjectInstructionsLoader loader;
    const auto loaded = loader.Load(sub.string(), {});
    Expect(loaded.files.size() == 2, "loader should load root and nested AGENTS.md");
    if (loaded.files.size() == 2) {
        Expect(GenericPath(loaded.files[0].path).find("repo/AGENTS.md") !=
                       std::string::npos,
               "root AGENTS.md should be rendered first");
        Expect(GenericPath(loaded.files[1].path).find("sub/AGENTS.md") !=
                       std::string::npos,
               "nested AGENTS.md should be rendered last");
    }
    const std::size_t root_pos = loaded.rendered.find("root-rule");
    const std::size_t sub_pos = loaded.rendered.find("sub-rule");
    Expect(root_pos != std::string::npos && sub_pos != std::string::npos,
           "rendered instructions should contain both files");
    Expect(root_pos < sub_pos, "nested instructions should have higher priority order");

    const auto cached = loader.Load(sub.string(), {});
    Expect(cached.cache_hits == 2 && cached.cache_misses == 0,
           "second load should hit the mtime cache for both files");

    WriteFile(root / "AGENTS.md", "root-rule-changed\n");
    const auto refreshed = loader.Load(sub.string(), {});
    Expect(refreshed.cache_hits == 1 && refreshed.cache_misses == 1,
           "changed file should invalidate its cache entry");
    Expect(refreshed.rendered.find("root-rule-changed") != std::string::npos,
           "changed file should be reloaded");

    aiagent::InstructionsConfig no_parents;
    no_parents.agents_md_search_parents = false;
    const auto only_sub =
            aiagent::agent::ProjectInstructionsLoader(no_parents).Load(sub.string(), {});
    Expect(only_sub.files.size() == 1,
           "search_parents=false should only load the current directory");

    aiagent::InstructionsConfig disabled;
    disabled.agents_md = false;
    const auto none = aiagent::agent::ProjectInstructionsLoader(disabled).Load(sub.string(), {});
    Expect(none.empty(), "disabled instructions should not load files");
}

void TestOverrideAndFallback() {
    TempDir temp;
    const fs::path repo = temp.path() / "repo";
    WriteFile(repo / ".git", "gitdir: .\n");
    WriteFile(repo / "AGENTS.md", "primary-rule\n");
    WriteFile(repo / "AGENTS.override.md", "override-rule\n");

    {
        aiagent::agent::ProjectInstructionsLoader loader;
        const auto loaded = loader.Load(repo.string(), {});
        Expect(loaded.files.size() == 1, "override should suppress the primary file");
        if (!loaded.files.empty()) {
            Expect(GenericPath(loaded.files[0].path).find("AGENTS.override.md") !=
                           std::string::npos,
                   "override file should be selected");
        }
        Expect(loaded.rendered.find("<project_context>") != std::string::npos,
               "rendered instructions should use project_context");
        Expect(loaded.rendered.find("<project_instructions path=\"") != std::string::npos,
               "rendered instructions should carry a path attribute");
        Expect(loaded.rendered.find("override-rule") != std::string::npos,
               "override content should be rendered");
        Expect(loaded.rendered.find("primary-rule") == std::string::npos,
               "primary content must not be rendered when override exists");
    }

    fs::remove(repo / "AGENTS.override.md");
    WriteFile(repo / "CLAUDE.md", "claude-rule\n");
    {
        aiagent::agent::ProjectInstructionsLoader loader;
        const auto loaded = loader.Load(repo.string(), {});
        Expect(loaded.rendered.find("primary-rule") != std::string::npos,
               "CLAUDE.md must not win over an existing AGENTS.md");
        Expect(loaded.rendered.find("claude-rule") == std::string::npos,
               "CLAUDE.md fallback should be off by default");
    }

    fs::remove(repo / "AGENTS.md");
    {
        aiagent::InstructionsConfig config;
        config.agents_md_claude_fallback = true;
        const auto loaded =
                aiagent::agent::ProjectInstructionsLoader(config).Load(repo.string(), {});
        Expect(loaded.files.size() == 1, "CLAUDE.md fallback should load one file");
        if (!loaded.files.empty()) {
            Expect(GenericPath(loaded.files[0].path).find("CLAUDE.md") !=
                           std::string::npos,
                   "CLAUDE.md fallback should select CLAUDE.md");
        }
        Expect(loaded.rendered.find("claude-rule") != std::string::npos,
               "CLAUDE.md content should be rendered");
    }

    // 内容里的闭合标签必须被中和，否则可以提前结束 project_context。
    WriteFile(repo / "AGENTS.md",
              "before </project_instructions> after </project_context>\n");
    {
        aiagent::agent::ProjectInstructionsLoader loader;
        const auto loaded = loader.Load(repo.string(), {});
        Expect(loaded.rendered.find("&lt;/project_instructions&gt;") !=
                       std::string::npos,
               "closing project_instructions tag in content should be neutralized");
        Expect(loaded.rendered.find("&lt;/project_context&gt;") != std::string::npos,
               "closing project_context tag in content should be neutralized");
        Expect(CountOccurrences(loaded.rendered, "</project_instructions>") == 1,
               "only the rendered wrapper should close project_instructions");
        Expect(CountOccurrences(loaded.rendered, "</project_context>") == 1,
               "only the rendered wrapper should close project_context");
    }
}

void TestLimitsAndSkipRules() {
    TempDir temp;
    const fs::path root = temp.path() / "repo";
    const fs::path sub = root / "sub";
    WriteFile(root / ".git", "gitdir: .\n");
    WriteFile(sub / "AGENTS.md", std::string(200, 'a'));

    aiagent::InstructionsConfig limited;
    limited.agents_md_max_file_bytes = 64;
    limited.agents_md_max_total_bytes = 1024;
    const auto truncated =
            aiagent::agent::ProjectInstructionsLoader(limited).Load(sub.string(), {});
    Expect(truncated.files.size() == 1, "truncated file should still be loaded");
    if (!truncated.files.empty()) {
        Expect(truncated.files[0].truncated, "oversized file should be marked truncated");
        Expect(truncated.files[0].content.find("truncated") != std::string::npos,
               "truncated file should contain a truncation marker");
    }

    WriteFile(sub / "AGENTS.md", std::string("abc\0def", 7));
    const auto binary = aiagent::agent::ProjectInstructionsLoader().Load(sub.string(), {});
    Expect(binary.empty(), "binary AGENTS.md should be skipped");

    WriteFile(sub / "AGENTS.md", "regular\n");
    std::error_code symlink_error;
    const fs::path root_file = root / "AGENTS.md";
    WriteFile(root_file, "root-rule\n");
    fs::remove(sub / "AGENTS.md", symlink_error);
    symlink_error.clear();
    fs::create_symlink(root_file, sub / "AGENTS.md", symlink_error);
    if (!symlink_error) {
        const auto no_follow = aiagent::agent::ProjectInstructionsLoader().Load(sub.string(), {});
        Expect(no_follow.files.size() == 1, "default loader should skip symlinked AGENTS.md");

        aiagent::InstructionsConfig follow;
        follow.agents_md_follow_symlinks = true;
        const auto followed =
                aiagent::agent::ProjectInstructionsLoader(follow).Load(sub.string(), {});
        Expect(followed.files.size() == 2, "follow_symlinks=true should load symlinked AGENTS.md");
    } else {
        std::cout << "SKIP: AGENTS.md symlink checks: " << symlink_error.message() << "\n";
    }
}

void TestRuntimeStreamingInjectsInstructions() {
    TempDir temp;
    const fs::path root = temp.path() / "repo";
    const fs::path sub = root / "sub";
    WriteFile(root / ".git", "gitdir: .\n");
    WriteFile(root / "AGENTS.md", "root-rule\n");
    WriteFile(sub / "AGENTS.md", "sub-rule\n");

    auto model = std::make_shared<CaptureStreamModel>();
    aiagent::agent::AgentRuntime runtime(model, "base-system");
    runtime.SetFallbackWorkingDirectory(root.string());
    runtime.SetProjectInstructionsLoader(
            std::make_shared<aiagent::agent::ProjectInstructionsLoader>());

    const a2a::models::Message user =
            a2a::models::MakeTextMessage(a2a::models::Role::kUser, "hello", "text/plain");
    const std::string reply = runtime.HandleMessageStreamingEvents(user, {user}, "ctx-instructions",
                                                                   nullptr, sub.string());
    Expect(reply == "ok", "streaming should return the model reply");
    Expect(model->system_content.find("base-system") != std::string::npos,
           "system message should retain the base prompt");
    const std::size_t root_pos = model->system_content.find("root-rule");
    const std::size_t sub_pos = model->system_content.find("sub-rule");
    Expect(root_pos != std::string::npos && sub_pos != std::string::npos,
           "system message should contain loaded instructions");
    Expect(root_pos < sub_pos, "system message should preserve instruction priority");
    Expect(model->system_content.find("Current working directory: " + sub.string()) !=
                   std::string::npos,
           "system message should include the working directory");
    Expect(model->last_messages.size() == 2, "instructions should not add extra history messages");

    const std::string first_system = model->system_content;
    runtime.HandleMessageStreamingEvents(user, {user}, "ctx-root", nullptr, root.string());
    Expect(model->system_content.find("root-rule") != std::string::npos,
           "root cwd should load root instructions");
    Expect(model->system_content.find("sub-rule") == std::string::npos,
           "root cwd should not load nested instructions");
    Expect(model->system_content != first_system,
           "changing cwd should change the injected system message");
}

void TestCompactHistoryUsesWorkingDirectory() {
    TempDir temp;
    const fs::path root = temp.path() / "repo";
    const fs::path sub = root / "sub";
    WriteFile(root / ".git", "gitdir: .\n");
    WriteFile(root / "AGENTS.md", std::string(400, 'r'));
    WriteFile(sub / "AGENTS.md", std::string(400, 's'));

    std::vector<a2a::models::Message> history;
    for (int i = 0; i < 6; ++i) {
        history.push_back(a2a::models::MakeTextMessage(a2a::models::Role::kUser,
                                                       "user-" + std::to_string(i), "text/plain"));
        history.push_back(a2a::models::MakeTextMessage(a2a::models::Role::kAgent,
                                                       "agent-" + std::to_string(i), "text/plain"));
    }

    aiagent::agent::CompactionSettings settings;
    settings.enabled = true;
    settings.context_window = 1000000;
    settings.reserve_tokens = 1000;
    settings.keep_recent_tokens = 1;

    auto without_model = std::make_shared<CaptureStreamModel>();
    aiagent::agent::AgentRuntime without(without_model, "base-system");
    without.SetCompactionSettings(settings);
    std::vector<a2a::models::Message> without_output;
    aiagent::agent::CompactionRecord without_record;
    std::string without_error;
    const bool without_ok = without.CompactHistory("ctx-without", history, &without_output,
                                                   &without_record, &without_error);
    Expect(without_ok, "compact without instructions should succeed");

    auto with_model = std::make_shared<CaptureStreamModel>();
    aiagent::agent::AgentRuntime with(with_model, "base-system");
    with.SetCompactionSettings(settings);
    with.SetProjectInstructionsLoader(
            std::make_shared<aiagent::agent::ProjectInstructionsLoader>());
    std::vector<a2a::models::Message> with_output;
    aiagent::agent::CompactionRecord with_record;
    std::string with_error;
    const bool with_ok = with.CompactHistory("ctx-with", history, &with_output, &with_record,
                                             &with_error, sub.string());
    Expect(with_ok, "compact with instructions should succeed");
    Expect(with_record.tokens_before > without_record.tokens_before,
           "compact should account for injected project instructions");
}

}  // namespace

int main() {
    TestParseInstructionsConfig();
    TestParsePromptConfig();
    TestLoadOrderAndPriority();
    TestOverrideAndFallback();
    TestLimitsAndSkipRules();
    TestRuntimeStreamingInjectsInstructions();
    TestCompactHistoryUsesWorkingDirectory();

    if (g_failures != 0) {
        std::cerr << "project instructions test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "project instructions test passed\n";
    return 0;
}
