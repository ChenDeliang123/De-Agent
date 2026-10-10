#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "aiagent/permissions/permission_policy.hpp"

namespace aiagent::permissions {

// 一次权限解析结果。
struct PermissionEntry {
    std::string path;   // 实际命中的 canonical 路径
    PermissionMode mode = PermissionMode::kConfirm;
    bool inherited = false;  // true 表示由祖先目录继承而来
};

// 用户级「按工作路径」权限存储。
//
// 语义（对齐 pi 的 ~/.pi/agent/trust.json）：
//   - 键为 canonical 绝对路径；
//   - Resolve 从给定路径逐级向上找最近的已配置祖先；
//   - full 不参与继承（ModeInherits(mode)==false），只对精确路径生效；
//   - 落盘为 JSON，写入使用「临时文件 + rename」原子替换；目录按需创建。
//
// 线程安全：内部互斥，可被 httplib 线程池并发调用。
class PermissionStore {
public:
    // path 为空时退化为纯内存实现（供测试）。
    explicit PermissionStore(std::string path = {});

    PermissionStore(const PermissionStore&) = delete;
    PermissionStore& operator=(const PermissionStore&) = delete;

    // 解析某路径的有效模式（含就近继承）。nullopt 表示未配置。
    std::optional<PermissionEntry> Resolve(const std::string& path) const;

    // 为精确路径设置模式并落盘。
    void Set(const std::string& path, PermissionMode mode);

    // 删除精确路径的配置（重置为继承/默认）。返回是否确实删除。
    bool Remove(const std::string& path);

    // 全部显式配置（canonical 路径 -> 模式）。
    std::map<std::string, PermissionMode> List() const;

    void Clear();
    std::size_t Size() const;

    const std::string& path() const noexcept { return path_; }

private:
    void Load();
    // 调用方需持有 mutex_。
    void SaveLocked() const;

    static std::string Canonicalize(const std::string& path);

    std::string path_;
    mutable std::mutex mutex_;
    std::map<std::string, PermissionMode> modes_;
};

}  // namespace aiagent::permissions
