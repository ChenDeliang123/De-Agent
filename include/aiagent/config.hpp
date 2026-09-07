#pragma once

#include <optional>
#include <string>

namespace aiagent {

struct DeepSeekConfig {
    std::string api_key;
    std::string base_url = "https://api.deepseek.com";
    std::string model = "deepseek-v4-flash";
    std::string reasoning_model = "deepseek-v4-pro";
    long timeout_seconds = 60;
    int max_retries = 3;
};

// 解析 INI 文本中的 [deepseek] 段。api_key 缺失或为占位值时，
// 若环境变量 DEEPSEEK_API_KEY 存在则回退使用。
std::optional<DeepSeekConfig> ParseDeepSeekConfig(const std::string& content,
                                                  std::string* error = nullptr);

std::optional<DeepSeekConfig> LoadDeepSeekConfigFile(const std::string& path,
                                                     std::string* error = nullptr);

// 返回实际请求地址：base_url 已带 /chat/completions 时原样使用，否则补全。
std::string DeepSeekChatEndpoint(const DeepSeekConfig& config);

}  // namespace aiagent
