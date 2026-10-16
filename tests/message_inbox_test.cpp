// MessageInbox 单元测试（M5a）：有界收件箱的语义与并发安全。
// 不依赖网络与模型；并发用例只在"多生产者 + 单消费者"这一条不变式上做断言。

#include <algorithm>
#include <atomic>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "aiagent/agent/message_inbox.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

using aiagent::agent::MessageInbox;
using PushResult = MessageInbox::PushResult;

void TestPushDrainOrder() {
    MessageInbox inbox;
    Expect(inbox.Pending() == 0 && !inbox.Closed(), "fresh inbox should be empty and open");
    Expect(inbox.Push("第一句") == PushResult::kAccepted, "first push should be accepted");
    Expect(inbox.Push("第二句") == PushResult::kAccepted, "second push should be accepted");
    Expect(inbox.Pending() == 2, "pending should count both messages");

    const std::vector<std::string> drained = inbox.Drain();
    Expect(drained.size() == 2 && drained[0] == "第一句" && drained[1] == "第二句",
           "drain should preserve FIFO order");
    Expect(inbox.Pending() == 0, "drain should clear the queue");
    Expect(inbox.Drain().empty(), "second drain should be empty");
}

void TestLimits() {
    MessageInbox inbox(MessageInbox::Limits{/*max_messages=*/2, /*max_text_bytes=*/8});
    Expect(inbox.Push("") == PushResult::kTooLarge, "empty text should be rejected");
    Expect(inbox.Push("   \n\t") == PushResult::kTooLarge, "blank text should be rejected");
    Expect(inbox.Push("123456789") == PushResult::kTooLarge, "text over the byte cap should be rejected");
    Expect(inbox.Push("12345678") == PushResult::kAccepted, "text at the byte cap should be accepted");
    Expect(inbox.Push("second") == PushResult::kAccepted, "second message should fit");
    Expect(inbox.Push("third") == PushResult::kFull, "message over the count cap should be rejected");
    const std::vector<std::string> drained = inbox.Drain();
    Expect(drained.size() == 2 && drained[0] == "12345678" && drained[1] == "second",
           "rejected messages must not evict accepted ones");
}

void TestClose() {
    MessageInbox inbox;
    Expect(inbox.Push("before") == PushResult::kAccepted, "push before close should work");
    inbox.Close();
    Expect(inbox.Closed(), "close should flip the state");
    Expect(inbox.Push("after") == PushResult::kClosed, "push after close should be rejected");
    const std::vector<std::string> drained = inbox.Drain();
    Expect(drained.size() == 1 && drained[0] == "before",
           "drain after close should still return the leftovers (for undelivered accounting)");
    Expect(inbox.Pending() == 0, "leftovers should be cleared by the drain");
}

void TestDeliveryCallback() {
    MessageInbox inbox;
    std::mutex seen_mutex;
    std::vector<std::string> seen;
    inbox.SetDeliveryCallback([&seen_mutex, &seen](const std::string& text) {
        std::lock_guard<std::mutex> lock(seen_mutex);
        seen.push_back(text);
        if (text == "boom") {
            throw std::runtime_error("observer failed");
        }
    });
    Expect(inbox.Push("one") == PushResult::kAccepted, "push one");
    Expect(inbox.Push("boom") == PushResult::kAccepted, "push boom");
    Expect(inbox.Push("two") == PushResult::kAccepted, "push two");
    const std::vector<std::string> drained = inbox.Drain();
    Expect(drained.size() == 3, "drain should return all three");
    std::lock_guard<std::mutex> lock(seen_mutex);
    Expect(seen.size() == 3 && seen[0] == "one" && seen[1] == "boom" && seen[2] == "two",
           "callback should fire per message in order; a throwing callback must not stop delivery");
}

void TestConcurrentProducers() {
    MessageInbox inbox(MessageInbox::Limits{/*max_messages=*/5000, /*max_text_bytes=*/64});
    constexpr int kThreads = 4;
    constexpr int kPerThread = 200;
    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&inbox, &accepted, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                const std::string text = "t" + std::to_string(t) + "-" + std::to_string(i);
                if (inbox.Push(text) == PushResult::kAccepted) {
                    accepted.fetch_add(1);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const std::vector<std::string> drained = inbox.Drain();
    const std::set<std::string> unique(drained.begin(), drained.end());
    Expect(accepted.load() == kThreads * kPerThread,
           "every concurrent push should be accepted, got " + std::to_string(accepted.load()));
    Expect(drained.size() == static_cast<std::size_t>(kThreads * kPerThread) &&
                   unique.size() == drained.size(),
           "concurrent pushes must not be lost or duplicated, got " +
                   std::to_string(drained.size()));
}

void TestSteerMessageFormat() {
    const std::string message = aiagent::agent::FormatSteerMessage("只看 src/agent");
    Expect(message.rfind(aiagent::agent::kSteerMarker, 0) == 0,
           "steer message should start with the marker, got: " + message);
    Expect(message.find("只看 src/agent") != std::string::npos,
           "steer message should carry the original text");
    Expect(message.find("do not restart your plan") != std::string::npos,
           "steer message should explain that it is not a new task");
}

}  // namespace

int main() {
    TestPushDrainOrder();
    TestLimits();
    TestClose();
    TestDeliveryCallback();
    TestConcurrentProducers();
    TestSteerMessageFormat();

    if (g_failures != 0) {
        std::cerr << "message inbox test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "message inbox test passed\n";
    return 0;
}
