#include "aiagent/agent/compaction.hpp"

#include <cctype>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <sstream>
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

std::string TrimAscii(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

// 从 content 中提取 <tag>...</tag> 内的逐行文件名，用于累积历史摘要中的清单。
void ExtractTaggedFiles(const std::string& content, const std::string& tag,
                        std::set<std::string>& out) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    std::size_t pos = 0;
    while ((pos = content.find(open, pos)) != std::string::npos) {
        const std::size_t start = pos + open.size();
        const std::size_t end = content.find(close, start);
        if (end == std::string::npos) {
            break;
        }
        std::istringstream input(content.substr(start, end - start));
        std::string line;
        while (std::getline(input, line)) {
            const std::string trimmed = TrimAscii(line);
            if (!trimmed.empty()) {
                out.insert(trimmed);
            }
        }
        pos = end + close.size();
    }
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

// 对齐 Pi 的增量更新模板：保留旧摘要、合并新进展。
constexpr const char* kUpdateSummarizationPrompt =
        "The messages above are NEW conversation messages to incorporate into "
        "the existing summary provided in <previous-summary> tags. Update the "
        "existing structured summary with new information. RULES:\n"
        "- PRESERVE all existing information from the previous summary\n"
        "- ADD new progress, decisions, and context from the new messages\n"
        "- UPDATE the Progress section: move items from \"In Progress\" to "
        "\"Done\" when completed\n"
        "- UPDATE \"Next Steps\" based on what was accomplished\n"
        "- PRESERVE exact file paths, function names, and error messages\n"
        "- If something is no longer relevant, you may remove it\n\n"
        "Use this EXACT format:\n\n"
        "## Goal\n"
        "[Preserve existing goals, add new ones if the task expanded]\n\n"
        "## Constraints & Preferences\n"
        "- [Preserve existing, add new ones discovered]\n\n"
        "## Progress\n"
        "### Done\n"
        "- [x] [Include previously done items AND newly completed items]\n\n"
        "### In Progress\n"
        "- [ ] [Current work - update based on progress]\n\n"
        "### Blocked\n"
        "- [Current blockers - remove if resolved]\n\n"
        "## Key Decisions\n"
        "- **[Decision]**: [Brief rationale] (preserve all previous, add new)\n\n"
        "## Next Steps\n"
        "1. [Update based on current state]\n\n"
        "## Critical Context\n"
        "- [Preserve important context, add new if needed]\n\n"
        "Keep each section concise. Preserve exact file paths, function names, "
        "and error messages.";

// 去掉文本中的 <read-files>/<modified-files> 清单，避免在增量摘要中重复。
std::string StripFileOperationTags(const std::string& text) {
    std::string result = text;
    for (const char* tag : {"read-files", "modified-files"}) {
        const std::string open = "<" + std::string(tag) + ">";
        const std::string close = "</" + std::string(tag) + ">";
        std::size_t pos = 0;
        while ((pos = result.find(open, pos)) != std::string::npos) {
            const std::size_t end = result.find(close, pos);
            if (end == std::string::npos) {
                break;
            }
            result.erase(pos, end + close.size() - pos);
        }
    }
    return TrimAscii(result);
}

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

int EffectiveCompactThreshold(const CompactionSettings& settings) {
    if (settings.context_window <= 0) {
        return 0;
    }
    const int window_budget = settings.context_window - settings.reserve_tokens;
    const int fallback = std::max(1, settings.context_window / 2);
    int threshold = window_budget > 0 ? window_budget : fallback;
    if (settings.compact_at_tokens > 0) {
        // 软阈值只允许比硬线更早触发，不允许超过它。
        threshold = std::min(threshold, settings.compact_at_tokens);
    }
    return std::max(1, threshold);
}

CompactionAdjustment NormalizeCompactionSettings(CompactionSettings* settings) {
    CompactionAdjustment adjustment;
    if (settings == nullptr) {
        return adjustment;
    }
    const int threshold = EffectiveCompactThreshold(*settings);
    if (threshold <= 0 || settings->keep_recent_tokens <= 0) {
        adjustment.keep_recent_tokens = settings->keep_recent_tokens;
        return adjustment;
    }
    const int max_keep = std::max(1, threshold / 3);
    if (settings->keep_recent_tokens <= max_keep) {
        adjustment.keep_recent_tokens = settings->keep_recent_tokens;
        return adjustment;
    }
    adjustment.changed = true;
    adjustment.keep_recent_tokens = max_keep;
    adjustment.reason = "keep_recent_tokens=" +
                        std::to_string(settings->keep_recent_tokens) +
                        " is too close to the compaction threshold=" +
                        std::to_string(threshold) + "; clamped to " +
                        std::to_string(max_keep);
    settings->keep_recent_tokens = max_keep;
    return adjustment;
}

bool ShouldCompact(int context_tokens, const CompactionSettings& settings) {
    if (!settings.enabled) {
        return false;
    }
    const int threshold = EffectiveCompactThreshold(settings);
    if (threshold <= 0) {
        return false;
    }
    return context_tokens > threshold;
}

bool IsContextOverflowMessage(const std::string& message) {
    std::string lower;
    lower.reserve(message.size());
    for (const char c : message) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    static const char* const kPatterns[] = {
            "maximum context length",
            "context length",
            "context_length_exceeded",
            "maximum number of tokens",
            "too many tokens",
            "reduce the length",
            "exceeds the maximum",
            "token limit",
    };
    for (const char* pattern : kPatterns) {
        if (lower.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool IsRecoverableLengthStop(const std::string& finish_reason) {
    return finish_reason == "length";
}

FileOperations ExtractFileOperations(const std::vector<llm::ChatMessage>& messages) {
    FileOperations operations;
    for (const auto& message : messages) {
        if (message.role == "assistant") {
            for (const auto& call : message.tool_calls) {
                std::string path;
                try {
                    if (!call.arguments.empty()) {
                        const auto arguments = nlohmann::json::parse(call.arguments);
                        if (arguments.is_object() && arguments.contains("path") &&
                            arguments["path"].is_string()) {
                            path = arguments["path"].get<std::string>();
                        }
                    }
                } catch (const std::exception&) {
                    // 参数不是合法 JSON 时跳过。
                }
                if (path.empty()) {
                    continue;
                }
                if (call.name == "read") {
                    operations.read.insert(path);
                } else if (call.name == "write") {
                    operations.written.insert(path);
                } else if (call.name == "edit") {
                    operations.edited.insert(path);
                }
            }
        }
        // 累积此前摘要中已记录的文件清单。
        ExtractTaggedFiles(message.content, "read-files", operations.read);
        ExtractTaggedFiles(message.content, "modified-files", operations.edited);
    }
    return operations;
}

std::string FormatFileOperations(const FileOperations& operations) {
    std::set<std::string> modified = operations.written;
    modified.insert(operations.edited.begin(), operations.edited.end());

    std::vector<std::string> read_only;
    for (const auto& file : operations.read) {
        if (modified.find(file) == modified.end()) {
            read_only.push_back(file);
        }
    }

    std::string out;
    if (!read_only.empty()) {
        out += "<read-files>\n";
        for (const auto& file : read_only) {
            out += file;
            out += "\n";
        }
        out += "</read-files>\n";
    }
    if (!modified.empty()) {
        if (!out.empty()) {
            out += "\n";
        }
        out += "<modified-files>\n";
        for (const auto& file : modified) {
            out += file;
            out += "\n";
        }
        out += "</modified-files>\n";
    }
    if (out.empty()) {
        return {};
    }
    return "\n\n" + out;
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
            // 优先取「>= i」的最近切点（少保留一点）；若不存在（例如 i 正好落在
            // 排在最尾部的 tool 结果上，或单条尾部消息本身就超过预算），则退回到
            // 「< i」的最近切点 —— 多保留一条消息，但绝不因此判定"不可压缩"。
            for (const std::size_t candidate : cut_points) {
                if (candidate >= i) {
                    cut = candidate;
                    break;
                }
                cut = candidate;
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
                            const std::vector<llm::ChatMessage>& to_summarize,
                            const std::string& previous_summary) {
    std::vector<llm::ChatMessage> request;
    llm::ChatMessage system;
    system.role = "system";
    system.content = kSummarizationSystemPrompt;
    request.push_back(std::move(system));

    std::string prompt;
    if (!previous_summary.empty()) {
        prompt = "<previous-summary>\n" + previous_summary +
                 "\n</previous-summary>\n\n" + kUpdateSummarizationPrompt;
    } else {
        prompt = kSummarizationPrompt;
    }

    llm::ChatMessage user;
    user.role = "user";
    user.content = "<conversation>\n" + SerializeConversation(to_summarize) +
                   "\n</conversation>\n\n" + prompt;
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

    // 若已有旧摘要（在 system 之后的第一条），提取其正文作为增量更新的基础。
    std::size_t start = 1;
    std::string previous_summary;
    if (messages.size() > 1 && messages[1].role == "user" &&
        messages[1].content.rfind(kCompactionSummaryMarker, 0) == 0) {
        previous_summary = StripFileOperationTags(
                messages[1].content.substr(std::strlen(kCompactionSummaryMarker)));
        start = 2;
    }

    if (cut.first_kept_index <= start) {
        // 没有可摘要的新内容（例如刚刚压缩过）。
        return outcome;
    }

    std::vector<llm::ChatMessage> to_summarize(
            messages.begin() + static_cast<std::ptrdiff_t>(start),
            messages.begin() + static_cast<std::ptrdiff_t>(cut.first_kept_index));
    if (to_summarize.empty()) {
        return outcome;
    }

    // 文件清单从 [1, cut) 累积：既包含旧摘要中的清单，也包含新工具调用。
    const std::vector<llm::ChatMessage> file_scope(
            messages.begin() + 1,
            messages.begin() + static_cast<std::ptrdiff_t>(cut.first_kept_index));

    std::vector<llm::ChatMessage> compacted;
    compacted.reserve(messages.size() - cut.first_kept_index + 2);
    // 保留 system 提示词。
    compacted.push_back(messages.front());

    llm::ChatMessage summary_message;
    summary_message.role = "user";
    std::string summary_text = GenerateSummary(model, to_summarize, previous_summary);
    summary_text += FormatFileOperations(ExtractFileOperations(file_scope));
    summary_message.content =
            std::string(kCompactionSummaryMarker) + "\n\n" + summary_text;
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
