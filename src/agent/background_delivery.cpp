#include "aiagent/agent/background_delivery.hpp"

#include <cstdio>
#include <utility>

#include "a2a/core/id.hpp"

#include "aiagent/agent/session_history.hpp"

namespace aiagent::agent {
namespace {

std::string FormatSeconds(long long milliseconds) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1fs", static_cast<double>(milliseconds) / 1000.0);
    return buffer;
}

std::string FormatTokens(long long total_tokens) {
    if (total_tokens < 1000) {
        return std::to_string(total_tokens) + " tokens";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1fk tokens",
                  static_cast<double>(total_tokens) / 1000.0);
    return buffer;
}

// 状态词：与注入标题行/CLI 打印共用的措辞（finished / failed / cancelled / timed out）。
const char* StateWord(BackgroundState state) {
    switch (state) {
        case BackgroundState::kRunning:
            return "running";
        case BackgroundState::kDone:
            return "finished";
        case BackgroundState::kFailed:
            return "failed";
        case BackgroundState::kCancelled:
            return "cancelled";
        case BackgroundState::kTimedOut:
            return "timed out";
    }
    return "unknown";
}

}  // namespace

BackgroundResultInjector::BackgroundResultInjector(a2a::server::ITaskStore& task_store,
                                                  HistoryCache& history_cache)
    : task_store_(task_store), history_cache_(history_cache) {}

std::string BackgroundResultInjector::FormatHeadline(const BackgroundTaskSnapshot& snapshot) {
    const SubAgentResult& result = snapshot.result;
    std::string line = "[sub-agent " + snapshot.agent_id + " " + StateWord(snapshot.state) +
                       "] " + snapshot.task_id + " " + BackgroundStateName(snapshot.state) +
                       " in " + FormatSeconds(snapshot.elapsed_ms);
    if (result.ok) {
        line += ", " + std::to_string(result.tool_calls) + " tool calls";
        const long long total_tokens = result.prompt_tokens + result.completion_tokens;
        if (total_tokens > 0) {
            line += ", " + FormatTokens(total_tokens);
        }
    } else if (!result.error.empty()) {
        line += ": " + result.error;
    }
    return line;
}

std::string BackgroundResultInjector::FormatMessage(const BackgroundTaskSnapshot& snapshot) {
    std::string message = FormatHeadline(snapshot);
    if (!snapshot.result.text.empty()) {
        message += "\n" + snapshot.result.text;
    }
    message += "\n(this is an automatic notice about a background sub-agent you started; "
               "no reply is needed unless you want to act on it.)";
    return message;
}

void BackgroundResultInjector::BeginTurn(const std::string& session_id) {
    if (session_id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ++active_turns_[session_id];
    // 先把回合期间排队的结果补写进最新历史：调用方随后读到的历史就包含它们。
    const auto queued = queued_.find(session_id);
    if (queued != queued_.end()) {
        const std::vector<a2a::models::Message> pending = std::move(queued->second);
        queued_.erase(queued);
        for (const auto& message : pending) {
            AppendLocked(session_id, message);
        }
    }
}

void BackgroundResultInjector::EndTurn(const std::string& session_id) {
    if (session_id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = active_turns_.find(session_id);
    if (it == active_turns_.end()) {
        return;
    }
    if (--it->second <= 0) {
        active_turns_.erase(it);
    }
}

bool BackgroundResultInjector::TurnActive(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = active_turns_.find(session_id);
    return it != active_turns_.end() && it->second > 0;
}

bool BackgroundResultInjector::Deliver(const BackgroundTaskSnapshot& snapshot) {
    if (snapshot.session_id.empty()) {
        return false;
    }
    const a2a::models::Message message = a2a::models::MakeTextMessage(
            a2a::models::Role::kUser, FormatMessage(snapshot), "text/plain");
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = active_turns_.find(snapshot.session_id);
    if (it != active_turns_.end() && it->second > 0) {
        // 回合进行中：排队等下一个回合开始补写（回合结束的整体覆盖写会吃掉这里）。
        queued_[snapshot.session_id].push_back(message);
        return true;
    }
    AppendLocked(snapshot.session_id, message);
    return true;
}

void BackgroundResultInjector::AppendLocked(const std::string& session_id,
                                            const a2a::models::Message& message) {
    std::vector<a2a::models::Message> history =
            LoadLatestHistory(&history_cache_, task_store_, session_id);
    history.push_back(message);
    a2a::models::Task task;
    task.id = std::string(a2a::core::NewUuid());
    task.context_id = session_id;
    task.status = a2a::models::MakeTaskStatus(a2a::models::TaskState::kCompleted);
    task.history = history;
    history_cache_.Put(session_id, std::move(history));
    task_store_.Put(std::move(task));
}

}  // namespace aiagent::agent
