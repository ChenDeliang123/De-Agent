#include "aiagent/agent/approval_broker.hpp"

#include <utility>
#include <vector>

namespace aiagent::agent {

std::string ApprovalBroker::Key(const std::string& session_id,
                                const std::string& tool_call_id) {
    return session_id + "\n" + tool_call_id;
}

ApprovalDecision ApprovalBroker::Wait(const std::string& session_id,
                                      const std::string& tool_call_id,
                                      std::chrono::milliseconds timeout) {
    auto pending = std::make_shared<Pending>();
    const std::string key = Key(session_id, tool_call_id);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[key] = pending;
    }

    ApprovalDecision decision = ApprovalDecision::kDeny;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pending->cv.wait_for(lock, timeout, [&pending] { return pending->resolved; });
        decision = pending->resolved ? pending->decision : ApprovalDecision::kDeny;
        pending_.erase(key);
    }
    return decision;
}

bool ApprovalBroker::Resolve(const std::string& session_id,
                             const std::string& tool_call_id,
                             ApprovalDecision decision) {
    const std::string key = Key(session_id, tool_call_id);
    std::shared_ptr<Pending> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pending_.find(key);
        if (it == pending_.end()) {
            return false;
        }
        pending = it->second;
        pending->resolved = true;
        pending->decision = decision;
    }
    pending->cv.notify_all();
    return true;
}

void ApprovalBroker::CancelSession(const std::string& session_id) {
    const std::string prefix = session_id + "\n";
    std::vector<std::shared_ptr<Pending>> to_notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->first.rfind(prefix, 0) == 0) {
                it->second->resolved = true;
                it->second->decision = ApprovalDecision::kDeny;
                to_notify.push_back(it->second);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& pending : to_notify) {
        pending->cv.notify_all();
    }
}

std::size_t ApprovalBroker::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

}  // namespace aiagent::agent
