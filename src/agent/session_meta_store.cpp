#include "aiagent/agent/session_meta_store.hpp"

#include <json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace aiagent::agent {

SessionMetaStore::SessionMetaStore(std::string path) : path_(std::move(path)) {
    if (!path_.empty()) {
        Load();
    }
}

void SessionMetaStore::Load() {
    std::ifstream input(path_);
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
        titles_.clear();
        cwds_.clear();
        if (json.contains("titles") && json["titles"].is_object()) {
            for (auto it = json["titles"].begin(); it != json["titles"].end(); ++it) {
                if (it.value().is_string()) {
                    titles_[it.key()] = it.value().get<std::string>();
                }
            }
        }
        if (json.contains("cwds") && json["cwds"].is_object()) {
            for (auto it = json["cwds"].begin(); it != json["cwds"].end(); ++it) {
                if (it.value().is_string()) {
                    cwds_[it.key()] = it.value().get<std::string>();
                }
            }
        }
    } catch (const nlohmann::json::exception&) {
        // 元数据文件损坏时从空开始，不阻塞进程启动。
    }
}

void SessionMetaStore::SaveLocked() const {
    if (path_.empty()) {
        return;
    }
    nlohmann::json json;
    json["titles"] = nlohmann::json::object();
    for (const auto& [session_id, title] : titles_) {
        json["titles"][session_id] = title;
    }
    json["cwds"] = nlohmann::json::object();
    for (const auto& [session_id, cwd] : cwds_) {
        json["cwds"][session_id] = cwd;
    }
    const std::filesystem::path target(path_);
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    const std::filesystem::path temp = std::filesystem::path(path_ + ".tmp");
    {
        std::ofstream output(temp, std::ios::trunc);
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

std::optional<std::string> SessionMetaStore::GetTitle(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = titles_.find(session_id);
    if (it == titles_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void SessionMetaStore::SetTitle(const std::string& session_id, const std::string& title) {
    std::lock_guard<std::mutex> lock(mutex_);
    titles_[session_id] = title;
    SaveLocked();
}

std::optional<std::string> SessionMetaStore::GetCwd(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cwds_.find(session_id);
    if (it == cwds_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void SessionMetaStore::SetCwd(const std::string& session_id, const std::string& cwd) {
    std::lock_guard<std::mutex> lock(mutex_);
    cwds_[session_id] = cwd;
    SaveLocked();
}

void SessionMetaStore::Remove(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool removed_title = titles_.erase(session_id) > 0;
    const bool removed_cwd = cwds_.erase(session_id) > 0;
    if (removed_title || removed_cwd) {
        SaveLocked();
    }
}

void SessionMetaStore::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    titles_.clear();
    cwds_.clear();
    SaveLocked();
}

std::size_t SessionMetaStore::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return titles_.size();
}

}  // namespace aiagent::agent
