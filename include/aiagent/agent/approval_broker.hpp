#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace aiagent::agent {

// 一次工具调用的审批结果。
enum class ApprovalDecision {
    kAllowOnce,     // 仅本次允许
    kAllowSession,  // 本会话内该工具一律允许
    kDeny,          // 拒绝（也是超时/取消时的默认值）
};

// 逐次人工确认的「等待/唤醒」中枢。
//
// AgentRuntime 在需要确认时以 (session_id, tool_call_id) 为键调用 Wait 并阻塞，
// CLI 收到 approval_request 事件后弹提示，再通过 /api/v1/chat/approve 调用
// Resolve 唤醒。会话取消时 CancelSession 会让所有未决请求立即返回 kDeny。
//
// 线程安全：内部互斥 + 条件变量，可被 httplib 线程池与 Agent 执行线程并发使用。
class ApprovalBroker {
public:
    // 阻塞等待决定；超时、会话取消或已不存在时返回 kDeny。
    ApprovalDecision Wait(const std::string& session_id,
                          const std::string& tool_call_id,
                          std::chrono::milliseconds timeout);

    // 由审批接口调用；命中等待中的请求返回 true，否则 false。
    bool Resolve(const std::string& session_id,
                 const std::string& tool_call_id,
                 ApprovalDecision decision);

    // 会话取消：唤醒该会话所有未决请求并置为 kDeny。
    void CancelSession(const std::string& session_id);

    // 当前未决请求数（测试/观测用）。
    std::size_t pending_count() const;

private:
    struct Pending {
        std::condition_variable cv;
        bool resolved = false;
        ApprovalDecision decision = ApprovalDecision::kDeny;
    };

    static std::string Key(const std::string& session_id,
                           const std::string& tool_call_id);

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Pending>> pending_;
};

}  // namespace aiagent::agent
