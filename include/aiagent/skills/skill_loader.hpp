#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "aiagent/config.hpp"

namespace aiagent::skills {

// 技能来源只用于日志/API/CLI，不进入模型可见目录。
enum class SkillSource {
    kProject,
    kExtra,
    kUser,
    kBuiltin,
};

const char* SkillSourceName(SkillSource source);

struct SkillEntry {
    std::string name;
    std::string description;
    std::string path;  // SKILL.md 绝对路径（UTF-8）
    SkillSource source = SkillSource::kProject;
};

struct SkillCatalog {
    std::vector<SkillEntry> entries;
    bool truncated = false;
};

struct SkillContent {
    SkillEntry entry;
    std::string body;
    bool truncated = false;
    std::size_t bytes = 0;
};

// 技能发现与按需加载。
//
// 发现根优先级：项目 <project-root>/.de/skills > [skills] extra_roots >
// 用户 roots > 内置 roots。每个根只扫描直接子目录的 SKILL.md；同名技能
// 高优先级胜出。文件解析结果按 (mtime, size) 缓存，线程安全。
class SkillLoader {
public:
    SkillLoader(SkillsConfig config, std::vector<std::string> user_roots,
                std::vector<std::string> builtin_roots);
    ~SkillLoader();

    SkillLoader(const SkillLoader&) = delete;
    SkillLoader& operator=(const SkillLoader&) = delete;

    // working_directory 为空时使用进程当前目录。
    SkillCatalog LoadCatalog(const std::string& working_directory) const;

    // file 为空：加载 SKILL.md 正文；非空：读取技能目录内的资源文件。
    // 失败时返回 false 并写 error；技能名必须来自当前目录的目录结果。
    bool LoadContent(const std::string& name, const std::string& file,
                     const std::string& working_directory, SkillContent* content,
                     std::string* error) const;

    // 渲染为可追加到 system prompt 的文本；目录为空时返回空串。
    static std::string RenderCatalog(const SkillCatalog& catalog);

    const SkillsConfig& config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aiagent::skills
