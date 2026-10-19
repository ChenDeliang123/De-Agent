#include "aiagent/permissions/project_config.hpp"

#include "aiagent/platform/paths.hpp"

#include <filesystem>
#include <system_error>

namespace aiagent::permissions {

std::string FindProjectRoot(const std::string& cwd, bool fallback_to_cwd) {
    if (cwd.empty()) {
        return {};
    }
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::weakly_canonical(aiagent::platform::FromUtf8(cwd), ec);
    if (ec) {
        dir = std::filesystem::absolute(aiagent::platform::FromUtf8(cwd), ec);
        if (ec) {
            return {};
        }
    }

    std::filesystem::path current = dir;
    while (true) {
        if (std::filesystem::exists(current / ".git", ec)) {
            return aiagent::platform::ToUtf8(current);
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
    }

    // 非 git 项目：init 时直接认 cwd；读取时只在 cwd 已有 .de/ 时认。
    if (fallback_to_cwd) {
        return aiagent::platform::ToUtf8(dir);
    }
    if (std::filesystem::is_directory(dir / ".de", ec)) {
        return aiagent::platform::ToUtf8(dir);
    }
    return {};
}

std::shared_ptr<const ProjectConfig> ProjectConfigCache::Get(const std::string& cwd) {
    const std::string root = FindProjectRoot(cwd);
    if (root.empty()) {
        return nullptr;
    }
    const std::filesystem::path de_dir = aiagent::platform::FromUtf8(root) / ".de";
    std::error_code ec;
    if (!std::filesystem::is_directory(de_dir, ec)) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cache_.find(root);
    if (it != cache_.end()) {
        return it->second;
    }

    auto config = std::make_shared<ProjectConfig>();
    config->root = root;

    const std::filesystem::path permissions_file = de_dir / "permissions.json";
    if (std::filesystem::exists(permissions_file, ec)) {
        config->permissions =
                std::make_shared<PermissionStore>(aiagent::platform::ToUtf8(permissions_file));
    }

    const std::filesystem::path bash_file = de_dir / "bash_policy.rules";
    if (std::filesystem::exists(bash_file, ec)) {
        BashPolicy policy;
        if (LoadBashPolicyFile(aiagent::platform::ToUtf8(bash_file), &policy, nullptr)) {
            config->bash_policy = std::make_shared<const BashPolicy>(std::move(policy));
        }
    }

    // P0：尚无信任机制，项目级一律视为未信任（只能收紧，不能放松）。
    config->trusted = false;

    if (!config->permissions && !config->bash_policy) {
        cache_[root] = nullptr;
        return nullptr;
    }
    cache_[root] = config;
    return config;
}

void ProjectConfigCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

}  // namespace aiagent::permissions
