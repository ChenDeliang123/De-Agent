#include <iostream>
#include <string>
#include <vector>

#include "aiagent/agent/compaction.hpp"
#include "aiagent/config.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

aiagent::llm::ChatMessage TextMessage(const std::string& role,
                                      const std::string& content) {
    aiagent::llm::ChatMessage message;
    message.role = role;
    message.content = content;
    return message;
}

void TestEstimateTokens() {
    using aiagent::agent::EstimateMessagesTokens;
    using aiagent::agent::EstimateTokens;

    aiagent::llm::ChatMessage empty;
    empty.role = "user";
    // 空内容仍有角色/分隔符固定开销。
    Expect(EstimateTokens(empty) == 4, "empty message should cost overhead only");

    aiagent::llm::ChatMessage ascii = TextMessage("user", "hello world");
    Expect(EstimateTokens(ascii) > 4, "ascii content should add tokens");
    Expect(EstimateTokens(ascii) < 12, "short ascii content should stay small");

    aiagent::llm::ChatMessage chinese = TextMessage("user", "你好世界");
    // 中文 4 字 = 12 字节，按字节/3 约 4 token + 4 开销。
    Expect(EstimateTokens(chinese) >= 8, "chinese text should be estimated conservatively");

    aiagent::llm::ChatMessage with_tool;
    with_tool.role = "assistant";
    with_tool.tool_calls.push_back(
            aiagent::llm::ToolCall{"call-1", "read", R"({"path":"a.cpp"})"});
    Expect(EstimateTokens(with_tool) > EstimateTokens(empty),
           "tool call arguments should count toward tokens");

    const int total = EstimateMessagesTokens({ascii, chinese});
    Expect(total == EstimateTokens(ascii) + EstimateTokens(chinese),
           "message list estimate should sum individual estimates");
}

void TestEstimateContextTokens() {
    using aiagent::agent::EstimateContextTokens;

    const std::vector<aiagent::llm::ChatMessage> messages = {
            TextMessage("user", "hello"),
            TextMessage("assistant", "world"),
    };

    // 无 usage 时全部估算。
    const auto no_usage = EstimateContextTokens(messages, 0, 0);
    Expect(!no_usage.has_usage, "estimate without usage should not report usage");
    Expect(no_usage.usage_tokens == 0, "usage tokens should be zero without usage");
    Expect(no_usage.trailing_tokens > 0, "all messages should be estimated as trailing");
    Expect(no_usage.tokens == no_usage.trailing_tokens,
           "total should equal trailing when usage is absent");

    // 有 usage：usage 覆盖前一条，尾部的 assistant 再估算。
    const auto with_usage = EstimateContextTokens(messages, 1234, 1);
    Expect(with_usage.has_usage, "estimate with usage should report usage");
    Expect(with_usage.usage_tokens == 1234, "usage tokens should be preserved");
    Expect(with_usage.trailing_tokens > 0, "messages after usage should be estimated");
    Expect(with_usage.tokens == 1234 + with_usage.trailing_tokens,
           "total should be usage plus trailing");

    // usage 条数超过消息数时退回纯估算，避免负索引。
    const auto stale = EstimateContextTokens(messages, 999, 10);
    Expect(!stale.has_usage, "stale usage should be ignored");
    Expect(stale.tokens == no_usage.tokens, "stale usage should fall back to full estimate");
}

void TestShouldCompact() {
    using aiagent::agent::CompactionSettings;
    using aiagent::agent::ShouldCompact;

    CompactionSettings settings;
    settings.enabled = true;
    settings.context_window = 1000;
    settings.reserve_tokens = 200;
    settings.keep_recent_tokens = 300;

    Expect(!ShouldCompact(799, settings), "below threshold should not compact");
    Expect(!ShouldCompact(800, settings), "equal to threshold should not compact");
    Expect(ShouldCompact(801, settings), "above threshold should compact");

    settings.enabled = false;
    Expect(!ShouldCompact(100000, settings), "disabled compaction should never trigger");

    settings.enabled = true;
    settings.context_window = 0;
    Expect(!ShouldCompact(100000, settings), "zero context window should be ignored");
}

void TestCompactionConfig() {
    const std::string ini = R"INI(
[compaction]
enabled = false
context_window = 32000
reserve_tokens = 4096
keep_recent_tokens = 8000
)INI";

    std::string error;
    const auto config = aiagent::ParseCompactionConfig(ini, &error);
    Expect(config.has_value(), "compaction config should parse");
    if (config.has_value()) {
        Expect(!config->enabled, "enabled should parse false");
        Expect(config->context_window == 32000, "context_window should parse");
        Expect(config->reserve_tokens == 4096, "reserve_tokens should parse");
        Expect(config->keep_recent_tokens == 8000, "keep_recent_tokens should parse");
    }

    // 没有 [compaction] 段时返回默认值。
    const auto defaults = aiagent::ParseCompactionConfig("[deepseek]\nmodel = x\n", &error);
    Expect(defaults.has_value(), "missing section should return defaults");
    if (defaults.has_value()) {
        Expect(defaults->enabled, "default enabled should be true");
        Expect(defaults->context_window == 64000, "default context_window");
        Expect(defaults->reserve_tokens == 16384, "default reserve_tokens");
        Expect(defaults->keep_recent_tokens == 20000, "default keep_recent_tokens");
    }

    // 非法值应报错。
    const auto bad = aiagent::ParseCompactionConfig(
            "[compaction]\ncontext_window = -1\n", &error);
    Expect(!bad.has_value(), "negative context_window should fail");

    const auto bad_number = aiagent::ParseCompactionConfig(
            "[compaction]\nreserve_tokens = abc\n", &error);
    Expect(!bad_number.has_value(), "non-numeric reserve_tokens should fail");
}

}  // namespace

int main() {
    TestEstimateTokens();
    TestEstimateContextTokens();
    TestShouldCompact();
    TestCompactionConfig();

    if (g_failures != 0) {
        std::cerr << "compaction test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "compaction test passed\n";
    return 0;
}
