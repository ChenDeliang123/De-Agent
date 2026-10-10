#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "aiagent/permissions/bash_policy.hpp"
#include "aiagent/permissions/permission_store.hpp"

namespace aiagent::permissions {

// 一个项目（`<project-root>/.de/`）的只读配置。
// P0 阶段仅读取，不创建；trusted 恒为 false（尚无信任机制）。
struct ProjectConfig {
    std::string root;
    std::shared_ptr<PermissionStore> permissions;  // null = 无项目 permissions.json
    std::shared_ptr<const BashPolicy> bash_policy; // null = 无项目规则文件
    bool trusted = false;
};

// 从 cwd 向上找最近包含 `.git` 的祖先作为项目根；
// 找不到时：fallback_to_cwd=true 则直接返回 cwd（供 init 使用）；
// 否则若 cwd 自身有 `.de/` 则用 cwd，否则返回空。
std::string FindProjectRoot(const std::string& cwd, bool fallback_to_cwd = false);

// 进程内缓存的项目级配置。按项目根缓存，首次读取后不再失效
// （与全局配置一致：改动需重启后端）。
// 线程安全：内部互斥，可被 httplib 线程池与 Agent 执行线程并发使用。
class ProjectConfigCache {
public:
    // 返回 cwd 对应项目的配置；无项目根或无有效配置时返回 nullptr。
    std::shared_ptr<const ProjectConfig> Get(const std::string& cwd);

    void Clear();

private:
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const ProjectConfig>> cache_;
};

}  // namespace aiagent::permissions
