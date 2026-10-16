// BackgroundResultInjector 单元测试：后台结果 → 会话历史注入（M4c 路径 2）。
// 全部使用内存 TaskStore + 真实 HistoryCache，确定性：不依赖网络与线程调度。

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "a2a/models/message.hpp"
#include "a2a/models/task.hpp"
#include "a2a/server/memory_task_store.hpp"

#include "aiagent/agent/background_delivery.hpp"
#include "aiagent/agent/history_cache.hpp"
#include "aiagent/agent/session_history.hpp"
#include "aiagent/agent/sub_agent.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

aiagent::agent::BackgroundTaskSnapshot DoneSnapshot(const std::string& session_id,
                                                    const std::string& task_id,
                                                    const std::string& agent_id,
                                                    const std::string& text =
                                                            "REPORT-BODY") {
    aiagent::agent::BackgroundTaskSnapshot snapshot;
    snapshot.task_id = task_id;
    snapshot.agent_id = agent_id;
    snapshot.agent_type = "explorer";
    snapshot.session_id = session_id;
    snapshot.state = aiagent::agent::BackgroundState::kDone;
    snapshot.elapsed_ms = 12345;
    snapshot.result.ok = true;
    snapshot.result.agent_id = agent_id;
    snapshot.result.text = text;
    snapshot.result.duration_ms = 12345;
    snapshot.result.tool_calls = 4;
    snapshot.result.prompt_tokens = 5000;
    snapshot.result.completion_tokens = 200;
    return snapshot;
}

// 历史条目的文本（首条 part 的 text）。
std::string TextOf(const a2a::models::Message& message) {
    for (const auto& part : message.parts) {
        if (part.text.has_value()) {
            return *part.text;
        }
    }
    return {};
}

void TestMessageFormat() {
    const auto snapshot = DoneSnapshot("sess-1", "bg-3", "explorer#7");
    const std::string headline =
            aiagent::agent::BackgroundResultInjector::FormatHeadline(snapshot);
    Expect(headline ==
                   "[sub-agent explorer#7 finished] bg-3 done in 12.3s, 4 tool calls, 5.2k tokens",
           "done headline should carry state/agent/task/metrics, got: " + headline);

    aiagent::agent::BackgroundTaskSnapshot failed =
            DoneSnapshot("sess-1", "bg-4", "general#2");
    failed.state = aiagent::agent::BackgroundState::kFailed;
    failed.result.ok = false;
    failed.result.text.clear();
    failed.result.error = "child exploded";
    Expect(aiagent::agent::BackgroundResultInjector::FormatHeadline(failed) ==
                   "[sub-agent general#2 failed] bg-4 failed in 12.3s: child exploded",
           "failed headline should carry the reason, got: " +
                   aiagent::agent::BackgroundResultInjector::FormatHeadline(failed));

    aiagent::agent::BackgroundTaskSnapshot cancelled =
            DoneSnapshot("sess-1", "bg-5", "general#3");
    cancelled.state = aiagent::agent::BackgroundState::kCancelled;
    cancelled.result.ok = false;
    cancelled.result.text.clear();
    cancelled.result.error = "stopped";
    Expect(aiagent::agent::BackgroundResultInjector::FormatHeadline(cancelled) ==
                   "[sub-agent general#3 cancelled] bg-5 cancelled in 12.3s: stopped",
           "cancelled headline should carry the reason");

    const std::string message =
            aiagent::agent::BackgroundResultInjector::FormatMessage(snapshot);
    Expect(message.find(headline) == 0 && message.find("REPORT-BODY") != std::string::npos,
           "message should be headline + report body");
    Expect(message.find("no reply is needed") != std::string::npos,
           "message should tell the model that no reply is needed");
}

void TestDeliverOutOfTurn() {
    a2a::server::MemoryTaskStore store;
    aiagent::agent::HistoryCache cache;
    aiagent::agent::BackgroundResultInjector injector(store, cache);

    // 先有一条历史（模拟上一回合落库）。
    std::vector<a2a::models::Message> history;
    history.push_back(a2a::models::MakeTextMessage(a2a::models::Role::kUser, "hello"));
    a2a::models::Task task;
    task.id = "task-1";
    task.context_id = "sess-1";
    task.status = a2a::models::MakeTaskStatus(a2a::models::TaskState::kCompleted);
    task.history = history;
    cache.Put("sess-1", history);
    store.Put(std::move(task));

    // 空会话 id：不注入。
    aiagent::agent::BackgroundTaskSnapshot orphan = DoneSnapshot("", "bg-9", "general#9");
    Expect(!injector.Deliver(orphan), "empty session should not be delivered");

    // 回合外（无活跃回合）：立即写入历史与存储。
    Expect(injector.Deliver(DoneSnapshot("sess-1", "bg-3", "explorer#7")),
           "out-of-turn delivery should succeed");
    std::vector<a2a::models::Message> loaded =
            aiagent::agent::LoadLatestHistory(&cache, store, "sess-1");
    Expect(loaded.size() == 2 && loaded[1].role == a2a::models::Role::kUser,
           "injected notice should be appended as a user message, got: " +
                   std::to_string(loaded.size()));
    Expect(TextOf(loaded.back()).find("[sub-agent explorer#7 finished] bg-3") == 0,
           "injected notice should start with the tagged headline, got: " +
                   TextOf(loaded.back()));
    Expect(store.Size() == 2, "injection should persist a new task snapshot");
    Expect(!injector.TurnActive("sess-1"), "no turn should be active");
}

void TestDeliverDuringTurnIsQueued() {
    a2a::server::MemoryTaskStore store;
    aiagent::agent::HistoryCache cache;
    aiagent::agent::BackgroundResultInjector injector(store, cache);

    {
        const aiagent::agent::BackgroundTurnGuard turn(&injector, "sess-2");
        Expect(injector.TurnActive("sess-2"), "turn guard should mark the session active");
        // 回合中到达：排队，不落库（回合结束的整体覆盖写会吃掉这里）。
        Expect(injector.Deliver(DoneSnapshot("sess-2", "bg-1", "explorer#1")),
               "in-turn delivery should queue");
        Expect(store.Size() == 0, "in-turn delivery must not write while the turn runs");
        Expect(aiagent::agent::LoadLatestHistory(&cache, store, "sess-2").empty(),
               "in-turn delivery must not appear before the turn ends");
    }
    Expect(!injector.TurnActive("sess-2"), "turn guard destructor should end the turn");

    // 下一个回合开始：先补写排队中的通知，调用方随后读到的历史里就有它。
    {
        const aiagent::agent::BackgroundTurnGuard turn(&injector, "sess-2");
        const std::vector<a2a::models::Message> loaded =
                aiagent::agent::LoadLatestHistory(&cache, store, "sess-2");
        Expect(loaded.size() == 1 &&
                       TextOf(loaded[0]).find("[sub-agent explorer#1 finished] bg-1") == 0,
               "queued notice should land in the history at the next turn start");
        Expect(store.Size() == 1, "queued notice should be persisted at the next turn start");
    }
    // 补写之后不再重复注入。
    {
        const aiagent::agent::BackgroundTurnGuard turn(&injector, "sess-2");
        Expect(aiagent::agent::LoadLatestHistory(&cache, store, "sess-2").size() == 1,
               "queued notice should be written exactly once");
    }
}

void TestTurnEdges() {
    a2a::server::MemoryTaskStore store;
    aiagent::agent::HistoryCache cache;
    aiagent::agent::BackgroundResultInjector injector(store, cache);

    // 空会话 id 的回合边界不产生状态。
    const aiagent::agent::BackgroundTurnGuard empty_turn(&injector, "");
    Expect(!injector.TurnActive(""), "empty session should never be active");

    // 嵌套/重复 EndTurn 不应把它算成"无回合"或崩掉。
    injector.BeginTurn("sess-3");
    injector.BeginTurn("sess-3");
    injector.EndTurn("sess-3");
    Expect(injector.TurnActive("sess-3"), "nested turn should stay active until the last end");
    injector.EndTurn("sess-3");
    injector.EndTurn("sess-3");  // 多余的 End：安全且无副作用
    Expect(!injector.TurnActive("sess-3"), "turn should end after the counted ends");

    // 未知会话的 EndTurn 安全。
    injector.EndTurn("sess-unknown");
    Expect(!injector.TurnActive("sess-unknown"), "unknown session is not active");
}

}  // namespace

int main() {
    TestMessageFormat();
    TestDeliverOutOfTurn();
    TestDeliverDuringTurnIsQueued();
    TestTurnEdges();

    if (g_failures != 0) {
        std::cerr << "background delivery test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "background delivery test passed\n";
    return 0;
}
