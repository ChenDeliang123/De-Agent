#include "aiagent/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace aiagent {
namespace {

std::string Trim(std::string value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

std::string Lower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

bool SetSection(const std::string& line, std::string& section) {
    if (line.size() < 2 || line.front() != '[' || line.back() != ']') {
        return false;
    }
    section = Trim(line.substr(1, line.size() - 2));
    return true;
}

// "a, b , c" -> {"a", "b", "c"}（逗号分隔，忽略空白与空项）。
std::vector<std::string> SplitList(const std::string& value) {
    std::vector<std::string> items;
    std::string current;
    for (const char c : value) {
        if (c == ',') {
            const std::string item = Trim(current);
            if (!item.empty()) {
                items.push_back(item);
            }
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    const std::string item = Trim(current);
    if (!item.empty()) {
        items.push_back(item);
    }
    return items;
}

}  // namespace

std::optional<DeepSeekConfig> ParseDeepSeekConfig(const std::string& content,
                                                  std::string* error) {
    DeepSeekConfig config;
    bool in_deepseek = false;
    bool saw_deepseek_section = false;

    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }

        std::string section;
        if (SetSection(trimmed, section)) {
            in_deepseek = (section == "deepseek");
            saw_deepseek_section = saw_deepseek_section || in_deepseek;
            continue;
        }
        if (!in_deepseek) {
            continue;
        }

        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(trimmed.substr(0, eq));
        const std::string value = Trim(trimmed.substr(eq + 1));

        if (key == "api_key") {
            config.api_key = value;
        } else if (key == "base_url") {
            config.base_url = value;
        } else if (key == "model") {
            config.model = value;
        } else if (key == "reasoning_model") {
            config.reasoning_model = value;
        } else if (key == "timeout_seconds") {
            try {
                const long parsed = std::stol(value);
                if (parsed <= 0) {
                    if (error != nullptr) {
                        *error = "timeout_seconds must be a positive integer";
                    }
                    return std::nullopt;
                }
                config.timeout_seconds = parsed;
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "invalid timeout_seconds: " + value;
                }
                return std::nullopt;
            }
        } else if (key == "max_retries") {
            try {
                const int parsed = std::stoi(value);
                if (parsed < 0) {
                    if (error != nullptr) {
                        *error = "max_retries must be non-negative";
                    }
                    return std::nullopt;
                }
                config.max_retries = parsed;
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "invalid max_retries: " + value;
                }
                return std::nullopt;
            }
        }
    }

    if (!saw_deepseek_section) {
        if (error != nullptr) {
            *error = "missing [deepseek] section";
        }
        return std::nullopt;
    }

    const char* env_key = std::getenv("DEEPSEEK_API_KEY");
    if ((config.api_key.empty() || config.api_key == "your-deepseek-api-key") &&
        env_key != nullptr && *env_key != '\0') {
        config.api_key = env_key;
    }
    if (config.api_key.empty() || config.api_key == "your-deepseek-api-key") {
        if (error != nullptr) {
            *error = "missing api_key; set it in conf/conf.ini or DEEPSEEK_API_KEY";
        }
        return std::nullopt;
    }
    if (config.base_url.empty() || config.model.empty()) {
        if (error != nullptr) {
            *error = "base_url/model must not be empty";
        }
        return std::nullopt;
    }
    return config;
}

std::optional<DeepSeekConfig> LoadDeepSeekConfigFile(const std::string& path,
                                                     std::string* error) {
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open config file: " + path;
        }
        return std::nullopt;
    }
    std::ostringstream content;
    content << input.rdbuf();
    return ParseDeepSeekConfig(content.str(), error);
}

std::optional<MCPConfig> ParseMCPConfig(const std::string& content, std::string* error) {
    MCPConfig config;
    bool in_mcp = false;
    bool saw_mcp_section = false;

    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }

        std::string section;
        if (SetSection(trimmed, section)) {
            in_mcp = (section == "mcp");
            saw_mcp_section = saw_mcp_section || in_mcp;
            continue;
        }
        if (!in_mcp) {
            continue;
        }

        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(trimmed.substr(0, eq));
        const std::string value = Trim(trimmed.substr(eq + 1));

        if (key == "binary") {
            config.binary = value;
        } else if (key == "plugins") {
            config.plugins_dir = value;
        } else if (key == "python_path") {
            config.python_path = value;
        } else if (key == "log_dir") {
            config.log_dir = value;
        } else if (key == "request_timeout_seconds") {
            try {
                const long parsed = std::stol(value);
                if (parsed <= 0) {
                    if (error != nullptr) {
                        *error = "request_timeout_seconds must be positive";
                    }
                    return std::nullopt;
                }
                config.request_timeout_seconds = parsed;
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "invalid request_timeout_seconds: " + value;
                }
                return std::nullopt;
            }
        }
    }

    if (!saw_mcp_section) {
        return config;
    }

    const char* env_binary = std::getenv("AIAGENT_MCP_BINARY");
    if ((config.binary.empty() || config.binary == "/absolute/path/to/mcp_server") &&
        env_binary != nullptr && *env_binary != '\0') {
        config.binary = env_binary;
    }
    const char* env_plugins = std::getenv("AIAGENT_MCP_PLUGINS");
    if (config.plugins_dir.empty() && env_plugins != nullptr && *env_plugins != '\0') {
        config.plugins_dir = env_plugins;
    }

    if (config.binary.empty() || config.binary == "/absolute/path/to/mcp_server") {
        config.enabled = false;
        return config;
    }
    config.enabled = true;
    return config;
}

std::optional<MCPConfig> LoadMCPConfigFile(const std::string& path, std::string* error) {
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open config file: " + path;
        }
        return std::nullopt;
    }
    std::ostringstream content;
    content << input.rdbuf();
    return ParseMCPConfig(content.str(), error);
}

std::optional<LogConfig> ParseLogConfig(const std::string& content, std::string* error) {
    LogConfig config;
    bool in_logging = false;
    bool saw_logging_section = false;

    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }

        std::string section;
        if (SetSection(trimmed, section)) {
            in_logging = (section == "logging");
            saw_logging_section = saw_logging_section || in_logging;
            continue;
        }
        if (!in_logging) {
            continue;
        }

        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(trimmed.substr(0, eq));
        const std::string value = Trim(trimmed.substr(eq + 1));

        if (key == "level") {
            config.level = value;
        } else if (key == "file") {
            config.file = value;
        } else if (key == "console") {
            config.console = Lower(value) == "true" || value == "1";
        } else if (key == "max_size_bytes") {
            try {
                const unsigned long long parsed = std::stoull(value);
                config.max_size_bytes = static_cast<std::size_t>(parsed);
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "invalid max_size_bytes: " + value;
                }
                return std::nullopt;
            }
        } else if (key == "max_files") {
            try {
                config.max_files = std::stoi(value);
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "invalid max_files: " + value;
                }
                return std::nullopt;
            }
        }
    }

    if (!saw_logging_section) {
        if (error != nullptr) {
            *error = "missing [logging] section; using defaults";
        }
        return config;
    }
    return config;
}

std::optional<LogConfig> LoadLogConfigFile(const std::string& path, std::string* error) {
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open config file: " + path;
        }
        return std::nullopt;
    }
    std::ostringstream content;
    content << input.rdbuf();
    return ParseLogConfig(content.str(), error);
}

std::optional<CompactionConfig> ParseCompactionConfig(const std::string& content,
                                                      std::string* error) {
    CompactionConfig config;
    bool in_compaction = false;

    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }

        std::string section;
        if (SetSection(trimmed, section)) {
            in_compaction = (section == "compaction");
            continue;
        }
        if (!in_compaction) {
            continue;
        }

        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(trimmed.substr(0, eq));
        const std::string value = Trim(trimmed.substr(eq + 1));

        if (key == "enabled") {
            config.enabled = Lower(value) == "true" || value == "1";
        } else if (key == "context_window") {
            try {
                const int parsed = std::stoi(value);
                if (parsed <= 0) {
                    if (error != nullptr) {
                        *error = "context_window must be positive";
                    }
                    return std::nullopt;
                }
                config.context_window = parsed;
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "invalid context_window: " + value;
                }
                return std::nullopt;
            }
        } else if (key == "reserve_tokens") {
            try {
                const int parsed = std::stoi(value);
                if (parsed < 0) {
                    if (error != nullptr) {
                        *error = "reserve_tokens must be non-negative";
                    }
                    return std::nullopt;
                }
                config.reserve_tokens = parsed;
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "invalid reserve_tokens: " + value;
                }
                return std::nullopt;
            }
        } else if (key == "keep_recent_tokens") {
            try {
                const int parsed = std::stoi(value);
                if (parsed < 0) {
                    if (error != nullptr) {
                        *error = "keep_recent_tokens must be non-negative";
                    }
                    return std::nullopt;
                }
                config.keep_recent_tokens = parsed;
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "invalid keep_recent_tokens: " + value;
                }
                return std::nullopt;
            }
        } else if (key == "compact_at_tokens") {
            try {
                const int parsed = std::stoi(value);
                if (parsed < 0) {
                    if (error != nullptr) {
                        *error = "compact_at_tokens must be non-negative";
                    }
                    return std::nullopt;
                }
                config.compact_at_tokens = parsed;
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "invalid compact_at_tokens: " + value;
                }
                return std::nullopt;
            }
        } else if (key == "min_gain_ratio") {
            try {
                const double parsed = std::stod(value);
                if (parsed < 0.0 || parsed > 1.0) {
                    if (error != nullptr) {
                        *error = "min_gain_ratio must be within [0, 1]";
                    }
                    return std::nullopt;
                }
                config.min_gain_ratio = parsed;
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "invalid min_gain_ratio: " + value;
                }
                return std::nullopt;
            }
        }
    }

    return config;
}

std::optional<CompactionConfig> LoadCompactionConfigFile(const std::string& path,
                                                         std::string* error) {
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open config file: " + path;
        }
        return std::nullopt;
    }
    std::ostringstream content;
    content << input.rdbuf();
    return ParseCompactionConfig(content.str(), error);
}

std::optional<InstructionsConfig> ParseInstructionsConfig(const std::string& content,
                                                          std::string* error) {
    InstructionsConfig config;
    bool in_instructions = false;

    auto parse_size = [error](const std::string& key, const std::string& value,
                              std::size_t* output, bool allow_zero) {
        if (!value.empty() && value.front() == '-') {
            if (error != nullptr) {
                *error = key + " must be non-negative";
            }
            return false;
        }
        try {
            const unsigned long long parsed = std::stoull(value);
            if (!allow_zero && parsed == 0) {
                if (error != nullptr) {
                    *error = key + " must be positive";
                }
                return false;
            }
            *output = static_cast<std::size_t>(parsed);
            return true;
        } catch (const std::exception&) {
            if (error != nullptr) {
                *error = "invalid " + key + ": " + value;
            }
            return false;
        }
    };

    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }

        std::string section;
        if (SetSection(trimmed, section)) {
            in_instructions = (section == "instructions");
            continue;
        }
        if (!in_instructions) {
            continue;
        }

        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(trimmed.substr(0, eq));
        const std::string value = Trim(trimmed.substr(eq + 1));

        if (key == "agents_md") {
            config.agents_md = Lower(value) == "true" || value == "1";
        } else if (key == "agents_md_file_name") {
            if (value.empty() || value.find('/') != std::string::npos ||
                value.find('\\') != std::string::npos) {
                if (error != nullptr) {
                    *error = "agents_md_file_name must be a plain file name";
                }
                return std::nullopt;
            }
            config.agents_md_file_name = value;
        } else if (key == "agents_md_search_parents") {
            config.agents_md_search_parents =
                    Lower(value) == "true" || value == "1";
        } else if (key == "agents_md_max_files") {
            if (!parse_size(key, value, &config.agents_md_max_files, false)) {
                return std::nullopt;
            }
        } else if (key == "agents_md_max_file_bytes") {
            if (!parse_size(key, value, &config.agents_md_max_file_bytes, false)) {
                return std::nullopt;
            }
        } else if (key == "agents_md_max_total_bytes") {
            if (!parse_size(key, value, &config.agents_md_max_total_bytes, false)) {
                return std::nullopt;
            }
        } else if (key == "agents_md_follow_symlinks") {
            config.agents_md_follow_symlinks =
                    Lower(value) == "true" || value == "1";
        } else if (key == "agents_md_max_depth") {
            if (!parse_size(key, value, &config.agents_md_max_depth, false)) {
                return std::nullopt;
            }
        }
    }

    return config;
}

std::optional<InstructionsConfig> LoadInstructionsConfigFile(const std::string& path,
                                                             std::string* error) {
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open config file: " + path;
        }
        return std::nullopt;
    }
    std::ostringstream content;
    content << input.rdbuf();
    return ParseInstructionsConfig(content.str(), error);
}

std::optional<ToolsConfig> ParseToolsConfig(const std::string& content,
                                            std::string* error) {
    (void)error;
    ToolsConfig config;
    bool in_tools = false;

    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }

        std::string section;
        if (SetSection(trimmed, section)) {
            in_tools = (section == "tools");
            continue;
        }
        if (!in_tools) {
            continue;
        }

        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(trimmed.substr(0, eq));
        const std::string value = Trim(trimmed.substr(eq + 1));

        if (key == "sandbox_enabled") {
            config.sandbox_enabled = Lower(value) == "true" || value == "1";
        } else if (key == "sandbox_roots") {
            // 允许逗号分隔，且允许多次出现以追加。
            std::size_t start = 0;
            while (start <= value.size()) {
                const std::size_t comma = value.find(',', start);
                const std::string item = Trim(value.substr(
                        start, comma == std::string::npos ? std::string::npos
                                                          : comma - start));
                if (!item.empty()) {
                    config.sandbox_roots.push_back(item);
                }
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
        }
    }

    return config;
}

std::optional<ToolsConfig> LoadToolsConfigFile(const std::string& path,
                                               std::string* error) {
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open config file: " + path;
        }
        return std::nullopt;
    }
    std::ostringstream content;
    content << input.rdbuf();
    return ParseToolsConfig(content.str(), error);
}

std::optional<StorageConfig> ParseStorageConfig(const std::string& content,
                                                std::string* error) {
    (void)error;
    StorageConfig config;
    bool in_storage = false;

    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }
        std::string section;
        if (SetSection(trimmed, section)) {
            in_storage = (section == "storage");
            continue;
        }
        if (!in_storage) {
            continue;
        }
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(trimmed.substr(0, eq));
        const std::string value = Trim(trimmed.substr(eq + 1));
        if (key == "data_dir") {
            config.data_dir = value;
        } else if (key == "permissions_file") {
            config.permissions_file = value;
        } else if (key == "bash_policy_file") {
            config.bash_policy_file = value;
        } else if (key == "trust_file") {
            config.trust_file = value;
        }
    }
    return config;
}

std::optional<StorageConfig> LoadStorageConfigFile(const std::string& path,
                                                   std::string* error) {
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open config file: " + path;
        }
        return std::nullopt;
    }
    std::ostringstream content;
    content << input.rdbuf();
    return ParseStorageConfig(content.str(), error);
}

std::optional<PermissionsConfig> ParsePermissionsConfig(const std::string& content,
                                                        std::string* error) {
    (void)error;
    PermissionsConfig config;
    bool in_permissions = false;

    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }
        std::string section;
        if (SetSection(trimmed, section)) {
            in_permissions = (section == "permissions");
            continue;
        }
        if (!in_permissions) {
            continue;
        }
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(trimmed.substr(0, eq));
        const std::string value = Trim(trimmed.substr(eq + 1));
        if (key == "default_mode") {
            config.default_mode = value;
        } else if (key == "max_mode") {
            config.max_mode = value;
        } else if (key == "non_interactive_max_mode") {
            config.non_interactive_max_mode = value;
        } else if (key == "allow_full_access") {
            config.allow_full_access = Lower(value) == "true" || value == "1";
        } else if (key == "approval_timeout_seconds") {
            try {
                const int parsed = std::stoi(value);
                if (parsed > 0) {
                    config.approval_timeout_seconds = parsed;
                }
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "invalid approval_timeout_seconds: " + value;
                }
                return std::nullopt;
            }
        }
    }
    return config;
}

std::optional<PermissionsConfig> LoadPermissionsConfigFile(const std::string& path,
                                                           std::string* error) {
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open config file: " + path;
        }
        return std::nullopt;
    }
    std::ostringstream content;
    content << input.rdbuf();
    return ParsePermissionsConfig(content.str(), error);
}

std::optional<AgentsConfig> ParseAgentsConfig(const std::string& content, std::string* error) {
    AgentsConfig config;
    bool in_agents = false;
    AgentDefinitionConfig* current = nullptr;

    auto parse_int = [error](const std::string& key, const std::string& value, long minimum,
                            long maximum, long* output) {
        try {
            const long parsed = std::stol(value);
            if (parsed < minimum || parsed > maximum) {
                if (error != nullptr) {
                    *error = key + " must be within [" + std::to_string(minimum) + ", " +
                             std::to_string(maximum) + "]";
                }
                return false;
            }
            *output = parsed;
            return true;
        } catch (const std::exception&) {
            if (error != nullptr) {
                *error = "invalid " + key + ": " + value;
            }
            return false;
        }
    };

    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }

        std::string section;
        if (SetSection(trimmed, section)) {
            in_agents = (section == "agents");
            current = nullptr;
            constexpr const char* kPrefix = "agents.";
            if (section.rfind(kPrefix, 0) == 0) {
                const std::string name = Trim(section.substr(std::string(kPrefix).size()));
                if (name.empty()) {
                    if (error != nullptr) {
                        *error = "[agents.<name>] requires a non-empty name";
                    }
                    return std::nullopt;
                }
                auto it = std::find_if(config.agents.begin(), config.agents.end(),
                                       [&name](const AgentDefinitionConfig& definition) {
                                           return definition.name == name;
                                       });
                if (it == config.agents.end()) {
                    AgentDefinitionConfig definition;
                    definition.name = name;
                    config.agents.push_back(std::move(definition));
                    current = &config.agents.back();
                } else {
                    current = &*it;
                }
                in_agents = true;
            }
            continue;
        }
        if (!in_agents) {
            continue;
        }
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(trimmed.substr(0, eq));
        const std::string value = Trim(trimmed.substr(eq + 1));

        if (current != nullptr) {
            if (key == "description") {
                current->description = value;
            } else if (key == "system_prompt_file" || key == "prompt_file") {
                current->system_prompt_file = value;
            } else if (key == "model") {
                current->model = value;
            } else if (key == "tools") {
                current->tools = SplitList(value);
            }
            continue;
        }

        long parsed = 0;
        if (key == "enabled") {
            config.enabled = Lower(value) == "true" || value == "1";
        } else if (key == "max_depth") {
            if (!parse_int(key, value, 0, 4, &parsed)) {
                return std::nullopt;
            }
            config.max_depth = static_cast<int>(parsed);
        } else if (key == "max_concurrent") {
            if (!parse_int(key, value, 1, 16, &parsed)) {
                return std::nullopt;
            }
            config.max_concurrent = static_cast<int>(parsed);
        } else if (key == "max_background") {
            if (!parse_int(key, value, 0, 16, &parsed)) {
                return std::nullopt;
            }
            config.max_background = static_cast<int>(parsed);
        } else if (key == "default_timeout_s") {
            if (!parse_int(key, value, 1, 3600, &parsed)) {
                return std::nullopt;
            }
            config.default_timeout_s = parsed;
        } else if (key == "max_rounds") {
            if (!parse_int(key, value, 1, 1000, &parsed)) {
                return std::nullopt;
            }
            config.max_rounds = static_cast<int>(parsed);
        } else if (key == "result_max_chars") {
            if (!parse_int(key, value, 100, 1000000, &parsed)) {
                return std::nullopt;
            }
            config.result_max_chars = static_cast<std::size_t>(parsed);
        } else if (key == "stream_events") {
            config.stream_events = Lower(value) == "true" || value == "1";
        } else if (key == "default_model") {
            config.default_model = value;
        }
    }
    return config;
}

std::optional<AgentsConfig> LoadAgentsConfigFile(const std::string& path, std::string* error) {
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open config file: " + path;
        }
        return std::nullopt;
    }
    std::ostringstream content;
    content << input.rdbuf();
    return ParseAgentsConfig(content.str(), error);
}

std::string DeepSeekChatEndpoint(const DeepSeekConfig& config) {
    constexpr const char* kSuffix = "/chat/completions";
    if (config.base_url.size() >= 18 &&
        config.base_url.compare(config.base_url.size() - 18, 18, kSuffix) == 0) {
        return config.base_url;
    }
    std::string url = config.base_url;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url + kSuffix;
}

}  // namespace aiagent
