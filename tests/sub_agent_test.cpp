// SubAgentManager / task 工具 / FilteredToolProvider / LoadAgentDefinitions 单元测试。
// 全部使用假模型与假工具，确定性：并发闸门用可控 sleep 断言上限，不依赖网络。

#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "a2a/models/message.hpp"

#include "aiagent/agent/agent_event.hpp"
#include "aiagent/agent/agent_runtime.hpp"
#include "aiagent/agent/sub_agent.hpp"
#include "aiagent/config.hpp"
#include "aiagent/llm/deepseek_client.hpp"
#include "aiagent/tools/composite_tool_provider.hpp"
#include "aiagent/tools/filtered_tool_provider.hpp"
#include "aiagent/tools/sub_agent_tool.hpp"
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
                      ("aiagent_sub_agent_" + std::to_string(::getpid()));
    std::filesystem::create_directories(path);
    return path;
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

// ---------------------------------------------------------------------------
// 假模型与记录器
// ---------------------------------------------------------------------------

struct Harness {
    std::mutex mutex;
    std::vector<std::vector<aiagent::llm::ChatMessage>> calls;
    std::atomic<int> active{0};
    std::atomic<int> max_active{0};
    std::atomic<int> streams{0};

    void Enter() {
        const int now = active.fetch_add(1) + 1;
        int expected = max_active.load();
        while (now > expected && !max_active.compare_exchange_weak(expected, now)) {
        }
    }
    void Leave() { active.fetch_sub(1); }

    std::vector<std::vector<aiagent::llm::ChatMessage>> Calls() {
        std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }
};

aiagent::llm::ChatResult EmptyChatResult() {
    aiagent::llm::ChatResult result;
    aiagent::llm::ChatChoice choice;
    choice.role = "assistant";
    choice.finish_reason = "stop";
    result.choices.push_back(choice);
    return result;
}

void EmitText(const std::function<void(const aiagent::llm::StreamEvent&)>& on_event,
              const std::string& text) {
    aiagent::llm::StreamEvent event;
    event.content = text;
    event.finish_reason = "stop";
    on_event(event);
}

// 子 Agent 专用：返回固定报告（可带延迟）。
class ChildModel final : public aiagent::llm::IModelClient {
public:
    ChildModel(std::shared_ptr<Harness> harness, std::string report, int delay_ms = 0)
        : harness_(std::move(harness)), report_(std::move(report)), delay_ms_(delay_ms) {}

    aiagent::llm::ChatResult Chat(const std::vector<aiagent::llm::ChatMessage>&,
                                  const std::vector<aiagent::llm::ToolSpec>&,
                                  const std::optional<std::string>&) override {
        return EmptyChatResult();
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>& messages,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        {
            std::lock_guard<std::mutex> lock(harness_->mutex);
            harness_->calls.push_back(messages);
        }
        harness_->streams.fetch_add(1);
        harness_->Enter();
        if (delay_ms_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        }
        harness_->Leave();
        EmitText(on_event, report_);
    }

private:
    std::shared_ptr<Harness> harness_;
    std::string report_;
    int delay_ms_ = 0;
};

// 子 Agent 专用：每轮都发 noop 工具调用（用于触发运行时的取消/超时检查）。
class LoopingChildModel final : public aiagent::llm::IModelClient {
public:
    LoopingChildModel(std::shared_ptr<Harness> harness, int delay_ms)
        : harness_(std::move(harness)), delay_ms_(delay_ms) {}

    aiagent::llm::ChatResult Chat(const std::vector<aiagent::llm::ChatMessage>&,
                                  const std::vector<aiagent::llm::ToolSpec>&,
                                  const std::optional<std::string>&) override {
        return EmptyChatResult();
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        harness_->Enter();
        if (delay_ms_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        }
        harness_->Leave();
        aiagent::llm::StreamEvent event;
        aiagent::llm::ToolCall call;
        call.id = "child-tool-" + std::to_string(++counter_);
        call.name = "noop";
        call.arguments = "{}";
        event.tool_calls.push_back(call);
        event.finish_reason = "tool_calls";
        on_event(event);
    }

private:
    std::shared_ptr<Harness> harness_;
    int delay_ms_ = 0;
    std::atomic<int> counter_{0};
};

// 子 Agent 专用：第一轮发一次 noop 工具调用，随后返回报告（用于事件归属断言）。
class OneToolCallChildModel final : public aiagent::llm::IModelClient {
public:
    OneToolCallChildModel(std::shared_ptr<Harness> harness, std::string report)
        : harness_(std::move(harness)), report_(std::move(report)) {}

    aiagent::llm::ChatResult Chat(const std::vector<aiagent::llm::ChatMessage>&,
                                  const std::vector<aiagent::llm::ToolSpec>&,
                                  const std::optional<std::string>&) override {
        return EmptyChatResult();
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>& messages,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        {
            std::lock_guard<std::mutex> lock(harness_->mutex);
            harness_->calls.push_back(messages);
        }
        for (const auto& message : messages) {
            if (message.role == "tool") {
                EmitText(on_event, report_);
                return;
            }
        }
        aiagent::llm::StreamEvent event;
        aiagent::llm::ToolCall call;
        call.id = "c1";
        call.name = "noop";
        call.arguments = "{}";
        event.tool_calls.push_back(call);
        event.finish_reason = "tool_calls";
        on_event(event);
    }

private:
    std::shared_ptr<Harness> harness_;
    std::string report_;
};

// 父 Agent 专用：第一轮发 task 工具调用，第二轮把工具结果回显为最终答案。
class ParentModel final : public aiagent::llm::IModelClient {
public:
    ParentModel(std::shared_ptr<Harness> harness, std::string agent_type, std::string prompt)
        : harness_(std::move(harness)),
          agent_type_(std::move(agent_type)),
          prompt_(std::move(prompt)) {}

    aiagent::llm::ChatResult Chat(const std::vector<aiagent::llm::ChatMessage>&,
                                  const std::vector<aiagent::llm::ToolSpec>&,
                                  const std::optional<std::string>&) override {
        return EmptyChatResult();
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>& messages,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>& on_event)
            override {
        {
            std::lock_guard<std::mutex> lock(harness_->mutex);
            harness_->calls.push_back(messages);
        }
        for (const auto& message : messages) {
            if (message.role != "tool") {
                continue;
            }
            EmitText(on_event, "final: " + message.content);
            return;
        }
        aiagent::llm::StreamEvent event;
        aiagent::llm::ToolCall call;
        call.id = "call_parent";
        call.name = aiagent::tools::kTaskToolName;
        call.arguments =
                nlohmann::json{{"type", agent_type_}, {"prompt", prompt_}}.dump();
        event.tool_calls.push_back(call);
        event.finish_reason = "tool_calls";
        on_event(event);
    }

private:
    std::shared_ptr<Harness> harness_;
    std::string agent_type_;
    std::string prompt_;
};

// ---------------------------------------------------------------------------
// 假工具
// ---------------------------------------------------------------------------

class NoopToolProvider final : public aiagent::tools::IToolProvider {
public:
    std::vector<aiagent::llm::ToolSpec> ListTools() override {
        aiagent::llm::ToolSpec spec;
        spec.name = "noop";
        spec.description = "no-op test tool";
        spec.parameters = nlohmann::json::object();
        return {spec};
    }

    aiagent::tools::ToolExecutionResult Execute(const std::string& name,
                                                const nlohmann::json&,
                                                const aiagent::tools::ToolContext&) override {
        aiagent::tools::ToolExecutionResult result;
        result.ok = (name == "noop");
        result.text = "noop-result";
        if (!result.ok) {
            result.error = "unknown tool";
        }
        return result;
    }
};

// ---------------------------------------------------------------------------
// 夹具
// ---------------------------------------------------------------------------

aiagent::AgentsConfig Settings(int max_depth = 1, int max_concurrent = 3,
                               long default_timeout_s = 5, int max_rounds = 6,
                               std::size_t result_max_chars = 8000,
                               bool stream_events = true) {
    aiagent::AgentsConfig config;
    config.enabled = true;
    config.max_depth = max_depth;
    config.max_concurrent = max_concurrent;
    config.default_timeout_s = default_timeout_s;
    config.max_rounds = max_rounds;
    config.result_max_chars = result_max_chars;
    config.stream_events = stream_events;
    return config;
}

std::vector<aiagent::agent::AgentDefinition> Definitions() {
    return {
            aiagent::agent::AgentDefinition{"general", "general", "GENERAL PROMPT", "", {}},
            aiagent::agent::AgentDefinition{"explorer", "explorer", "EXPLORER PROMPT", "",
                                            {"noop"}},
    };
}

struct CapturedSpecs {
    std::mutex mutex;
    std::vector<aiagent::agent::SubAgentManager::ChildRuntimeSpec> specs;
    std::vector<std::vector<std::string>> child_tool_names;
};

aiagent::agent::SubAgentManager::ChildRuntimeFactory MakeFactory(
        const std::shared_ptr<CapturedSpecs>& captured,
        const std::function<std::shared_ptr<aiagent::llm::IModelClient>(
                const aiagent::agent::SubAgentManager::ChildRuntimeSpec&)>& make_model) {
    return [captured, make_model](
                   const aiagent::agent::SubAgentManager::ChildRuntimeSpec& spec)
                   -> std::shared_ptr<aiagent::agent::AgentRuntime> {
        {
            std::lock_guard<std::mutex> lock(captured->mutex);
            std::vector<std::string> names;
            if (spec.tools) {
                for (const auto& tool : spec.tools->ListTools()) {
                    names.push_back(tool.name);
                }
            }
            captured->specs.push_back(spec);
            captured->child_tool_names.push_back(names);
        }
        auto runtime =
                std::make_shared<aiagent::agent::AgentRuntime>(make_model(spec), spec.system_prompt);
        runtime->SetToolProvider(spec.tools);
        runtime->SetMaxToolRounds(spec.max_tool_rounds);
        runtime->SetFallbackWorkingDirectory("/tmp");
        aiagent::agent::CompactionSettings compaction;
        compaction.enabled = false;
        runtime->SetCompactionSettings(compaction);
        return runtime;
    };
}

std::shared_ptr<aiagent::tools::IToolProvider> ParentTools() {
    auto composite = std::make_shared<aiagent::tools::CompositeToolProvider>();
    composite->Add(std::make_shared<NoopToolProvider>());
    return composite;
}

// ---------------------------------------------------------------------------
// 测试
// ---------------------------------------------------------------------------

void TestFilteredToolProvider() {
    auto inner = ParentTools();
    aiagent::tools::FilteredToolProvider allowlist(inner, {"noop"});
    const auto tools = allowlist.ListTools();
    Expect(tools.size() == 1 && tools[0].name == "noop",
           "filtered provider should expose only allowlisted tools");
    Expect(allowlist.Allows("noop"), "allowlisted tool should be allowed");
    Expect(!allowlist.Allows("bash"), "non-allowlisted tool should be rejected");

    aiagent::tools::ToolContext context;
    const auto executed = allowlist.Execute("bash", nlohmann::json::object(), context);
    Expect(!executed.ok && executed.error.find("not available") != std::string::npos,
           "filtered provider should reject execution of filtered tools");

    aiagent::tools::FilteredToolProvider deny_task(inner, {}, {"noop"});
    Expect(deny_task.ListTools().empty(), "deny list should win over inherit-all");
}

void TestDefinitionLoading() {
    const auto dir = TempDir();
    std::filesystem::create_directories(dir / "prompts");
    WriteFile(dir / "prompts" / "general.md", "GENERAL FROM FILE\n");
    WriteFile(dir / "prompts" / "explorer.md", "EXPLORER FROM FILE\n");
    WriteFile(dir / "prompts" / "reviewer.md", "REVIEWER FROM FILE\n");

    aiagent::AgentsConfig config;
    aiagent::AgentDefinitionConfig custom;
    custom.name = "reviewer";
    custom.description = "reviews changes";
    custom.system_prompt_file = "reviewer.md";
    custom.tools = {"read"};
    custom.model = "deepseek-v4-flash";
    config.agents.push_back(custom);

    std::string error;
    const auto definitions = aiagent::agent::LoadAgentDefinitions(
            config, {dir.string() + "/prompts"}, &error);
    Expect(definitions.has_value(), "definitions should load from prompt dirs");
    if (definitions.has_value()) {
        Expect(definitions->definitions.size() == 3,
               "builtins plus custom definition should be present");
        Expect(definitions->skipped_builtins.empty(),
               "no builtin should be skipped when all prompts exist");
        const auto& general = definitions->definitions[0];
        const auto& explorer = definitions->definitions[1];
        const auto& reviewer = definitions->definitions[2];
        Expect(general.name == "general" && general.tools.empty(),
               "general should inherit the parent tool set");
        Expect(general.system_prompt.find("GENERAL FROM FILE") != std::string::npos,
               "general prompt should come from prompts/general.md");
        Expect(explorer.name == "explorer" && explorer.tools.size() == 4 &&
                       explorer.tools[0] == "read",
               "explorer should keep the read-only allowlist");
        Expect(reviewer.name == "reviewer" && reviewer.model == "deepseek-v4-flash" &&
                       reviewer.system_prompt.find("REVIEWER FROM FILE") != std::string::npos,
               "custom definition should be loaded with its model and prompt");
    }

    // 用户显式配置的定义缺提示词 → 硬错误（不静默降级）。
    aiagent::AgentsConfig missing = config;
    missing.agents[0].system_prompt_file = "missing.md";
    error.clear();
    const auto failed =
            aiagent::agent::LoadAgentDefinitions(missing, {dir.string() + "/prompts"}, &error);
    Expect(!failed.has_value() && error.find("missing.md") != std::string::npos,
           "missing prompt file for a configured definition should fail loudly");

    // 内置定义缺提示词（安装不完整）→ 跳过该定义并记录，由调用方降级。
    const auto bare = TempDir() / "bare";
    std::filesystem::create_directories(bare);
    error.clear();
    const auto degraded = aiagent::agent::LoadAgentDefinitions({}, {bare.string()}, &error);
    Expect(degraded.has_value() && degraded->definitions.empty(),
           "builtins without prompt files should be skipped instead of failing");
    Expect(degraded.has_value() && degraded->skipped_builtins.size() == 2,
           "skipped builtins should be reported to the caller");
    Expect(error.empty(), "skipping builtins should not be reported as a config error");

    std::filesystem::remove_all(dir);
}

void TestManagerRun() {
    auto harness = std::make_shared<Harness>();
    auto captured = std::make_shared<CapturedSpecs>();
    auto manager = std::make_shared<aiagent::agent::SubAgentManager>(
            Settings(), Definitions(),
            MakeFactory(captured, [harness](const auto&) -> std::shared_ptr<aiagent::llm::IModelClient> {
                return std::make_shared<ChildModel>(harness, "REPORT-CONTENT");
            }),
            ParentTools());

    aiagent::agent::SubAgentRequest request;
    request.agent_type = "explorer";
    request.prompt = "explore the widget";
    request.parent_session_id = "sess-1";
    const aiagent::agent::SubAgentResult result = manager->Run(request, 0);

    Expect(result.ok, "run should succeed");
    Expect(result.text == "REPORT-CONTENT", "result text should be the child report");
    Expect(result.agent_id == "explorer#1", "agent id should be <type>#<index>");
    Expect(result.tool_calls == 0, "plain report should not record tool calls");

    {
        std::lock_guard<std::mutex> lock(captured->mutex);
        Expect(captured->specs.size() == 1, "factory should be called once");
        const auto& spec = captured->specs[0];
        Expect(spec.system_prompt.find("EXPLORER PROMPT") != std::string::npos,
               "child runtime should receive the definition prompt");
        Expect(spec.child_context_id.find("sess-1") != std::string::npos &&
                       spec.child_context_id.find("explorer") != std::string::npos,
               "child context id should derive from the parent session");
        Expect(spec.tool_id_prefix == "sub1:", "tool id prefix should match the run index");
        Expect(spec.max_tool_rounds == 6, "definition max rounds should reach the child runtime");
        Expect(!captured->child_tool_names[0].empty() &&
                       captured->child_tool_names[0][0] == "noop",
               "explorer child tools should come from the allowlist");
    }

    const auto calls = harness->Calls();
    Expect(calls.size() == 1 && calls[0].size() == 2, "child should see exactly system + prompt");
    if (!calls.empty() && calls[0].size() >= 2) {
        Expect(calls[0][0].role == "system" &&
                       calls[0][0].content.find("EXPLORER PROMPT") != std::string::npos,
               "child system message should be the definition prompt");
        Expect(calls[0][1].role == "user" && calls[0][1].content == "explore the widget",
               "child user message should be the delegated prompt");
    }

    const aiagent::agent::SubAgentStats stats = manager->Stats();
    Expect(stats.runs == 1 && stats.failures == 0, "stats should count the run");
}

void TestDepthControl() {
    auto harness = std::make_shared<Harness>();
    auto captured = std::make_shared<CapturedSpecs>();
    const auto factory = MakeFactory(
            captured, [harness](const auto&) -> std::shared_ptr<aiagent::llm::IModelClient> {
                return std::make_shared<ChildModel>(harness, "REPORT");
            });

    // max_depth=1：子 Agent 不能再派生（工具集里没有 task）。
    auto shallow = std::make_shared<aiagent::agent::SubAgentManager>(
            Settings(/*max_depth=*/1), Definitions(), factory, ParentTools());
    aiagent::agent::SubAgentRequest request;
    request.agent_type = "general";
    request.prompt = "do something";
    const auto first = shallow->Run(request, 0);
    Expect(first.ok, "depth-0 run should succeed under max_depth=1");
    {
        std::lock_guard<std::mutex> lock(captured->mutex);
        const auto& names = captured->child_tool_names.back();
        Expect(std::find(names.begin(), names.end(), "task") == names.end(),
               "child at depth 1 must not have the task tool");
    }
    {
        std::lock_guard<std::mutex> lock(captured->mutex);
        captured->specs.clear();
        captured->child_tool_names.clear();
    }

    // max_depth=2：depth-0 的子 Agent 仍可派生。
    auto deep = std::make_shared<aiagent::agent::SubAgentManager>(
            Settings(/*max_depth=*/2), Definitions(), factory, ParentTools());
    const auto second = deep->Run(request, 0);
    Expect(second.ok, "depth-0 run should succeed under max_depth=2");
    {
        std::lock_guard<std::mutex> lock(captured->mutex);
        const auto& names = captured->child_tool_names.back();
        Expect(std::find(names.begin(), names.end(), "task") != names.end(),
               "child at depth 1 should get a nested task tool when max_depth=2");
    }

    // depth 已达上限：直接拒绝。
    const auto rejected = shallow->Run(request, 1);
    Expect(!rejected.ok && rejected.error.find("depth limit") != std::string::npos,
           "run at max depth should be rejected");

    // 未知类型 / 空 prompt。
    aiagent::agent::SubAgentRequest unknown;
    unknown.agent_type = "nope";
    unknown.prompt = "x";
    const auto unknown_result = shallow->Run(unknown, 0);
    Expect(!unknown_result.ok && unknown_result.error.find("unknown agent type") != std::string::npos &&
                   unknown_result.error.find("general") != std::string::npos,
           "unknown agent type should list available definitions");
    aiagent::agent::SubAgentRequest empty_prompt;
    empty_prompt.agent_type = "general";
    const auto empty_result = shallow->Run(empty_prompt, 0);
    Expect(!empty_result.ok && empty_result.error.find("prompt") != std::string::npos,
           "empty prompt should be rejected");
}

void TestConcurrencyGate() {
    auto harness = std::make_shared<Harness>();
    auto captured = std::make_shared<CapturedSpecs>();
    auto manager = std::make_shared<aiagent::agent::SubAgentManager>(
            Settings(/*max_depth=*/1, /*max_concurrent=*/2), Definitions(),
            MakeFactory(captured,
                        [harness](const auto&) -> std::shared_ptr<aiagent::llm::IModelClient> {
                            return std::make_shared<ChildModel>(harness, "REPORT", 150);
                        }),
            ParentTools());

    std::vector<aiagent::agent::SubAgentRequest> requests;
    for (int i = 0; i < 3; ++i) {
        aiagent::agent::SubAgentRequest request;
        request.agent_type = "general";
        request.prompt = "task " + std::to_string(i);
        requests.push_back(request);
    }
    const auto results = manager->RunBatch(requests, 0);
    Expect(results.size() == 3, "batch should return one result per request");
    for (const auto& result : results) {
        Expect(result.ok, "batched sub-agent should succeed");
    }
    Expect(harness->max_active.load() == 2,
           "concurrency gate should cap parallel sub-agents at max_concurrent");
}

void TestTimeoutAndCancel() {
    auto harness = std::make_shared<Harness>();
    auto captured = std::make_shared<CapturedSpecs>();
    auto manager = std::make_shared<aiagent::agent::SubAgentManager>(
            Settings(/*max_depth=*/1, /*max_concurrent=*/3, /*default_timeout_s=*/1,
                     /*max_rounds=*/50),
            Definitions(),
            MakeFactory(captured,
                        [harness](const auto&) -> std::shared_ptr<aiagent::llm::IModelClient> {
                            return std::make_shared<LoopingChildModel>(harness, 60);
                        }),
            ParentTools());

    aiagent::agent::SubAgentRequest request;
    request.agent_type = "general";
    request.prompt = "loop forever";
    const auto timed_out = manager->Run(request, 0);
    Expect(!timed_out.ok && timed_out.timed_out,
           "child exceeding its timeout should be reported as timed out");
    Expect(timed_out.error.find("timeout") != std::string::npos,
           "timeout error should mention timeout");

    // 父取消：should_cancel 一开始就为真。
    aiagent::agent::SubAgentRequest cancelled_request = request;
    cancelled_request.should_cancel = []() { return true; };
    const auto cancelled = manager->Run(cancelled_request, 0);
    Expect(!cancelled.ok && cancelled.cancelled,
           "child should observe the parent's cancellation");
}

void TestResultTruncation() {
    auto harness = std::make_shared<Harness>();
    auto captured = std::make_shared<CapturedSpecs>();
    auto manager = std::make_shared<aiagent::agent::SubAgentManager>(
            Settings(/*max_depth=*/1, /*max_concurrent=*/3, /*default_timeout_s=*/5,
                     /*max_rounds=*/6, /*result_max_chars=*/200),
            Definitions(),
            MakeFactory(captured,
                        [harness](const auto&) -> std::shared_ptr<aiagent::llm::IModelClient> {
                            return std::make_shared<ChildModel>(harness, std::string(2000, 'x'));
                        }),
            ParentTools());

    aiagent::agent::SubAgentRequest request;
    request.agent_type = "general";
    request.prompt = "produce a huge report";
    const auto result = manager->Run(request, 0);
    Expect(result.ok, "large report run should succeed");
    Expect(result.text.find("(truncated 1800 chars)") != std::string::npos,
           "large report should be truncated in the middle");
    Expect(result.text.size() < 400, "truncated report should respect result_max_chars");
}

void TestEventAttribution() {
    auto harness = std::make_shared<Harness>();
    auto captured = std::make_shared<CapturedSpecs>();
    auto manager = std::make_shared<aiagent::agent::SubAgentManager>(
            Settings(), Definitions(),
            MakeFactory(captured,
                        [harness](const auto&) -> std::shared_ptr<aiagent::llm::IModelClient> {
                            return std::make_shared<OneToolCallChildModel>(harness, "REPORT");
                        }),
            ParentTools());

    std::mutex events_mutex;
    std::vector<aiagent::agent::AgentEvent> events;
    aiagent::agent::SubAgentRequest request;
    request.agent_type = "general";
    request.prompt = "use a tool";
    request.parent_session_id = "sess-events";
    request.parent_tool_call_id = "call_parent";
    request.emit = [&events_mutex, &events](const aiagent::agent::AgentEvent& event) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(event);
    };
    const auto result = manager->Run(request, 0);
    Expect(result.ok && result.tool_calls == 1, "child tool call should be counted");

    bool attributed = false;
    bool start_seen = false;
    bool end_seen = false;
    {
        std::lock_guard<std::mutex> lock(events_mutex);
        for (const auto& event : events) {
            if (event.kind == aiagent::agent::AgentEventKind::kToolStart &&
                event.agent_id == "general#1" && event.tool_call_id == "sub1:c1" &&
                event.parent_tool_call_id == "call_parent") {
                attributed = true;
            }
            if (event.kind == aiagent::agent::AgentEventKind::kSubAgentStart &&
                event.agent_id == "general#1" &&
                event.parent_tool_call_id == "call_parent") {
                start_seen = true;
            }
            if (event.kind == aiagent::agent::AgentEventKind::kSubAgentEnd &&
                event.agent_id == "general#1" && event.ok && event.tool_calls == 1 &&
                event.duration_ms >= 0 && event.text.empty()) {
                end_seen = true;
            }
        }
    }
    Expect(attributed, "child events should carry agent id, prefixed tool id and parent id");
    Expect(start_seen, "sub-agent start event should be forwarded with attribution");
    Expect(end_seen, "sub-agent end event should carry ok, duration and tool call count");

    // stream_events=false：仍统计工具调用，但不向父会话转发事件。
    auto quiet_harness = std::make_shared<Harness>();
    auto quiet_captured = std::make_shared<CapturedSpecs>();
    auto quiet = std::make_shared<aiagent::agent::SubAgentManager>(
            Settings(/*max_depth=*/1, /*max_concurrent=*/3, /*default_timeout_s=*/5,
                     /*max_rounds=*/6, /*result_max_chars=*/8000, /*stream_events=*/false),
            Definitions(),
            MakeFactory(quiet_captured,
                        [quiet_harness](const auto&)
                                -> std::shared_ptr<aiagent::llm::IModelClient> {
                            return std::make_shared<OneToolCallChildModel>(quiet_harness, "REPORT");
                        }),
            ParentTools());
    std::size_t quiet_events = 0;
    aiagent::agent::SubAgentRequest quiet_request = request;
    quiet_request.emit = [&quiet_events](const aiagent::agent::AgentEvent&) { ++quiet_events; };
    const auto quiet_result = quiet->Run(quiet_request, 0);
    Expect(quiet_result.ok && quiet_result.tool_calls == 1,
           "quiet mode should still count tool calls");
    Expect(quiet_events == 0, "stream_events=false should not forward child events");
}

void TestTaskToolEndToEnd() {
    auto parent_harness = std::make_shared<Harness>();
    auto child_harness = std::make_shared<Harness>();
    auto captured = std::make_shared<CapturedSpecs>();

    auto parent_tools = std::make_shared<aiagent::tools::CompositeToolProvider>();
    parent_tools->Add(std::make_shared<NoopToolProvider>());

    auto manager = std::make_shared<aiagent::agent::SubAgentManager>(
            Settings(), Definitions(),
            MakeFactory(captured,
                        [child_harness](
                                const auto&) -> std::shared_ptr<aiagent::llm::IModelClient> {
                            return std::make_shared<ChildModel>(child_harness, "REPORT-CONTENT");
                        }),
            parent_tools);
    parent_tools->Add(aiagent::tools::MakeSubAgentToolProvider(manager, 0));

    auto parent_model = std::make_shared<ParentModel>(parent_harness, "explorer",
                                                      "explore the widget");
    aiagent::agent::AgentRuntime runtime(parent_model, "PARENT PROMPT");
    runtime.SetToolProvider(parent_tools);
    runtime.SetMaxToolRounds(4);
    runtime.SetFallbackWorkingDirectory("/tmp");
    aiagent::agent::CompactionSettings compaction;
    compaction.enabled = false;
    runtime.SetCompactionSettings(compaction);

    std::mutex events_mutex;
    std::vector<aiagent::agent::AgentEvent> events;
    const std::string reply = runtime.HandleMessageStreamingEvents(
            a2a::models::MakeTextMessage(a2a::models::Role::kUser, "please explore"), {},
            "sess-e2e",
            [&events_mutex, &events](const aiagent::agent::AgentEvent& event) {
                std::lock_guard<std::mutex> lock(events_mutex);
                events.push_back(event);
            },
            "/tmp", {});

    Expect(reply.find("final: [explorer#1] done in") != std::string::npos,
           "parent should receive the formatted sub-agent result");
    Expect(reply.find("REPORT-CONTENT") != std::string::npos,
           "parent should receive the child report text");

    bool task_tool_end_seen = false;
    {
        std::lock_guard<std::mutex> lock(events_mutex);
        for (const auto& event : events) {
            if (event.kind == aiagent::agent::AgentEventKind::kToolEnd &&
                event.tool_call_id == "call_parent" && event.ok &&
                event.text.find("explorer#1") != std::string::npos) {
                task_tool_end_seen = true;
            }
        }
    }
    Expect(task_tool_end_seen, "task tool end event should carry the sub-agent summary");

    const auto parent_calls = parent_harness->Calls();
    const auto child_calls = child_harness->Calls();
    Expect(parent_calls.size() == 2, "parent should stream twice (tool call + final answer)");
    Expect(child_calls.size() == 1, "child should stream exactly once");
    for (const auto& messages : parent_calls) {
        for (const auto& message : messages) {
            Expect(message.content.find("EXPLORER PROMPT") == std::string::npos,
                   "child system prompt must not leak into the parent context");
        }
    }
    Expect(!child_calls.empty() && !child_calls[0].empty() &&
                   child_calls[0][0].content.find("EXPLORER PROMPT") != std::string::npos,
           "child should run with its own definition prompt");
    // 父的第二轮上下文里，task 结果只能以一条 tool 消息出现（子 Agent 历史不入父上下文）。
    bool parent_tool_message = false;
    for (const auto& messages : parent_calls) {
        for (const auto& message : messages) {
            if (message.role == "tool" && message.tool_call_id == "call_parent") {
                parent_tool_message = true;
                Expect(message.content.find("REPORT-CONTENT") != std::string::npos,
                       "parent tool message should contain the child report");
                Expect(message.content.find("EXPLORER PROMPT") == std::string::npos,
                       "parent tool message should not contain child context");
            }
        }
    }
    Expect(parent_tool_message, "parent context should contain the task tool result");
}

void TestTaskToolBatchAndErrors() {
    auto harness = std::make_shared<Harness>();
    auto captured = std::make_shared<CapturedSpecs>();
    auto manager = std::make_shared<aiagent::agent::SubAgentManager>(
            Settings(), Definitions(),
            MakeFactory(captured,
                        [harness](const auto&) -> std::shared_ptr<aiagent::llm::IModelClient> {
                            return std::make_shared<ChildModel>(harness, "REPORT");
                        }),
            ParentTools());
    aiagent::tools::SubAgentToolProvider tool(manager, 0);

    const auto specs = tool.ListTools();
    Expect(specs.size() == 1 && specs[0].name == "task", "task tool should expose one spec");

    aiagent::tools::ToolContext context;
    context.session_id = "sess-batch";
    context.tool_call_id = "call_batch";

    // 单发。
    const auto single = tool.Execute(
            aiagent::tools::kTaskToolName,
            nlohmann::json{{"prompt", "one"}, {"type", "explorer"}}, context);
    Expect(single.ok && single.text.find("explorer#1") != std::string::npos,
           "single task should report the sub-agent id");

    // 批量：结果按输入顺序拼接。
    const auto batch = tool.Execute(
            aiagent::tools::kTaskToolName,
            nlohmann::json{{"tasks",
                            nlohmann::json::array(
                                    {{{"prompt", "a"}, {"type", "general"}},
                                     {{"prompt", "b"}, {"type", "explorer"}}})}},
            context);
    Expect(batch.ok, "batch task should succeed");
    Expect(batch.text.find("general#2") != std::string::npos &&
                   batch.text.find("explorer#3") != std::string::npos &&
                   batch.text.find("---") != std::string::npos,
           "batch results should be concatenated in order with separators");

    // 参数错误：prompt 与 tasks 混用、空 prompt、未知 type、超批量上限。
    const auto mixed = tool.Execute(
            aiagent::tools::kTaskToolName,
            nlohmann::json{{"prompt", "x"}, {"tasks", nlohmann::json::array()}}, context);
    Expect(!mixed.ok && mixed.error.find("not both") != std::string::npos,
           "prompt and tasks together should be rejected");
    const auto empty_prompt =
            tool.Execute(aiagent::tools::kTaskToolName, nlohmann::json{{"prompt", ""}}, context);
    Expect(!empty_prompt.ok && empty_prompt.error.find("prompt") != std::string::npos,
           "empty prompt should be rejected");
    const auto unknown_type = tool.Execute(
            aiagent::tools::kTaskToolName,
            nlohmann::json{{"prompt", "x"}, {"type", "nope"}}, context);
    Expect(!unknown_type.ok && unknown_type.error.find("unknown agent type") != std::string::npos,
           "unknown type should be rejected before running anything");
    nlohmann::json too_many = nlohmann::json::array();
    for (int i = 0; i < aiagent::tools::kMaxTaskBatch + 1; ++i) {
        too_many.push_back({{"prompt", "t" + std::to_string(i)}});
    }
    const auto oversized =
            tool.Execute(aiagent::tools::kTaskToolName, nlohmann::json{{"tasks", too_many}}, context);
    Expect(!oversized.ok && oversized.error.find("too many tasks") != std::string::npos,
           "batch above kMaxTaskBatch should be rejected");
}

}  // namespace

int main() {
    TestFilteredToolProvider();
    TestDefinitionLoading();
    TestManagerRun();
    TestDepthControl();
    TestConcurrencyGate();
    TestTimeoutAndCancel();
    TestResultTruncation();
    TestEventAttribution();
    TestTaskToolEndToEnd();
    TestTaskToolBatchAndErrors();

    if (g_failures != 0) {
        std::cerr << "sub agent test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "sub agent test passed\n";
    return 0;
}
