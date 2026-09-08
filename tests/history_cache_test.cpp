// HistoryCache 单元测试 + 「会话历史缓存优先」集成测试。
//
// 集成部分模拟 main.cpp 的接线方式（cache-first handler），用包装的
// CountingTaskStore 统计 ListAll() 调用次数，验证：
//   - 新会话首轮 miss → 回退存储层（允许一次全表扫描）；
//   - 后续轮次缓存命中 → ListAll() 不再增长（长会话取历史由 O(n²) 降为 O(1)）；
//   - 返回给 TaskManager 的 task 历史随轮数正确累积（行为与无缓存时一致）。

#include <a2a/models/message.hpp>
#include <a2a/models/requests.hpp>
#include <a2a/server/memory_task_store.hpp>
#include <a2a/server/task_manager.hpp>

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "aiagent/agent/agent_runtime.hpp"
#include "aiagent/agent/history_cache.hpp"
#include "aiagent/llm/deepseek_client.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

// 包装 MemoryTaskStore，统计 ListAll() 被调用的次数。
class CountingStore final : public a2a::server::ITaskStore {
public:
    mutable int list_all_calls = 0;

    std::optional<a2a::models::Task> Get(const std::string& task_id) const override {
        return inner.Get(task_id);
    }
    bool Put(a2a::models::Task task) override { return inner.Put(std::move(task)); }
    bool Delete(const std::string& task_id) override { return inner.Delete(task_id); }
    bool Exists(const std::string& task_id) const override { return inner.Exists(task_id); }
    std::size_t Size() const override { return inner.Size(); }
    std::vector<a2a::models::Task> ListAll() const override {
        ++list_all_calls;
        return inner.ListAll();
    }
    bool UpdateStatus(const std::string& task_id, const a2a::models::TaskStatus& status,
                      std::optional<a2a::models::TaskState> expected_current) override {
        return inner.UpdateStatus(task_id, status, expected_current);
    }
    bool AddArtifact(const std::string& task_id, a2a::models::Artifact artifact) override {
        return inner.AddArtifact(task_id, std::move(artifact));
    }
    bool AppendHistory(const std::string& task_id, a2a::models::Message message) override {
        return inner.AppendHistory(task_id, std::move(message));
    }

    a2a::server::MemoryTaskStore inner;
};

class FakeModel final : public aiagent::llm::IModelClient {
public:
    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>& messages,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        std::string last_user;
        for (const auto& message : messages) {
            if (message.role == "user") {
                last_user = message.content;
            }
        }
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "reply-to-" + last_user;
        choice.finish_reason = "stop";
        aiagent::llm::ChatResult result;
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>&) override {}
};

// 与 main.cpp 相同的 cache-first 接线语义，返回按 context 找最新历史。
std::vector<a2a::models::Message> LoadHistory(aiagent::agent::HistoryCache& cache,
                                               a2a::server::ITaskStore& store,
                                               const std::string& context_id) {
    std::vector<a2a::models::Message> history;
    if (context_id.empty()) {
        return history;
    }
    if (cache.TryGet(context_id, history)) {
        return history;
    }
    std::size_t latest_history_size = 0;
    for (const auto& task : store.ListAll()) {
        if (task.context_id == context_id && task.history.size() > latest_history_size) {
            history = task.history;
            latest_history_size = task.history.size();
        }
    }
    if (!history.empty()) {
        cache.Put(context_id, history);
    }
    return history;
}

void TestHistoryCacheBasics() {
    aiagent::agent::HistoryCache cache;

    std::vector<a2a::models::Message> out;
    Expect(!cache.TryGet("unknown", out), "miss on empty cache");

    std::vector<a2a::models::Message> history;
    history.push_back(a2a::models::MakeTextMessage(a2a::models::Role::kUser, "hello"));
    cache.Put("ctx-1", history);

    Expect(cache.TryGet("ctx-1", out), "hit after put");
    Expect(out.size() == 1, "cached history size");
    Expect(out[0].role == a2a::models::Role::kUser, "cached history role");

    // 覆盖旧值。
    history.push_back(a2a::models::MakeTextMessage(a2a::models::Role::kAgent, "hi"));
    cache.Put("ctx-1", history);
    Expect(cache.TryGet("ctx-1", out), "hit after overwrite");
    Expect(out.size() == 2, "overwritten history size");

    // 不同 context 互不影响。
    Expect(!cache.TryGet("ctx-2", out), "separate context stays empty");

    cache.Remove("ctx-1");
    Expect(!cache.TryGet("ctx-1", out), "miss after remove");

    cache.Put("ctx-a", history);
    cache.Put("ctx-b", history);
    cache.Clear();
    Expect(!cache.TryGet("ctx-a", out), "miss after clear");
    Expect(!cache.TryGet("ctx-b", out), "miss after clear (b)");
}

void TestLongSessionNoFullScan() {
    constexpr int kRounds = 60;  // 模拟长会话（60 轮 = 120 条消息）
    const std::string kContext = "ctx-long";

    auto model = std::make_shared<FakeModel>();
    aiagent::agent::AgentRuntime runtime(model);
    auto store = std::make_shared<CountingStore>();
    aiagent::agent::HistoryCache cache;
    a2a::server::TaskManager task_manager(store);

    task_manager.SetMessageHandler(
            [&](const a2a::models::SendMessageRequest& request) {
                std::vector<a2a::models::Message> history =
                        LoadHistory(cache, *store, request.message.context_id);
                history.push_back(request.message);
                auto response = runtime.HandleMessage(
                        request.message, history, request.message.context_id);
                if (!request.message.context_id.empty() && response.IsTask()) {
                    cache.Put(request.message.context_id, response.GetTask().history);
                }
                return response;
            });

    std::string latest_task_id;
    for (int round = 1; round <= kRounds; ++round) {
        a2a::models::SendMessageRequest request;
        request.message = a2a::models::MakeTextMessage(
                a2a::models::Role::kUser, "user-" + std::to_string(round), "text/plain");
        request.message.context_id = kContext;

        const auto response = task_manager.SendMessage(request);
        Expect(response.IsTask(), "response should carry a task");
        latest_task_id = response.GetTask().id;
    }

    // 首轮 miss 触发一次 ListAll，此后每轮都应缓存命中、不再全表扫描。
    Expect(store->list_all_calls == 1,
           "only the first round should scan the store; got " +
                   std::to_string(store->list_all_calls) + " calls");

    // 行为正确性：最新 task 的历史应累积全部 2*kRounds 条消息。
    const auto latest = store->Get(latest_task_id);
    Expect(latest.has_value(), "latest task should exist in store");
    Expect(latest->history.size() == static_cast<std::size_t>(2 * kRounds),
           "latest task should carry full accumulated history");

    // 缓存内容与存储一致（最新历史 = 2*kRounds 条）。
    std::vector<a2a::models::Message> cached;
    Expect(cache.TryGet(kContext, cached), "cache should hold the latest history");
    Expect(cached.size() == latest->history.size(), "cache should mirror the store history");

    // 冷启动模拟：丢弃缓存后，读取方回退存储层仍能得到完整历史。
    cache.Clear();
    std::vector<a2a::models::Message> from_store = LoadHistory(cache, *store, kContext);
    Expect(from_store.size() == static_cast<std::size_t>(2 * kRounds),
           "cold start should fall back to the store and rebuild full history");
    Expect(store->list_all_calls == 2,
           "cold-start fallback should trigger exactly one more scan");
    Expect(cache.TryGet(kContext, from_store), "fallback should repopulate the cache");
}

}  // namespace

int main() {
    TestHistoryCacheBasics();
    TestLongSessionNoFullScan();

    if (g_failures != 0) {
        std::cerr << "history_cache test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "history_cache test passed\n";
    return 0;
}
