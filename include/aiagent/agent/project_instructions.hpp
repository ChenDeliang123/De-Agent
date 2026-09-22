#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "aiagent/config.hpp"

namespace aiagent::agent {

// 一个已加载的项目指令文件。
struct InstructionFile {
    std::string path;
    std::string content;
    bool truncated = false;
};

// 渲染后的项目指令文本与加载元数据。
struct LoadedInstructions {
    std::string rendered;
    std::vector<InstructionFile> files;
    std::size_t total_bytes = 0;
    std::size_t cache_hits = 0;
    std::size_t cache_misses = 0;

    bool empty() const noexcept { return files.empty(); }
};

// 从工作目录向上发现 AGENTS.md，按“离 cwd 近者优先”的顺序加载，并渲染为
// 可注入 system prompt 的文本块。
//
// 读取失败、文件缺失、符号链接、二进制内容等情况不会抛异常，只跳过并记录日志。
class ProjectInstructionsLoader {
public:
    explicit ProjectInstructionsLoader(InstructionsConfig config = {});

    // working_directory 为空时使用 fallback_directory；两者都不可用时回退到
    // 进程当前目录。返回的 rendered 为空表示没有可注入的项目指令。
    LoadedInstructions Load(const std::string& working_directory,
                            const std::string& fallback_directory = {}) const;

    const InstructionsConfig& config() const noexcept { return config_; }

private:
    struct CachedFile;

    bool ReadFileCached(const std::filesystem::path& path, std::string* content, bool* binary,
                        bool* cache_hit, std::string* error) const;

    InstructionsConfig config_;
    mutable std::mutex cache_mutex_;
    mutable std::unordered_map<std::string, std::shared_ptr<const CachedFile>> cache_;
};

}  // namespace aiagent::agent
