#pragma once

#include <string>
#include <vector>

#include "a2a/models/message.hpp"
#include "a2a/server/task_store.hpp"

#include "aiagent/agent/history_cache.hpp"

namespace aiagent::agent {

// 缓存优先地取某个会话的最新完整历史；cache 为空或未命中时回退存储层。
// 存储层按任务写入时间取最新的那条：手动/自动压缩会写入更短的历史，
// 单纯比较条数会错误地回退到压缩前的旧任务。
std::vector<a2a::models::Message> LoadLatestHistory(HistoryCache* cache,
                                                    a2a::server::ITaskStore& store,
                                                    const std::string& context_id);

}  // namespace aiagent::agent
