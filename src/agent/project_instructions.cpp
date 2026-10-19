#include "aiagent/agent/project_instructions.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "aiagent/log.hpp"
#include "aiagent/platform/paths.hpp"

namespace aiagent::agent {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kBinaryProbeBytes = 8192;

std::string JoinPaths(const std::vector<InstructionFile>& files) {
    std::string result;
    for (const auto& file : files) {
        if (!result.empty()) {
            result += ";";
        }
        result += file.path;
    }
    return result;
}

fs::path NormalizeDirectory(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    std::error_code ec;
    fs::path path = aiagent::platform::FromUtf8(value);
    if (!path.is_absolute()) {
        path = fs::absolute(path, ec);
        if (ec) {
            return {};
        }
    }

    const fs::path canonical = fs::weakly_canonical(path, ec);
    if (!ec) {
        path = canonical;
    }

    if (!fs::is_directory(path, ec) || ec) {
        return {};
    }
    return path;
}

fs::path ResolveWorkingDirectory(const std::string& requested, const std::string& fallback) {
    if (!requested.empty()) {
        if (const fs::path resolved = NormalizeDirectory(requested); !resolved.empty()) {
            return resolved;
        }
        aiagent::DefaultLogger().Log(aiagent::LogLevel::kWarning, "instructions",
                                     "event=instructions_cwd_invalid cwd=" + requested);
    }
    if (!fallback.empty()) {
        if (const fs::path resolved = NormalizeDirectory(fallback); !resolved.empty()) {
            return resolved;
        }
        aiagent::DefaultLogger().Log(aiagent::LogLevel::kWarning, "instructions",
                                     "event=instructions_fallback_invalid cwd=" + fallback);
    }

    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (ec) {
        return {};
    }
    return cwd;
}

std::vector<fs::path> CollectDirectories(const fs::path& start, const InstructionsConfig& config) {
    std::vector<fs::path> directories;
    fs::path current = start;
    for (std::size_t depth = 0;; ++depth) {
        directories.push_back(current);
        if (!config.agents_md_search_parents) {
            break;
        }

        std::error_code ec;
        const bool has_git_marker = fs::exists(current / ".git", ec) && !ec;
        if (has_git_marker) {
            break;
        }
        if (depth + 1 >= config.agents_md_max_depth) {
            break;
        }

        const fs::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
    }
    return directories;
}

bool ReadFile(const fs::path& path, std::string* content, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        *error = "cannot open file";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        *error = "failed while reading file";
        return false;
    }
    *content = buffer.str();
    return true;
}

std::string TruncateUtf8(const std::string& content, std::size_t limit) {
    if (limit >= content.size()) {
        return content;
    }
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(content[end]) & 0xC0) == 0x80) {
        --end;
    }
    return content.substr(0, end);
}

bool LooksBinary(const std::string& content) {
    const std::size_t probe = std::min(content.size(), kBinaryProbeBytes);
    return content.substr(0, probe).find('\0') != std::string::npos;
}

std::string RenderInstructions(const LoadedInstructions& loaded, const std::string& file_name) {
    std::ostringstream output;
    output << "<project-instructions>\n"
           << "These instructions are loaded from " << file_name
           << " files for the current project.\n"
           << "They are scoped to the current working directory. Follow them "
              "for project work.\n"
           << "If they conflict with higher-level system or safety rules, the "
              "higher-level rules win.\n"
           << "Files are ordered from lowest to highest priority.\n";

    for (const auto& file : loaded.files) {
        output << "\n----- file: " << file.path << " -----\n";
        output << file.content;
        if (!file.content.empty() && file.content.back() != '\n') {
            output << '\n';
        }
    }
    output << "</project-instructions>";
    return output.str();
}

}  // namespace

struct ProjectInstructionsLoader::CachedFile {
    std::filesystem::file_time_type mtime;
    std::uintmax_t size = 0;
    std::string content;
    bool binary = false;
};

ProjectInstructionsLoader::ProjectInstructionsLoader(InstructionsConfig config)
    : config_(std::move(config)) {}

bool ProjectInstructionsLoader::ReadFileCached(const fs::path& path, std::string* content,
                                               bool* binary, bool* cache_hit,
                                               std::string* error) const {
    std::error_code ec;
    const fs::file_time_type mtime = fs::last_write_time(path, ec);
    if (ec) {
        *error = "cannot stat file";
        return false;
    }
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        *error = "cannot stat file size";
        return false;
    }

    const std::string key = aiagent::platform::ToUtf8(path);
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        const auto it = cache_.find(key);
        if (it != cache_.end() && it->second->mtime == mtime && it->second->size == size) {
            *content = it->second->content;
            *binary = it->second->binary;
            *cache_hit = true;
            return true;
        }
    }

    if (!ReadFile(path, content, error)) {
        return false;
    }
    *binary = LooksBinary(*content);
    *cache_hit = false;

    auto cached = std::make_shared<CachedFile>();
    cached->mtime = mtime;
    cached->size = size;
    cached->content = *content;
    cached->binary = *binary;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        cache_[key] = std::move(cached);
    }
    return true;
}

LoadedInstructions ProjectInstructionsLoader::Load(const std::string& working_directory,
                                                   const std::string& fallback_directory) const {
    LoadedInstructions loaded;
    if (!config_.agents_md) {
        return loaded;
    }

    const fs::path start = ResolveWorkingDirectory(working_directory, fallback_directory);
    if (start.empty()) {
        return loaded;
    }

    const std::vector<fs::path> directories = CollectDirectories(start, config_);
    std::vector<InstructionFile> files;
    files.reserve(std::min(directories.size(), config_.agents_md_max_files));
    std::size_t used_bytes = 0;

    for (const auto& directory : directories) {
        if (files.size() >= config_.agents_md_max_files) {
            break;
        }

        const fs::path path = directory / config_.agents_md_file_name;
        std::error_code ec;
        const fs::file_status symlink_status = fs::symlink_status(path, ec);
        if (ec || !fs::exists(symlink_status)) {
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kDebug, "instructions",
                    "event=instructions_skip path=" +
                            aiagent::platform::ToUtf8(path) + " reason=missing");
            continue;
        }
        if (fs::is_symlink(symlink_status) && !config_.agents_md_follow_symlinks) {
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kWarning, "instructions",
                    "event=instructions_skip path=" +
                            aiagent::platform::ToUtf8(path) + " reason=symlink");
            continue;
        }

        const fs::file_status status =
                fs::is_symlink(symlink_status) ? fs::status(path, ec) : symlink_status;
        if (ec || !fs::is_regular_file(status)) {
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kWarning, "instructions",
                    "event=instructions_skip path=" +
                            aiagent::platform::ToUtf8(path) +
                            " reason=not_regular_file");
            continue;
        }

        std::string content;
        bool binary = false;
        bool cache_hit = false;
        std::string read_error;
        if (!ReadFileCached(path, &content, &binary, &cache_hit, &read_error)) {
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kWarning, "instructions",
                    "event=instructions_skip path=" +
                            aiagent::platform::ToUtf8(path) +
                            " reason=read_error error=" + read_error);
            continue;
        }
        if (cache_hit) {
            ++loaded.cache_hits;
        } else {
            ++loaded.cache_misses;
        }
        if (binary) {
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kWarning, "instructions",
                    "event=instructions_skip path=" +
                            aiagent::platform::ToUtf8(path) + " reason=binary");
            continue;
        }
        if (content.empty()) {
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kDebug, "instructions",
                    "event=instructions_skip path=" +
                            aiagent::platform::ToUtf8(path) + " reason=empty");
            continue;
        }

        const std::size_t remaining = used_bytes >= config_.agents_md_max_total_bytes
                                              ? 0
                                              : config_.agents_md_max_total_bytes - used_bytes;
        if (remaining == 0) {
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kWarning, "instructions",
                    "event=instructions_skip path=" +
                            aiagent::platform::ToUtf8(path) +
                            " reason=total_bytes_limit");
            continue;
        }

        const std::size_t limit = std::min(config_.agents_md_max_file_bytes, remaining);
        bool truncated = false;
        if (content.size() > limit) {
            const std::size_t original_size = content.size();
            content = TruncateUtf8(content, limit);
            truncated = true;
            aiagent::DefaultLogger().Log(aiagent::LogLevel::kWarning, "instructions",
                                         "event=instructions_truncate path=" + aiagent::platform::ToUtf8(path) +
                                                 " limit=" + std::to_string(limit) +
                                                 " actual=" + std::to_string(original_size));
            content += "\n[... truncated: " + std::to_string(original_size - content.size()) +
                       " bytes omitted]";
        }
        if (content.empty()) {
            continue;
        }

        used_bytes += content.size();
        files.push_back(InstructionFile{aiagent::platform::ToUtf8(path), content, truncated});
    }

    if (files.empty()) {
        return loaded;
    }

    // 上面按“cwd 向根”收集，渲染前反转为“根向 cwd”，让靠近 cwd 的文件
    // 出现在后面，语义上拥有更高优先级。
    std::reverse(files.begin(), files.end());
    loaded.files = std::move(files);
    loaded.total_bytes = used_bytes;
    loaded.rendered = RenderInstructions(loaded, config_.agents_md_file_name);

    aiagent::DefaultLogger().Log(aiagent::LogLevel::kInfo, "instructions",
                                 "event=instructions_load cwd=" + aiagent::platform::ToUtf8(start) +
                                         " files=" + std::to_string(loaded.files.size()) +
                                         " bytes=" + std::to_string(loaded.total_bytes) +
                                         " cache_hits=" + std::to_string(loaded.cache_hits) +
                                         " cache_misses=" + std::to_string(loaded.cache_misses) +
                                         " sources=" + JoinPaths(loaded.files));
    return loaded;
}

}  // namespace aiagent::agent
