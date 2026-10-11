#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
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

aiagent::llm::ChatMessage ToolCallMessage(const std::string& name,
                                          const std::string& arguments = "{}") {
    aiagent::llm::ChatMessage message;
    message.role = "assistant";
    message.tool_calls.push_back(aiagent::llm::ToolCall{"call-1", name, arguments});
    return message;
}

// 只负责回答摘要请求（无 tools 的 Chat）的 fake 模型。
class SummaryModel final : public aiagent::llm::IModelClient {
public:
    int summary_calls = 0;
    std::string last_prompt;
    bool saw_previous_summary = false;

    aiagent::llm::ChatResult Chat(
            const std::vector<aiagent::llm::ChatMessage>& messages,
            const std::vector<aiagent::llm::ToolSpec>&,
            const std::optional<std::string>&) override {
        ++summary_calls;
        for (const auto& message : messages) {
            if (message.role == "user") {
                last_prompt = message.content;
            }
        }
        if (last_prompt.find("<previous-summary>") != std::string::npos) {
            saw_previous_summary = true;
        }
        aiagent::llm::ChatResult result;
        aiagent::llm::ChatChoice choice;
        choice.role = "assistant";
        choice.content = "## Goal\nfake summary";
        result.choices.push_back(std::move(choice));
        return result;
    }

    void ChatStream(const std::vector<aiagent::llm::ChatMessage>&,
                    const std::vector<aiagent::llm::ToolSpec>&,
                    const std::function<void(const aiagent::llm::StreamEvent&)>&) override {
        throw std::runtime_error("SummaryModel::ChatStream should not be called");
    }
};

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

void TestEffectiveCompactThreshold() {
    using aiagent::agent::CompactionSettings;
    using aiagent::agent::EffectiveCompactThreshold;
    using aiagent::agent::ShouldCompact;

    CompactionSettings settings;
    settings.context_window = 1000000;
    settings.reserve_tokens = 200000;
    Expect(EffectiveCompactThreshold(settings) == 800000,
           "default threshold should be context_window - reserve_tokens");
    Expect(!ShouldCompact(800000, settings), "equal to threshold should not compact");
    Expect(ShouldCompact(800001, settings), "above threshold should compact");

    // 软阈值只允许比硬线更早触发，不允许超过硬线。
    settings.compact_at_tokens = 300000;
    Expect(EffectiveCompactThreshold(settings) == 300000,
           "smaller soft threshold should win");
    settings.compact_at_tokens = 900000;
    Expect(EffectiveCompactThreshold(settings) == 800000,
           "soft threshold must not exceed context_window - reserve_tokens");

    // 预留配得离谱（>= 窗口）时退化为窗口一半，避免阈值为负导致每轮都压缩。
    settings.compact_at_tokens = 0;
    settings.reserve_tokens = 1000000;
    Expect(EffectiveCompactThreshold(settings) == 500000,
           "bogus reserve should fall back to half of the window");
    Expect(!ShouldCompact(200000, settings),
           "fallback threshold should still gate compaction");

    settings.context_window = 0;
    Expect(EffectiveCompactThreshold(settings) == 0, "zero window disables the threshold");
}

void TestNormalizeCompactionSettings() {
    using aiagent::agent::CompactionSettings;
    using aiagent::agent::NormalizeCompactionSettings;

    // keep_recent 太贴近阈值 → 夹到阈值 1/3 并给出原因（这就是"每 40 秒压一次"的成因）。
    CompactionSettings too_close;
    too_close.context_window = 1000000;
    too_close.reserve_tokens = 200000;
    too_close.keep_recent_tokens = 700000;
    const auto adjusted = NormalizeCompactionSettings(&too_close);
    Expect(adjusted.changed, "keep_recent close to the threshold should be adjusted");
    Expect(too_close.keep_recent_tokens == 800000 / 3,
           "keep_recent should be clamped to a third of the threshold");
    Expect(!adjusted.reason.empty(), "adjustment should explain itself");

    // 健康配置不动。
    CompactionSettings healthy;
    healthy.context_window = 1000000;
    healthy.reserve_tokens = 200000;
    healthy.keep_recent_tokens = 60000;
    const auto untouched = NormalizeCompactionSettings(&healthy);
    Expect(!untouched.changed, "healthy settings should stay untouched");
    Expect(healthy.keep_recent_tokens == 60000, "healthy keep_recent should be preserved");

    Expect(!NormalizeCompactionSettings(nullptr).changed, "null settings should be a no-op");
}

void TestFindCutPoint() {
    using aiagent::agent::FindCutPoint;

    // 一条 tool 消息必须紧跟其 assistant tool_calls，绝不能被切开。
    const std::vector<aiagent::llm::ChatMessage> messages = {
            TextMessage("system", "sys"),
            TextMessage("user", "read the file"),
            ToolCallMessage("read"),
            TextMessage("tool", std::string(3000, 'x')),
            ToolCallMessage("read"),
            TextMessage("tool", std::string(3000, 'x')),
            ToolCallMessage("read"),
            TextMessage("tool", std::string(3000, 'x')),
    };

    const auto too_small = FindCutPoint(messages, 100000);
    Expect(!too_small.valid, "huge keep_recent should not find a cut point");

    const auto cut = FindCutPoint(messages, 1500);
    Expect(cut.valid, "budget-spanning cut point should be found");
    if (cut.valid) {
        Expect(cut.first_kept_index > 1, "cut point should leave something to summarize");
        const std::string& role = messages[cut.first_kept_index].role;
        Expect(role == "user" || role == "assistant",
               "cut point must be user/assistant, never tool");
    }

    // 只有 system 时不可压缩。
    const auto tiny = FindCutPoint({TextMessage("system", "sys")}, 10);
    Expect(!tiny.valid, "single system message should not be compactable");
}

void TestSerializeConversation() {
    using aiagent::agent::SerializeConversation;

    std::vector<aiagent::llm::ChatMessage> messages = {
            TextMessage("system", "ignored system"),
            TextMessage("user", "hello"),
            ToolCallMessage("read"),
            TextMessage("tool", std::string(5000, 'y')),
    };
    const std::string text = SerializeConversation(messages);
    Expect(text.find("ignored system") == std::string::npos,
           "system prompt should be skipped in serialization");
    Expect(text.find("[User]: hello") != std::string::npos, "user text should appear");
    Expect(text.find("read(") != std::string::npos, "tool call should appear");
    Expect(text.find("truncated") != std::string::npos,
           "large tool result should be truncated in serialization");
}

void TestCompactMessages() {
    using aiagent::agent::CompactMessages;
    using aiagent::agent::CompactionSettings;
    using aiagent::agent::kCompactionSummaryMarker;

    auto model = std::make_shared<SummaryModel>();
    CompactionSettings settings;
    settings.enabled = true;
    settings.context_window = 2500;
    settings.reserve_tokens = 200;
    settings.keep_recent_tokens = 1500;

    std::vector<aiagent::llm::ChatMessage> messages = {
            TextMessage("system", "sys"),
            TextMessage("user", "u1"),
            ToolCallMessage("read", R"({"path":"src/b.cpp"})"),
            TextMessage("tool", std::string(3000, 'x')),
            ToolCallMessage("edit", R"({"path":"src/a.cpp"})"),
            TextMessage("tool", std::string(3000, 'x')),
            ToolCallMessage("read", R"({"path":"src/c.cpp"})"),
            TextMessage("tool", std::string(3000, 'x')),
    };

    const auto outcome = CompactMessages(*model, messages, settings);
    Expect(outcome.compacted, "large context should be compacted");
    Expect(model->summary_calls == 1, "compaction should call the model once");
    Expect(outcome.tokens_after < outcome.tokens_before,
           "compaction should reduce estimated tokens");
    if (outcome.compacted) {
        Expect(outcome.messages.size() < messages.size(),
               "compacted list should be shorter");
        Expect(outcome.messages.front().role == "system",
               "compacted list should keep the system prompt");
        Expect(outcome.messages[1].role == "user",
               "summary should be injected as a user message");
        Expect(outcome.messages[1].content.find(kCompactionSummaryMarker) == 0,
               "summary message should carry the compaction marker");
        Expect(outcome.messages[1].content.find("fake summary") != std::string::npos,
               "summary message should contain the model summary");
        Expect(outcome.messages[1].content.find("<read-files>") != std::string::npos,
               "summary should append read-files");
        Expect(outcome.messages[1].content.find("<modified-files>") != std::string::npos,
               "summary should append modified-files");
        Expect(outcome.messages[1].content.find("src/b.cpp") != std::string::npos,
               "read-only file should be recorded");
        Expect(outcome.messages[1].content.find("src/a.cpp") != std::string::npos,
               "modified file should be recorded");
        // 保留的尾部不应以孤立的 tool 消息开头。
        Expect(outcome.messages[2].role != "tool",
               "retained tail must not start with a tool result");
    }

    // 小上下文不触发压缩。
    const std::vector<aiagent::llm::ChatMessage> small = {
            TextMessage("system", "sys"),
            TextMessage("user", "hi"),
    };
    const auto skipped = CompactMessages(*model, small, settings);
    Expect(!skipped.compacted, "small context should not be compacted");
    Expect(model->summary_calls == 1, "no extra summary call for small context");
}

void TestFileOperations() {
    using aiagent::agent::ExtractFileOperations;
    using aiagent::agent::FormatFileOperations;

    const std::vector<aiagent::llm::ChatMessage> messages = {
            ToolCallMessage("read", R"({"path":"src/a.cpp"})"),
            ToolCallMessage("edit", R"({"path":"src/b.cpp"})"),
            ToolCallMessage("write", R"({"path":"src/c.cpp"})"),
            // b 既读过又改过，read-files 中应被排除。
            ToolCallMessage("read", R"({"path":"src/b.cpp"})"),
            // 非文件工具的调用应被忽略。
            ToolCallMessage("bash", R"({"command":"ls"})"),
    };

    const auto ops = ExtractFileOperations(messages);
    Expect(ops.read.size() == 2, "two distinct read files should be collected");
    Expect(ops.edited.count("src/b.cpp") == 1, "edit path should be collected");
    Expect(ops.written.count("src/c.cpp") == 1, "write path should be collected");

    const std::string text = FormatFileOperations(ops);
    Expect(text.find("<read-files>\nsrc/a.cpp\n</read-files>") != std::string::npos,
           "read-only file should be listed");
    Expect(text.find("<modified-files>") != std::string::npos,
           "modified files section should exist");
    const std::size_t read_begin = text.find("<read-files>");
    const std::size_t read_end = text.find("</read-files>");
    Expect(text.substr(read_begin, read_end - read_begin).find("src/b.cpp") ==
                   std::string::npos,
           "modified file should be excluded from read-files");
    Expect(text.find("src/b.cpp") != std::string::npos, "modified file should be listed");
    Expect(text.find("src/c.cpp") != std::string::npos, "written file should be listed");

    // 空操作返回空串。
    Expect(FormatFileOperations({}).empty(), "empty file ops should format to empty");

    // 历史摘要中的清单应可累积传递。
    aiagent::llm::ChatMessage previous_summary;
    previous_summary.role = "user";
    previous_summary.content =
            "[summary]\n\n<read-files>\nsrc/old.cpp\n</read-files>\n\n"
            "<modified-files>\nsrc/mod.cpp\n</modified-files>\n";
    const auto carried = ExtractFileOperations({previous_summary});
    Expect(carried.read.count("src/old.cpp") == 1,
           "previous read-files should be carried forward");
    Expect(carried.edited.count("src/mod.cpp") == 1,
           "previous modified-files should be carried forward");
}

void TestOverflowDetection() {
    using aiagent::agent::IsContextOverflowMessage;
    using aiagent::agent::IsRecoverableLengthStop;

    Expect(IsContextOverflowMessage(
                   "This model's maximum context length is 8192 tokens"),
           "maximum context length should be detected");
    Expect(IsContextOverflowMessage("error: context_length_exceeded"),
           "context_length_exceeded should be detected");
    Expect(IsContextOverflowMessage("Request has too many tokens"),
           "too many tokens should be detected");
    Expect(!IsContextOverflowMessage("invalid api key"),
           "unrelated errors should not be treated as overflow");

    Expect(IsRecoverableLengthStop("length"), "length finish_reason is recoverable");
    Expect(!IsRecoverableLengthStop("stop"), "stop finish_reason is not recoverable");
    Expect(!IsRecoverableLengthStop("tool_calls"),
           "tool_calls finish_reason is not recoverable");
}

void TestIncrementalSummary() {
    using aiagent::agent::CompactMessages;
    using aiagent::agent::CompactionSettings;
    using aiagent::agent::kCompactionSummaryMarker;

    auto model = std::make_shared<SummaryModel>();
    CompactionSettings settings;
    settings.enabled = true;
    settings.context_window = 2500;
    settings.reserve_tokens = 200;
    settings.keep_recent_tokens = 1500;

    const std::vector<aiagent::llm::ChatMessage> messages = {
            TextMessage("system", "sys"),
            TextMessage("user", "u1"),
            ToolCallMessage("read", R"({"path":"src/b.cpp"})"),
            TextMessage("tool", std::string(3000, 'x')),
            ToolCallMessage("edit", R"({"path":"src/a.cpp"})"),
            TextMessage("tool", std::string(3000, 'x')),
            ToolCallMessage("read", R"({"path":"src/c.cpp"})"),
            TextMessage("tool", std::string(3000, 'x')),
    };

    const auto first = CompactMessages(*model, messages, settings);
    Expect(first.compacted, "first compaction should succeed");
    Expect(!model->saw_previous_summary,
           "first compaction should use the initial summary template");

    // 在已压缩结果后继续追加新回合，再次压缩应走增量更新。
    std::vector<aiagent::llm::ChatMessage> extended = first.messages;
    extended.push_back(TextMessage("user", "u2"));
    extended.push_back(ToolCallMessage("read", R"({"path":"src/d.cpp"})"));
    extended.push_back(TextMessage("tool", std::string(3000, 'x')));

    const auto second = CompactMessages(*model, extended, settings);
    Expect(second.compacted, "second compaction should succeed");
    Expect(model->summary_calls == 2, "summary model should be called twice");
    Expect(model->saw_previous_summary,
           "second compaction should pass the previous summary");
    Expect(model->last_prompt.find("fake summary") != std::string::npos,
           "previous summary body should be preserved in the update prompt");

    std::size_t summary_count = 0;
    for (const auto& message : second.messages) {
        if (message.content.rfind(kCompactionSummaryMarker, 0) == 0) {
            ++summary_count;
        }
    }
    Expect(summary_count == 1,
           "incremental compaction should keep exactly one summary message");
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
        // 默认对齐 deepseek-flash 的真实窗口（1M）；阈值取「窗口 - 预留」= 80 万。
        Expect(defaults->context_window == 1000000, "default context_window");
        Expect(defaults->reserve_tokens == 200000, "default reserve_tokens");
        Expect(defaults->compact_at_tokens == 0, "default compact_at_tokens");
        Expect(defaults->keep_recent_tokens == 60000, "default keep_recent_tokens");
        Expect(defaults->min_gain_ratio == 0.3, "default min_gain_ratio");
    }

    // 新增键：软阈值与收益下限。
    const auto soft = aiagent::ParseCompactionConfig(
            "[compaction]\ncompact_at_tokens = 300000\nmin_gain_ratio = 0.5\n", &error);
    Expect(soft.has_value(), "compact_at_tokens/min_gain_ratio should parse");
    if (soft.has_value()) {
        Expect(soft->compact_at_tokens == 300000, "compact_at_tokens should parse");
        Expect(soft->min_gain_ratio == 0.5, "min_gain_ratio should parse");
    }

    const auto bad_soft = aiagent::ParseCompactionConfig(
            "[compaction]\ncompact_at_tokens = -1\n", &error);
    Expect(!bad_soft.has_value(), "negative compact_at_tokens should fail");

    const auto bad_ratio = aiagent::ParseCompactionConfig(
            "[compaction]\nmin_gain_ratio = 1.5\n", &error);
    Expect(!bad_ratio.has_value(), "min_gain_ratio above 1 should fail");

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
    TestEffectiveCompactThreshold();
    TestNormalizeCompactionSettings();
    TestFindCutPoint();
    TestSerializeConversation();
    TestCompactMessages();
    TestFileOperations();
    TestOverflowDetection();
    TestIncrementalSummary();
    TestCompactionConfig();

    if (g_failures != 0) {
        std::cerr << "compaction test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "compaction test passed\n";
    return 0;
}
