// ApprovalBroker 的单元测试：等待/唤醒、超时、会话取消。

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "aiagent/agent/approval_broker.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

using aiagent::agent::ApprovalBroker;
using aiagent::agent::ApprovalDecision;

void TestResolveWakesWaiter() {
    ApprovalBroker broker;
    std::atomic<ApprovalDecision> result{ApprovalDecision::kDeny};
    std::thread waiter([&] {
        result = broker.Wait("s1", "call-1", std::chrono::seconds(5));
    });

    // 等 Wait 注册完成。
    for (int i = 0; i < 200 && broker.pending_count() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Expect(broker.Resolve("s1", "call-1", ApprovalDecision::kAllowOnce),
           "resolve should find the pending request");
    waiter.join();
    Expect(result.load() == ApprovalDecision::kAllowOnce,
           "waiter should receive allow-once");
    Expect(broker.pending_count() == 0, "pending map should be empty after resolve");
}

void TestTimeoutDenies() {
    ApprovalBroker broker;
    const auto started = std::chrono::steady_clock::now();
    const auto decision = broker.Wait("s2", "call-2", std::chrono::milliseconds(30));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    Expect(decision == ApprovalDecision::kDeny, "timeout should deny");
    Expect(elapsed >= std::chrono::milliseconds(25), "wait should block until timeout");
}

void TestCancelSessionDenies() {
    ApprovalBroker broker;
    std::atomic<ApprovalDecision> result{ApprovalDecision::kAllowOnce};
    std::thread waiter([&] {
        result = broker.Wait("s3", "call-3", std::chrono::seconds(5));
    });
    for (int i = 0; i < 200 && broker.pending_count() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    broker.CancelSession("s3");
    waiter.join();
    Expect(result.load() == ApprovalDecision::kDeny, "cancel should deny the waiter");

    // 取消另一个会话不应影响本会话（这里已无 pending）。
    Expect(!broker.Resolve("s9", "call-9", ApprovalDecision::kAllowOnce),
           "resolving unknown request returns false");
}

}  // namespace

int main() {
    TestResolveWakesWaiter();
    TestTimeoutDenies();
    TestCancelSessionDenies();

    if (g_failures != 0) {
        std::cerr << "approval broker test failed with " << g_failures
                  << " failure(s)\n";
        return 1;
    }
    std::cout << "approval broker test passed\n";
    return 0;
}
