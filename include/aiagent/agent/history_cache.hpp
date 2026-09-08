#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "a2a/models/message.hpp"

namespace aiagent::agent {

// 会话「最新完整历史」的内存缓存。
//
// 目的：避免每次请求都 task_store_->ListAll() 全表扫描 + 整表 JSON
// 反序列化——当前每个 task 都冗余存了"截至当时的完整历史"，任务数随
// 消息线性增长，导致长会话下单次请求取历史为 O(消息数²)。
//
// 语义：以 context_id 为键，保存该会话最近一次成功落库后的完整历史
// （含最新 assistant 回复）。调用方约定：
//   - 写路径：在 task_store_->Put(...) 成功后调用 Put() 更新；
//   - 读路径：TryGet() 命中直接使用；未命中时回退 task_store 查询，
//     查询结果非空时可回填（Put）以避免冷启动重复全表扫描。
//
// 一致性：进程内单例持有，随进程存活；进程重启后首次请求自动回退
// 存储层兜底，不产生脏数据。
//
// 线程安全：内部互斥，可被 httplib 线程池并发调用。
class HistoryCache {
public:
    HistoryCache() = default;
    HistoryCache(const HistoryCache&) = delete;
    HistoryCache& operator=(const HistoryCache&) = delete;

    // 命中返回 true 并把该 context 的最新完整历史拷贝到 out。
    bool TryGet(const std::string& context_id,
                std::vector<a2a::models::Message>& out) const;

    // 记录 context 的最新完整历史（覆盖旧值）。
    void Put(const std::string& context_id, std::vector<a2a::models::Message> history);

    // 删除某个 context 的缓存条目。
    void Remove(const std::string& context_id);

    // 清空全部缓存（测试/运维用）。
    void Clear();

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<a2a::models::Message>> latest_;
};

}  // namespace aiagent::agent
