#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace aiagent::review {

// `<project-root>/.de/review.json` 的一条构建/测试命令。只接受 argv，
// 不接受 shell 字符串。
struct ProjectVerifyCommand {
    std::string id;
    std::string kind = "test";  // build / test
    std::vector<std::string> argv;
    std::string cwd = ".";
    long timeout_s = 600;
};

struct ProjectVerifyConfig {
    int version = 1;
    std::string language;
    std::vector<std::string> detected_from;
    std::string approved_at;
    std::vector<std::string> test_globs;
    std::vector<ProjectVerifyCommand> commands;
    // 文件当前内容的 sha256（加载时计算）。
    std::string sha256;
};

struct VerifyDetectResult {
    bool found = false;
    std::string language;
    std::vector<std::string> detected_from;
    std::vector<ProjectVerifyCommand> commands;
    std::vector<std::string> test_globs;
};

struct ConsentState {
    // missing / valid / invalid
    std::string status = "missing";
    std::string approved_at;
    std::string config_sha256;
};

std::string ProjectVerifyConfigPath(const std::string& repo_root);

// 读取并校验；文件不存在返回 false 且 error 为空。
bool LoadProjectVerifyConfig(const std::string& repo_root, ProjectVerifyConfig* config,
                             std::string* error);

// 校验 schema/路径/argv/危险子命令；不写文件。
bool ValidateProjectVerifyConfig(const ProjectVerifyConfig& config,
                                 const std::string& repo_root, std::string* error);

// 原子写 `.de/review.json`，并确保 `.de/.gitignore` 至少包含 `*`（不存在时创建）。
bool WriteProjectVerifyConfig(const std::string& repo_root,
                              const ProjectVerifyConfig& config, std::string* error);

// 扫描常见 manifest，返回候选命令；不写文件。
VerifyDetectResult DetectProjectVerifyConfig(const std::string& repo_root);

// 用户级同意记录：`<data_dir>/review_consent.json`，键为项目根，值为
// {config_sha256, approved_at}。配置文件被改动后旧同意失效。
class ReviewConsentStore {
public:
    explicit ReviewConsentStore(std::string path = {});

    ConsentState Check(const std::string& repo_root,
                       const std::string& current_config_sha256) const;
    bool Approve(const std::string& repo_root, const std::string& config_sha256,
                 std::string* error = nullptr);

private:
    struct Entry {
        std::string config_sha256;
        std::string approved_at;
    };
    void LoadLocked();
    bool SaveLocked() const;

    std::string path_;
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
};

// pack 命令的固定 allowlist（argv[0] + 子命令）。
bool IsPackCommandAllowed(const std::vector<std::string>& argv);
// 项目命令：允许任意可执行文件，但拒绝安装/发布/部署类子命令。
bool IsForbiddenProjectCommand(const std::vector<std::string>& argv);

// 测试路径分类：目录段/文件名后缀/test_globs（相对仓库根，支持 * 与 **）。
bool IsTestPath(const std::string& relative_path,
                const std::vector<std::string>& test_globs);

// 判断 cwd 是否为仓库内相对路径（不要求存在）。
bool IsSafeRelativePath(const std::string& path, bool allow_current = true);

std::string NormalizeRelativePath(std::string path);

}  // namespace aiagent::review
