#include "aiagent/permissions/permission_store.hpp"

#include "aiagent/platform/paths.hpp"

#include <json.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace aiagent::permissions {
namespace {

std::string NowIso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local) == 0) {
        return {};
    }
    return buffer;
}

}  // namespace

PermissionStore::PermissionStore(std::string path) : path_(std::move(path)) {
    if (!path_.empty()) {
        Load();
    }
}

std::string PermissionStore::Canonicalize(const std::string& path) {
    std::error_code ec;
    std::filesystem::path target = platform::FromUtf8(path);
    if (!target.is_absolute()) {
        std::error_code abs_ec;
        target = std::filesystem::absolute(target, abs_ec);
        if (abs_ec) {
            return platform::NormalizePathKey(
                    platform::ToUtf8(target.lexically_normal()));
        }
    }
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(target, ec);
    if (ec) {
        return platform::NormalizePathKey(
                platform::ToUtf8(target.lexically_normal()));
    }
    return platform::NormalizePathKey(platform::ToUtf8(canonical));
}

void PermissionStore::Load() {
    std::ifstream input(aiagent::platform::FromUtf8(path_));
    if (!input) {
        return;
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();
    if (text.empty()) {
        return;
    }
    try {
        const auto json = nlohmann::json::parse(text);
        std::lock_guard<std::mutex> lock(mutex_);
        modes_.clear();
        if (!json.contains("paths") || !json["paths"].is_object()) {
            return;
        }
        for (auto it = json["paths"].begin(); it != json["paths"].end(); ++it) {
            std::string mode_text;
            if (it.value().is_string()) {
                mode_text = it.value().get<std::string>();
            } else if (it.value().is_object() && it.value().contains("mode") &&
                       it.value()["mode"].is_string()) {
                mode_text = it.value()["mode"].get<std::string>();
            } else {
                continue;
            }
            const auto mode = ParseMode(mode_text);
            if (mode.has_value()) {
                modes_[it.key()] = *mode;
            }
        }
    } catch (const nlohmann::json::exception&) {
        // 文件损坏时从空开始，不阻塞进程启动。
    }
}

void PermissionStore::SaveLocked() const {
    if (path_.empty()) {
        return;
    }
    nlohmann::json json;
    json["version"] = 1;
    json["paths"] = nlohmann::json::object();
    for (const auto& [key, mode] : modes_) {
        json["paths"][key] = {{"mode", ModeName(mode)}, {"updated_at", NowIso8601()}};
    }
    const std::filesystem::path target = aiagent::platform::FromUtf8(path_);
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    const std::filesystem::path temp = aiagent::platform::FromUtf8(path_ + ".tmp");
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            return;
        }
        output << json.dump(2);
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        // Windows 上覆盖已存在文件可能失败，退化为删除后重试。
        std::filesystem::remove(target, ec);
        std::filesystem::rename(temp, target, ec);
    }
}

std::optional<PermissionEntry> PermissionStore::Resolve(const std::string& path) const {
    if (path.empty()) {
        return std::nullopt;
    }
    const std::string canonical = Canonicalize(path);
    std::lock_guard<std::mutex> lock(mutex_);

    std::filesystem::path candidate = platform::FromUtf8(canonical);
    while (true) {
        const std::string key =
                platform::NormalizePathKey(platform::ToUtf8(candidate));
        const auto it = modes_.find(key);
        if (it != modes_.end()) {
            const bool exact = (key == canonical);
            if (exact || ModeInherits(it->second)) {
                PermissionEntry entry;
                entry.path = key;
                entry.mode = it->second;
                entry.inherited = !exact;
                return entry;
            }
            // full 祖先：跳过，继续向上找。
        }
        const std::filesystem::path parent = candidate.parent_path();
        if (parent.empty() || parent == candidate) {
            break;
        }
        candidate = parent;
    }
    return std::nullopt;
}

void PermissionStore::Set(const std::string& path, PermissionMode mode) {
    const std::string key = Canonicalize(path);
    std::lock_guard<std::mutex> lock(mutex_);
    modes_[key] = mode;
    SaveLocked();
}

bool PermissionStore::Remove(const std::string& path) {
    const std::string key = Canonicalize(path);
    std::lock_guard<std::mutex> lock(mutex_);
    const bool removed = modes_.erase(key) > 0;
    if (removed) {
        SaveLocked();
    }
    return removed;
}

std::map<std::string, PermissionMode> PermissionStore::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return modes_;
}

void PermissionStore::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    modes_.clear();
    SaveLocked();
}

std::size_t PermissionStore::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return modes_.size();
}

}  // namespace aiagent::permissions
