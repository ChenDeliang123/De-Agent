#include "aiagent/agent/history_cache.hpp"

#include <utility>

namespace aiagent::agent {

bool HistoryCache::TryGet(const std::string& context_id,
                          std::vector<a2a::models::Message>& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = latest_.find(context_id);
    if (it == latest_.end()) {
        return false;
    }
    out = it->second;
    return true;
}

void HistoryCache::Put(const std::string& context_id,
                       std::vector<a2a::models::Message> history) {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_[std::move(context_id)] = std::move(history);
}

void HistoryCache::Remove(const std::string& context_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_.erase(context_id);
}

void HistoryCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_.clear();
}

}  // namespace aiagent::agent
