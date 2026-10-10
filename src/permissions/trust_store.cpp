#include "aiagent/permissions/trust_store.hpp"

#include <json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace aiagent::permissions {

TrustStore::TrustStore(std::string path) : path_(std::move(path)) {
    if (!path_.empty()) {
        Load();
    }
}

std::string TrustStore::Canonicalize(const std::string& path) {
    std::error_code ec;
    std::filesystem::path target(path);
    if (!target.is_absolute()) {
        std::error_code abs_ec;
        target = std::filesystem::absolute(target, abs_ec);
        if (abs_ec) {
            return target.lexically_normal().string();
        }
    }
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(target, ec);
    if (ec) {
        return target.lexically_normal().string();
    }
    return canonical.string();
}

void TrustStore::Load() {
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
        decisions_.clear();
        if (!json.contains("trust") || !json["trust"].is_object()) {
            return;
        }
        for (auto it = json["trust"].begin(); it != json["trust"].end(); ++it) {
            if (it.value().is_boolean()) {
                decisions_[it.key()] = it.value().get<bool>();
            }
        }
    } catch (const nlohmann::json::exception&) {
        // 文件损坏时从空开始。
    }
}

void TrustStore::SaveLocked() const {
    if (path_.empty()) {
        return;
    }
    nlohmann::json json;
    json["version"] = 1;
    json["trust"] = nlohmann::json::object();
    for (const auto& [key, trusted] : decisions_) {
        json["trust"][key] = trusted;
    }
    const std::filesystem::path target(path_);
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    const std::filesystem::path temp(path_ + ".tmp");
    {
        std::ofstream output(temp, std::ios::trunc);
        if (!output) {
            return;
        }
        output << json.dump(2);
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(target, ec);
        std::filesystem::rename(temp, target, ec);
    }
}

std::optional<bool> TrustStore::Resolve(const std::string& path) const {
    if (path.empty()) {
        return std::nullopt;
    }
    const std::string canonical = Canonicalize(path);
    std::lock_guard<std::mutex> lock(mutex_);
    std::filesystem::path candidate(canonical);
    while (true) {
        const auto it = decisions_.find(candidate.string());
        if (it != decisions_.end()) {
            return it->second;
        }
        const std::filesystem::path parent = candidate.parent_path();
        if (parent.empty() || parent == candidate) {
            break;
        }
        candidate = parent;
    }
    return std::nullopt;
}

bool TrustStore::IsTrusted(const std::string& path) const {
    return Resolve(path).value_or(false);
}

void TrustStore::Set(const std::string& path, bool trusted) {
    const std::string key = Canonicalize(path);
    std::lock_guard<std::mutex> lock(mutex_);
    decisions_[key] = trusted;
    SaveLocked();
}

bool TrustStore::Remove(const std::string& path) {
    const std::string key = Canonicalize(path);
    std::lock_guard<std::mutex> lock(mutex_);
    const bool removed = decisions_.erase(key) > 0;
    if (removed) {
        SaveLocked();
    }
    return removed;
}

std::map<std::string, bool> TrustStore::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return decisions_;
}

void TrustStore::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    decisions_.clear();
    SaveLocked();
}

std::size_t TrustStore::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return decisions_.size();
}

}  // namespace aiagent::permissions
