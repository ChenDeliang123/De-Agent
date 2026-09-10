#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace aiagent::agent {

// 会话「显式标题」元数据存储。
//
// 会话本身由 a2a TaskStore 按 context_id 派生（标题取首条 user 消息），
// 这里额外保存用户通过 `/name` 设置的显式标题，优先级高于派生值。
//
// 持久化：以 JSON 文件保存，path 为空时退化为纯内存（供测试使用）。
// 写入使用「临时文件 + rename」保证原子性；解析失败时从空开始，不阻塞启动。
// 线程安全：内部互斥，可被 httplib 线程池并发调用。
class SessionMetaStore {
public:
    explicit SessionMetaStore(std::string path = {});

    SessionMetaStore(const SessionMetaStore&) = delete;
    SessionMetaStore& operator=(const SessionMetaStore&) = delete;

    // 命中返回 true 并写入 title。
    std::optional<std::string> GetTitle(const std::string& session_id) const;

    // 设置/覆盖显式标题并落盘。
    void SetTitle(const std::string& session_id, const std::string& title);

    // 删除某个会话的显式标题。
    void Remove(const std::string& session_id);

    // 清空全部标题。
    void Clear();

    // 已记录的标题数量。
    std::size_t Size() const;

    const std::string& path() const noexcept { return path_; }

private:
    void Load();
    // 调用方需持有 mutex_。
    void SaveLocked() const;

    std::string path_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> titles_;
};

}  // namespace aiagent::agent
