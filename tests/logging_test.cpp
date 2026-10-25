#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "aiagent/config.hpp"
#include "aiagent/log.hpp"

#include "test_support.hpp"

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
                      ("aiagent_logging_" + std::to_string(test_support::Pid()) + ".log");
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

    // Windows 不允许删除仍被进程打开的文件；先关掉日志流。
    logger.Configure("off", "", false);
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
                      ("aiagent_logging_thread_" + std::to_string(test_support::Pid()) + ".log");
    aiagent::Logger logger;
    logger.Configure("info", path.string(), false);

    // 反复创建/销毁短生命周期线程：每条线程都会触达 logger 的 thread_local
    // request id。MinGW-w64 16.2 + UCRT 上，非平凡 thread_local 析构会在
    // 线程退出时损坏堆（0xc0000374），这里把线程退出次数放大到足以回归。
    constexpr int kRounds = 16;
    constexpr int kThreads = 4;
    constexpr int kLines = 50;
    for (int round = 0; round < kRounds; ++round) {
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&logger, t] {
                for (int i = 0; i < kLines; ++i) {
                    logger.Log(aiagent::LogLevel::kInfo, "thread",
                               "thread-" + std::to_string(t) + "-" +
                                       std::to_string(i));
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
    }

    std::size_t count = 0;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        ++count;
    }
    input.close();
    Expect(count == static_cast<std::size_t>(kRounds * kThreads * kLines),
           "all concurrent log lines should be written");
    // Windows 不允许删除仍被进程打开的文件；先关掉日志流。
    logger.Configure("off", "", false);
    std::filesystem::remove(path);
}

void TestRedaction() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("aiagent_logging_redact_" + std::to_string(test_support::Pid()) + ".log");
    aiagent::Logger logger;
    logger.Configure("info", path.string(), false);
    const std::string secret = "sk-1234567890abcdef1234567890abcdef";
    logger.Log(aiagent::LogLevel::kInfo, "test",
               "api_key=" + secret + " detail=ok");

    const std::string content = ReadAll(path);
    Expect(content.find("sk-***") != std::string::npos,
           "api key should be redacted");
    Expect(content.find(secret) == std::string::npos,
           "raw api key must not be written");
    // Windows 不允许删除仍被进程打开的文件；先关掉日志流。
    logger.Configure("off", "", false);
    std::filesystem::remove(path);
}

void TestRotation() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("aiagent_logging_rotate_" + std::to_string(test_support::Pid()) + ".log");
    aiagent::Logger logger;
    logger.Configure("info", path.string(), false, 80, 2);
    const std::string payload(200, 'x');
    for (int i = 0; i < 4; ++i) {
        logger.Log(aiagent::LogLevel::kInfo, "test",
                   "line-" + std::to_string(i) + " " + payload);
    }

    Expect(std::filesystem::exists(path.string() + ".1"),
           "rotated log file should exist");
    // Windows 不允许删除仍被进程打开的文件；先关掉日志流。
    logger.Configure("off", "", false);
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".1");
    std::filesystem::remove(path.string() + ".2");
}

void TestThreadRequestId() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("aiagent_logging_req_" + std::to_string(test_support::Pid()) + ".log");
    aiagent::Logger logger;
    logger.Configure("info", path.string(), false);
    aiagent::SetThreadRequestId("req-abc");
    logger.Log(aiagent::LogLevel::kInfo, "test", "event=hello");
    aiagent::ClearThreadRequestId();

    const std::string content = ReadAll(path);
    Expect(content.find("req=req-abc") != std::string::npos,
           "thread request id should be included");
    // Windows 不允许删除仍被进程打开的文件；先关掉日志流。
    logger.Configure("off", "", false);
    std::filesystem::remove(path);
}

}  // namespace

int main() {
    TestLevelAndFile();
    TestParseConfig();
    TestConcurrentWrites();
    TestRedaction();
    TestRotation();
    TestThreadRequestId();

    if (g_failures != 0) {
        std::cerr << "logging test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "logging test passed\n";
    return 0;
}
