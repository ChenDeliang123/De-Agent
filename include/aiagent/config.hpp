#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace aiagent {

struct DeepSeekConfig {
    std::string api_key;
    std::string base_url = "https://api.deepseek.com";
    std::string model = "deepseek-v4-flash";
    std::string reasoning_model = "deepseek-v4-pro";
    long timeout_seconds = 60;
    int max_retries = 3;
};

struct MCPConfig {
    bool enabled = false;
    std::string binary;
    std::string plugins_dir;
    std::string python_path = "python3";
    std::string log_dir = "/tmp/mcp_logs";
    long request_timeout_seconds = 60;
};

struct LogConfig {
    std::string level = "info";
    std::string file;
    bool console = false;
    std::size_t max_size_bytes = 10 * 1024 * 1024;
    int max_files = 5;
};

// 上下文压缩配置。语义见 agent/compaction.hpp 的 CompactionSettings。
struct CompactionConfig {
    bool enabled = true;
    int context_window = 64000;
    int reserve_tokens = 16384;
    int keep_recent_tokens = 20000;
};

// 文件工具（read/write/edit）的访问沙箱配置（[tools]）。
// sandbox_enabled=true 时，解析后的路径必须落在本次请求的工作目录
// 或 sandbox_roots 的某个根目录之内；越界直接拒绝。
// 注意：bash 工具不受该策略约束。
struct ToolsConfig {
    bool sandbox_enabled = true;
    // 额外放行的根目录；相对路径相对工作目录解析。
    std::vector<std::string> sandbox_roots;
};

// 用户级数据目录配置（[storage]）。
struct StorageConfig {
    std::string data_dir;          // 空则按 DE_HOME/XDG_DATA_HOME/HOME 解析
    std::string permissions_file;  // 空则用 <data_dir>/permissions.json
};

// 权限模式配置（[permissions]）。
// 模式以字符串保存，由上层用 permissions::ParseMode 解析。
struct PermissionsConfig {
    std::string default_mode = "confirm";
    std::string max_mode = "full";
    bool allow_full_access = true;
};

// 终端 Agent 的项目指令（AGENTS.md）配置。
struct InstructionsConfig {
    bool agents_md = true;
    std::string agents_md_file_name = "AGENTS.md";
    bool agents_md_search_parents = true;
    std::size_t agents_md_max_files = 8;
    std::size_t agents_md_max_file_bytes = 16384;
    std::size_t agents_md_max_total_bytes = 32768;
    bool agents_md_follow_symlinks = false;
    std::size_t agents_md_max_depth = 16;
};

// 解析 INI 文本中的 [deepseek] 段。api_key 缺失或为占位值时，
// 若环境变量 DEEPSEEK_API_KEY 存在则回退使用。
std::optional<DeepSeekConfig> ParseDeepSeekConfig(const std::string& content,
                                                  std::string* error = nullptr);

std::optional<DeepSeekConfig> LoadDeepSeekConfigFile(const std::string& path,
                                                     std::string* error = nullptr);

// 解析 INI 文本中的 [mcp] 段；没有该段时返回 enabled=false。
std::optional<MCPConfig> ParseMCPConfig(const std::string& content,
                                        std::string* error = nullptr);

std::optional<MCPConfig> LoadMCPConfigFile(const std::string& path,
                                           std::string* error = nullptr);

// 解析 INI 文本中的 [logging] 段；没有该段时返回默认值。
std::optional<LogConfig> ParseLogConfig(const std::string& content,
                                        std::string* error = nullptr);

std::optional<LogConfig> LoadLogConfigFile(const std::string& path,
                                           std::string* error = nullptr);

// 解析 INI 文本中的 [compaction] 段；没有该段时返回默认值。
std::optional<CompactionConfig> ParseCompactionConfig(const std::string& content,
                                                      std::string* error = nullptr);

std::optional<CompactionConfig> LoadCompactionConfigFile(const std::string& path,
                                                         std::string* error = nullptr);

// 解析 INI 文本中的 [instructions] 段；没有该段时返回默认值。
// 解析 INI 文本中的 [tools] 段；没有该段时返回默认值（沙箱开启）。
std::optional<ToolsConfig> ParseToolsConfig(const std::string& content,
                                            std::string* error = nullptr);

std::optional<ToolsConfig> LoadToolsConfigFile(const std::string& path,
                                               std::string* error = nullptr);

// 解析 INI 文本中的 [storage] 段；没有该段时返回默认值。
std::optional<StorageConfig> ParseStorageConfig(const std::string& content,
                                                std::string* error = nullptr);

std::optional<StorageConfig> LoadStorageConfigFile(const std::string& path,
                                                   std::string* error = nullptr);

// 解析 INI 文本中的 [permissions] 段；没有该段时返回默认值。
std::optional<PermissionsConfig> ParsePermissionsConfig(
        const std::string& content,
        std::string* error = nullptr);

std::optional<PermissionsConfig> LoadPermissionsConfigFile(
        const std::string& path,
        std::string* error = nullptr);

std::optional<InstructionsConfig> ParseInstructionsConfig(
        const std::string& content,
        std::string* error = nullptr);

std::optional<InstructionsConfig> LoadInstructionsConfigFile(
        const std::string& path,
        std::string* error = nullptr);

// 返回实际请求地址：base_url 已带 /chat/completions 时原样使用，否则补全。
std::string DeepSeekChatEndpoint(const DeepSeekConfig& config);

}  // namespace aiagent
