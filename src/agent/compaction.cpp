#include "aiagent/agent/compaction.hpp"

#include <stdexcept>
#include <utility>

namespace aiagent::agent {
namespace {

// 每条消息固定的角色标记、分隔符等开销，避免长会话被系统性低估。
constexpr int kPerMessageOverhead = 4;

// 摘要时单条工具结果的字符上限，控制摘要调用本身的输入量。
constexpr std::size_t kToolResultMaxChars = 2000;

// 摘要时保留的思考内容字符上限。
constexpr std::size_t kThinkingMaxChars = 500;

int EstimateBytes(std::size_t bytes) {
    if (bytes == 0) {
        return 0;
    }
    // ceil(bytes / 3)：对中文（3 字节/字）比 chars/4 更保守。
    return static_cast<int>((bytes + 2) / 3);
}

std::string TruncateForSummary(const std::string& text, std::size_t max_chars) {
    if (text.size() <= max_chars) {
        return text;
    }
    const std::size_t dropped = text.size() - max_chars;
    return text.substr(0, max_chars) + "\n[... " + std::to_string(dropped) +
           " more characters truncated]";
}

constexpr const char* kSummarizationSystemPrompt =
        "You are a context summarization assistant. Your task is to read a "
        "conversation between a user and an AI assistant, then produce a "
        "structured summary following the exact format specified.\n\n"
        "Do NOT continue the conversation. Do NOT respond to any questions in "
        "the conversation. ONLY output the structured summary.";

// 对齐 Pi 的六段摘要模板。
constexpr const char* kSummarizationPrompt =
        "The messages above are a conversation to summarize. Create a "
        "structured context checkpoint summary that another LLM will use to "
        "continue the work.\n\n"
        "Use this EXACT format:\n\n"
        "## Goal\n"
        "[What is the user trying to accomplish? Can be multiple items if the "
        "session covers different tasks.]\n\n"
        "## Constraints & Preferences\n"
        "- [Any constraints, preferences, or requirements mentioned by user]\n"
        "- [Or \"(none)\" if none were mentioned]\n\n"
        "## Progress\n"
        "### Done\n"
        "- [x] [Completed tasks/changes]\n\n"
        "### In Progress\n"
        "- [ ] [Current work]\n\n"
        "### Blocked\n"
        "- [Issues preventing progress, if any]\n\n"
        "## Key Decisions\n"
        "- **[Decision]**: [Brief rationale]\n\n"
        "## Next Steps\n"
        "1. [Ordered list of what should happen next]\n\n"
        "## Critical Context\n"
        "- [Any data, examples, or references needed to continue]\n"
        "- [Or \"(none)\" if not applicable]\n\n"
        "Keep each section concise. Preserve exact file paths, function names, "
        "and error messages.";

}  // namespace

int EstimateTokens(const llm::ChatMessage& message) {
    std::size_t bytes = message.content.size() + message.reasoning_content.size() +
                        message.tool_call_id.size();
    for (const auto& call : message.tool_calls) {
        bytes += call.name.size() + call.arguments.size();
    }
    return EstimateBytes(bytes) + kPerMessageOverhead;
}

int EstimateMessagesTokens(const std::vector<llm::ChatMessage>& messages) {
    int total = 0;
    for (const auto& message : messages) {
        total += EstimateTokens(message);
    }
    return total;
}

ContextUsageEstimate EstimateContextTokens(
        const std::vector<llm::ChatMessage>& messages,
        int last_usage_tokens,
        std::size_t last_usage_message_count) {
    ContextUsageEstimate estimate;
    if (last_usage_tokens > 0 && last_usage_message_count <= messages.size()) {
        estimate.has_usage = true;
        estimate.usage_tokens = last_usage_tokens;
        for (std::size_t i = last_usage_message_count; i < messages.size(); ++i) {
            estimate.trailing_tokens += EstimateTokens(messages[i]);
        }
    } else {
        for (const auto& message : messages) {
            estimate.trailing_tokens += EstimateTokens(message);
        }
    }
    estimate.tokens = estimate.usage_tokens + estimate.trailing_tokens;
    return estimate;
}

bool ShouldCompact(int context_tokens, const CompactionSettings& settings) {
    if (!settings.enabled || settings.context_window <= 0) {
        return false;
    }
    return context_tokens > settings.context_window - settings.reserve_tokens;
}

const char* const kCompactionSummaryMarker =
        "[Context summary: earlier messages were compacted to fit the context window.]";

CutPointResult FindCutPoint(const std::vector<llm::ChatMessage>& messages,
                            int keep_recent_tokens) {
    CutPointResult result;
    if (messages.size() <= 1) {
        return result;
    }

    // 合法切点：user / assistant。tool 结果必须紧跟自己 assistant 的 tool_calls，
    // system 不能丢，所以都不能作为切点。
    std::vector<std::size_t> cut_points;
    for (std::size_t i = 1; i < messages.size(); ++i) {
        const std::string& role = messages[i].role;
        if (role == "user" || role == "assistant") {
            cut_points.push_back(i);
        }
    }
    if (cut_points.empty()) {
        return result;
    }

    // 从最新往旧累计，落到预算后在最近的合法切点切开。
    std::size_t cut = cut_points.front();
    int accumulated = 0;
    for (std::size_t i = messages.size(); i-- > 1;) {
        const int tokens = EstimateTokens(messages[i]);
        if (tokens == 0) {
            continue;
        }
        accumulated += tokens;
        if (accumulated >= keep_recent_tokens) {
            for (const std::size_t candidate : cut_points) {
                if (candidate >= i) {
                    cut = candidate;
                    break;
                }
            }
            break;
        }
    }

    // 切点太靠前（<=1）意味着没有可摘要的历史，视为不可压缩。
    if (cut <= 1) {
        return result;
    }
    result.first_kept_index = cut;
    result.valid = true;
    return result;
}

std::string SerializeConversation(const std::vector<llm::ChatMessage>& messages) {
    std::string out;
    for (const auto& message : messages) {
        if (message.role == "system") {
            continue;
        }
        if (message.role == "user") {
            out += "[User]: ";
            out += message.content;
            out += "\n\n";
        } else if (message.role == "assistant") {
            if (!message.reasoning_content.empty()) {
                out += "[Assistant thinking]: ";
                out += TruncateForSummary(message.reasoning_content, kThinkingMaxChars);
                out += "\n";
            }
            if (!message.content.empty()) {
                out += "[Assistant]: ";
                out += message.content;
                out += "\n";
            }
            if (!message.tool_calls.empty()) {
                out += "[Assistant tool calls]: ";
                for (std::size_t i = 0; i < message.tool_calls.size(); ++i) {
                    if (i != 0) {
                        out += "; ";
                    }
                    out += message.tool_calls[i].name;
                    out += "(";
                    out += message.tool_calls[i].arguments;
                    out += ")";
                }
                out += "\n";
            }
            out += "\n";
        } else if (message.role == "tool") {
            out += "[Tool result]: ";
            out += TruncateForSummary(message.content, kToolResultMaxChars);
            out += "\n\n";
        }
    }
    return out;
}

std::string GenerateSummary(llm::IModelClient& model,
                            const std::vector<llm::ChatMessage>& to_summarize) {
    std::vector<llm::ChatMessage> request;
    llm::ChatMessage system;
    system.role = "system";
    system.content = kSummarizationSystemPrompt;
    request.push_back(std::move(system));

    llm::ChatMessage user;
    user.role = "user";
    user.content = "<conversation>\n" + SerializeConversation(to_summarize) +
                   "\n</conversation>\n\n" + kSummarizationPrompt;
    request.push_back(std::move(user));

    const llm::ChatResult result = model.Chat(request);
    if (result.choices.empty()) {
        throw std::runtime_error(
                "compaction summarization returned no choices");
    }
    std::string summary = result.choices.front().content;
    if (summary.empty()) {
        throw std::runtime_error("compaction summarization returned empty summary");
    }
    return summary;
}

CompactionOutcome CompactMessages(llm::IModelClient& model,
                                  const std::vector<llm::ChatMessage>& messages,
                                  const CompactionSettings& settings) {
    CompactionOutcome outcome;
    outcome.tokens_before = EstimateMessagesTokens(messages);

    const CutPointResult cut = FindCutPoint(messages, settings.keep_recent_tokens);
    if (!cut.valid) {
        return outcome;
    }

    std::vector<llm::ChatMessage> to_summarize(
            messages.begin() + 1, messages.begin() +
                                          static_cast<std::ptrdiff_t>(cut.first_kept_index));
    if (to_summarize.empty()) {
        return outcome;
    }

    std::vector<llm::ChatMessage> compacted;
    compacted.reserve(messages.size() - cut.first_kept_index + 2);
    // 保留 system 提示词。
    compacted.push_back(messages.front());

    llm::ChatMessage summary_message;
    summary_message.role = "user";
    summary_message.content =
            std::string(kCompactionSummaryMarker) + "\n\n" + GenerateSummary(model, to_summarize);
    compacted.push_back(std::move(summary_message));

    for (std::size_t i = cut.first_kept_index; i < messages.size(); ++i) {
        compacted.push_back(messages[i]);
    }

    outcome.compacted = true;
    outcome.messages = std::move(compacted);
    outcome.tokens_after = EstimateMessagesTokens(outcome.messages);
    return outcome;
}

}  // namespace aiagent::agent
