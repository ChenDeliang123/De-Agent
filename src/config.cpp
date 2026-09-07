#include "aiagent/config.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

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

bool SetSection(const std::string& line, std::string& section) {
    if (line.size() < 2 || line.front() != '[' || line.back() != ']') {
        return false;
    }
    section = Trim(line.substr(1, line.size() - 2));
    return true;
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
