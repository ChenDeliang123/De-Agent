#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace aiagent::permissions {

// 用户级「项目信任」存储（`~/.de/trust.json`）。
//
// 语义：键为 canonical 绝对路径，值 true/false；Resolve 从给定路径向
// 上查找最近的显式决定（就近继承）。项目级 `.de/` 配置只有在被信任时才
// 允许「放松」权限；未信任时只能收紧。
//
// 线程安全：内部互斥；写入使用「临时文件 + rename」原子替换。
class TrustStore {
public:
    // path 为空时退化为纯内存实现（供测试）。
    explicit TrustStore(std::string path = {});

    TrustStore(const TrustStore&) = delete;
    TrustStore& operator=(const TrustStore&) = delete;

    // 就近继承解析；nullopt 表示未配置（调用方视为未信任）。
    std::optional<bool> Resolve(const std::string& path) const;

    // 是否被信任（未配置视为 false）。
    bool IsTrusted(const std::string& path) const;

    void Set(const std::string& path, bool trusted);
    bool Remove(const std::string& path);

    std::map<std::string, bool> List() const;
    void Clear();
    std::size_t Size() const;

    const std::string& path() const noexcept { return path_; }

private:
    void Load();
    void SaveLocked() const;
    static std::string Canonicalize(const std::string& path);

    std::string path_;
    mutable std::mutex mutex_;
    std::map<std::string, bool> decisions_;
};

}  // namespace aiagent::permissions
