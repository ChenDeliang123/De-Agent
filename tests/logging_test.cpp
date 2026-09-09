#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "aiagent/config.hpp"
#include "aiagent/log.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

void TestLevelAndFile() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("aiagent_logging_" + std::to_string(::getpid()) + ".log");
    aiagent::Logger logger;
    logger.Configure("debug", path.string(), false);
    logger.Log(aiagent::LogLevel::kTrace, "test", "should-not-appear");
    logger.Log(aiagent::LogLevel::kDebug, "test", "debug-message");
    logger.Log(aiagent::LogLevel::kInfo, "test", "info-message");

    const std::string content = ReadAll(path);
    Expect(content.find("should-not-appear") == std::string::npos,
           "trace should be filtered at debug level");
    Expect(content.find("debug-message") != std::string::npos,
           "debug message should be written");
    Expect(content.find("info-message") != std::string::npos,
           "info message should be written");
    Expect(content.find("[test]") != std::string::npos,
           "log line should contain the module");

    std::filesystem::remove(path);
}

void TestParseConfig() {
    std::string error;
    const auto config = aiagent::ParseLogConfig(
            "[logging]\nlevel = debug\nfile = .runtime/backend.log\nconsole = true\n",
            &error);
    Expect(config.has_value(), "logging config should parse");
    if (config.has_value()) {
        Expect(config->level == "debug", "logging level should parse");
        Expect(config->file == ".runtime/backend.log", "logging file should parse");
        Expect(config->console, "logging console flag should parse");
    }
}

void TestConcurrentWrites() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("aiagent_logging_thread_" + std::to_string(::getpid()) + ".log");
    aiagent::Logger logger;
    logger.Configure("info", path.string(), false);

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&logger, t] {
            for (int i = 0; i < 50; ++i) {
                logger.Log(aiagent::LogLevel::kInfo, "thread",
                           "thread-" + std::to_string(t) + "-" + std::to_string(i));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    std::size_t count = 0;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        ++count;
    }
    Expect(count == 200, "all concurrent log lines should be written");
    std::filesystem::remove(path);
}

}  // namespace

int main() {
    TestLevelAndFile();
    TestParseConfig();
    TestConcurrentWrites();

    if (g_failures != 0) {
        std::cerr << "logging test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "logging test passed\n";
    return 0;
}
