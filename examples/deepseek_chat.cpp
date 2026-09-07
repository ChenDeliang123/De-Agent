#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "aiagent/config.hpp"
#include "aiagent/llm/deepseek_client.hpp"
#include "aiagent/llm/deepseek_types.hpp"

namespace {

void PrintUsage(const char* program) {
    std::cerr << "用法: " << program
              << " [--config <ini>] [--stream] \"你的问题\"\n";
}

std::optional<std::string> GetMessageText(const aiagent::llm::ChatResult& result) {
    if (result.choices.empty()) {
        return std::nullopt;
    }
    const std::string& content = result.choices.front().content;
    if (content.empty()) {
        return std::nullopt;
    }
    return content;
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "conf/conf.ini";
    std::string prompt;
    bool stream = false;

    if (const char* env_path = std::getenv("AIAGENT_CONFIG")) {
        config_path = env_path;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--stream") {
            stream = true;
        } else {
            prompt = arg;
        }
    }
    if (prompt.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::string error;
    const auto config = aiagent::LoadDeepSeekConfigFile(config_path, &error);
    if (!config.has_value()) {
        std::cerr << "配置错误: " << error << '\n';
        return 1;
    }

    aiagent::llm::ChatMessage user;
    user.role = "user";
    user.content = prompt;

    try {
        aiagent::llm::DeepSeekClient client(*config);
        if (stream) {
            client.ChatStream(
                    {user}, {},
                    [](const aiagent::llm::StreamEvent& event) {
                        if (!event.content.empty()) {
                            std::cout << event.content << std::flush;
                        }
                    });
            std::cout << '\n';
        } else {
            const auto result = client.Chat({user});
            const auto text = GetMessageText(result);
            if (text.has_value()) {
                std::cout << *text << '\n';
            } else {
                std::cerr << "DeepSeek 未返回文本内容\n";
                return 1;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "DeepSeek 调用失败: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
