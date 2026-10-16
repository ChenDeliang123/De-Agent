#include "aiagent/agent/session_history.hpp"

#include <optional>

namespace aiagent::agent {

std::vector<a2a::models::Message> LoadLatestHistory(HistoryCache* cache,
                                                   a2a::server::ITaskStore& store,
                                                   const std::string& context_id) {
    std::vector<a2a::models::Message> history;
    if (context_id.empty()) {
        return history;
    }
    if (cache != nullptr && cache->TryGet(context_id, history)) {
        return history;
    }
    std::optional<a2a::core::TimePoint> latest_time;
    for (const auto& task : store.ListAll()) {
        if (task.context_id != context_id || task.history.empty()) {
            continue;
        }
        if (latest_time.has_value()) {
            if (!task.status.timestamp.has_value() || !(*task.status.timestamp > *latest_time)) {
                continue;
            }
        }
        history = task.history;
        latest_time = task.status.timestamp;
    }
    if (cache != nullptr && !history.empty()) {
        // 冷启动回填，避免每个会话都重复全表扫描。
        cache->Put(context_id, history);
    }
    return history;
}

}  // namespace aiagent::agent
