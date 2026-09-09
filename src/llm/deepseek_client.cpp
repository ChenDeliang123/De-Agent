#include "aiagent/llm/deepseek_client.hpp"
#include "aiagent/log.hpp"

#include <a2a/core/http_client.hpp>
#include <a2a/core/json_rpc_error.hpp>
#include <a2a/core/sse_parser.hpp>
#include <json.hpp>

#include <chrono>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace aiagent::llm {
namespace {

std::string JsonString(const nlohmann::json& object, const char* key) {
    if (!object.contains(key) || !object[key].is_string()) {
        return {};
    }
    return object[key].get<std::string>();
}

nlohmann::json ToolCallsToJson(const std::vector<ToolCall>& tool_calls) {
    nlohmann::json array = nlohmann::json::array();
    for (const ToolCall& call : tool_calls) {
        array.push_back({{"id", call.id},
                         {"type", "function"},
                         {"function",
                          {{"name", call.name},
                           {"arguments", call.arguments.empty() ? "{}" : call.arguments}}}});
    }
    return array;
}

std::vector<ToolCall> ToolCallsFromJson(const nlohmann::json& value) {
    std::vector<ToolCall> calls;
    if (!value.is_array()) {
        return calls;
    }
    for (const auto& item : value) {
        if (!item.is_object()) {
            continue;
        }
        ToolCall call;
        call.id = JsonString(item, "id");
        if (item.contains("index") && item["index"].is_number_integer()) {
            call.index = item["index"].get<int>();
            call.has_index = true;
        }
        if (item.contains("function") && item["function"].is_object()) {
            call.name = JsonString(item["function"], "name");
            call.arguments = JsonString(item["function"], "arguments");
        }
        calls.push_back(std::move(call));
    }
    return calls;
}

nlohmann::json MessageToJson(const ChatMessage& message) {
    nlohmann::json object;
    object["role"] = message.role;
    if (message.role == "tool") {
        object["tool_call_id"] = message.tool_call_id;
    }
    if (!message.name.empty()) {
        object["name"] = message.name;
    }
    if (!message.reasoning_content.empty()) {
        object["reasoning_content"] = message.reasoning_content;
    }
    if (!message.tool_calls.empty()) {
        object["tool_calls"] = ToolCallsToJson(message.tool_calls);
    }
    // OpenAI/DeepSeek 兼容格式要求 content 至少是字符串；空内容按空串发送。
    object["content"] = message.content;
    return object;
}

int JsonInt(const nlohmann::json& object, const char* key) {
    if (!object.contains(key) || !object[key].is_number_integer()) {
        return 0;
    }
    return object[key].get<int>();
}

std::size_t RequestBytes(const std::vector<ChatMessage>& messages,
                         const std::vector<ToolSpec>& tools) {
    std::size_t bytes = 0;
    for (const auto& message : messages) {
        bytes += message.content.size() + message.reasoning_content.size();
        for (const auto& call : message.tool_calls) {
            bytes += call.arguments.size();
        }
    }
    for (const auto& tool : tools) {
        bytes += tool.name.size() + tool.description.size();
        if (tool.parameters.is_string()) {
            bytes += tool.parameters.get_ref<const std::string&>().size();
        }
    }
    return bytes;
}

// 429 限流与 5xx 服务端错误值得重试；4xx（认证/参数）与本地错误重试无意义。
bool IsRetryableStatusCode(int status_code) {
    return status_code == 429 || status_code >= 500;
}

// 指数退避：500ms * 2^attempt，封顶 5s。
void SleepBackoff(int attempt) {
    constexpr long kBaseDelayMs = 500;
    constexpr long kMaxDelayMs = 5000;
    long delay = kBaseDelayMs * (1L << attempt);
    if (delay > kMaxDelayMs) {
        delay = kMaxDelayMs;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
}

}  // namespace

std::string BuildChatRequestBody(const std::string& model,
                                 const std::vector<ChatMessage>& messages,
                                 const std::vector<ToolSpec>& tools,
                                 bool stream,
                                 const std::optional<std::string>& tool_choice) {
    nlohmann::json body{
        {"model", model},
        {"stream", stream},
    };

    nlohmann::json message_array = nlohmann::json::array();
    for (const ChatMessage& message : messages) {
        message_array.push_back(MessageToJson(message));
    }
    body["messages"] = std::move(message_array);

    if (!tools.empty()) {
        nlohmann::json tool_array = nlohmann::json::array();
        for (const ToolSpec& tool : tools) {
            tool_array.push_back({{"type", "function"},
                                  {"function",
                                   {{"name", tool.name},
                                    {"description", tool.description},
                                    {"parameters", tool.parameters}}}});
        }
        body["tools"] = std::move(tool_array);
        if (tool_choice.has_value()) {
            body["tool_choice"] = *tool_choice;
        }
    }
    return body.dump();
}

ChatResult ParseChatResponse(const std::string& body) {
    const nlohmann::json root = nlohmann::json::parse(body);
    if (root.contains("error")) {
        throw DeepSeekException(0, DeepSeekErrorMessage(body));
    }

    ChatResult result;
    result.id = JsonString(root, "id");
    if (root.contains("choices") && root["choices"].is_array()) {
        for (const auto& choice_value : root["choices"]) {
            ChatChoice choice;
            const auto& message =
                    choice_value.contains("message") ? choice_value["message"] : choice_value;
            choice.role = JsonString(message, "role");
            choice.content = JsonString(message, "content");
            choice.reasoning_content = JsonString(message, "reasoning_content");
            choice.tool_calls = ToolCallsFromJson(message.value("tool_calls", nlohmann::json::array()));
            choice.finish_reason = JsonString(choice_value, "finish_reason");
            result.choices.push_back(std::move(choice));
        }
    }
    if (root.contains("usage") && root["usage"].is_object()) {
        result.usage.prompt_tokens = JsonInt(root["usage"], "prompt_tokens");
        result.usage.completion_tokens = JsonInt(root["usage"], "completion_tokens");
        result.usage.total_tokens = JsonInt(root["usage"], "total_tokens");
    }
    return result;
}

StreamChunk ParseStreamChunk(const std::string& data) {
    const nlohmann::json root = nlohmann::json::parse(data);
    if (root.contains("error")) {
        throw DeepSeekException(0, DeepSeekErrorMessage(data));
    }

    StreamChunk chunk;
    chunk.id = JsonString(root, "id");
    if (root.contains("choices") && root["choices"].is_array() &&
        !root["choices"].empty()) {
        const auto& choice = root["choices"].front();
        chunk.finish_reason = JsonString(choice, "finish_reason");
        if (choice.contains("delta") && choice["delta"].is_object()) {
            const auto& delta = choice["delta"];
            chunk.role = JsonString(delta, "role");
            chunk.content = JsonString(delta, "content");
            chunk.reasoning_content = JsonString(delta, "reasoning_content");
            chunk.tool_calls =
                    ToolCallsFromJson(delta.value("tool_calls", nlohmann::json::array()));
        }
    }
    return chunk;
}

std::string DeepSeekErrorMessage(const std::string& body) {
    try {
        const nlohmann::json root = nlohmann::json::parse(body);
        if (root.contains("error")) {
            const auto& error = root["error"];
            const std::string message = JsonString(error, "message");
            if (!message.empty()) {
                return message;
            }
        }
    } catch (const std::exception&) {
        // 非 JSON 错误体直接截断返回。
    }
    return body.substr(0, 500);
}

DeepSeekClient::DeepSeekClient(DeepSeekConfig config) : config_(std::move(config)) {}

std::string DeepSeekClient::RequestBody(const std::vector<ChatMessage>& messages,
                                        const std::vector<ToolSpec>& tools,
                                        bool stream,
                                        const std::optional<std::string>& tool_choice) const {
    return BuildChatRequestBody(config_.model, messages, tools, stream, tool_choice);
}

ChatResult DeepSeekClient::Chat(const std::vector<ChatMessage>& messages,
                                const std::vector<ToolSpec>& tools,
                                const std::optional<std::string>& tool_choice) {
    int attempt = 0;
    while (true) {
        const auto started = std::chrono::steady_clock::now();
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kDebug, "deepseek",
                "mode=sync attempt=" + std::to_string(attempt) +
                        " event=request_start messages=" +
                        std::to_string(messages.size()) +
                        " request_bytes=" + std::to_string(RequestBytes(messages, tools)));
        try {
            const auto result = DoChat(messages, tools, tool_choice);
            const auto duration_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kInfo, "deepseek",
                    "mode=sync event=request_end status=200 duration_ms=" +
                            std::to_string(duration_ms) +
                            " finish=" +
                            (result.choices.empty()
                                     ? "-"
                                     : result.choices.front().finish_reason));
            return result;
        } catch (const DeepSeekException& e) {
            const bool retry = IsRetryableStatusCode(e.status_code()) &&
                               attempt < config_.max_retries;
            aiagent::DefaultLogger().Log(
                    retry ? aiagent::LogLevel::kWarning
                          : aiagent::LogLevel::kError,
                    "deepseek",
                    "mode=sync attempt=" + std::to_string(attempt) +
                            " event=error status=" + std::to_string(e.status_code()) +
                            " message=" + e.what());
            if (!retry) {
                throw;
            }
        } catch (const a2a::core::JsonRpcException& e) {
            const bool retry = attempt < config_.max_retries;
            aiagent::DefaultLogger().Log(
                    retry ? aiagent::LogLevel::kWarning
                          : aiagent::LogLevel::kError,
                    "deepseek",
                    "mode=sync event=network_error message=" + std::string(e.what()));
            if (!retry) {
                throw;
            }
        }
            // HttpClient 抛出的网络层错误（连接失败/超时等），可重试。
        SleepBackoff(attempt);
        ++attempt;
    }
}

ChatResult DeepSeekClient::DoChat(const std::vector<ChatMessage>& messages,
                                  const std::vector<ToolSpec>& tools,
                                  const std::optional<std::string>& tool_choice) {
    if (config_.api_key.empty()) {
        throw DeepSeekException(0, "DeepSeek api_key is not configured");
    }

    a2a::core::HttpClient http;
    http.SetTimeoutSeconds(config_.timeout_seconds);
    http.AddHeader("Authorization", "Bearer " + config_.api_key);
    http.AddHeader("Content-Type", "application/json");

    const auto response =
            http.Post(DeepSeekChatEndpoint(config_), RequestBody(messages, tools, false, tool_choice),
                      "application/json");
    if (!response.IsSuccess()) {
        throw DeepSeekException(response.status_code, DeepSeekErrorMessage(response.body));
    }
    return ParseChatResponse(response.body);
}

void DeepSeekClient::ChatStream(const std::vector<ChatMessage>& messages,
                                const std::vector<ToolSpec>& tools,
                                const std::function<void(const StreamEvent&)>& on_event) {
    // 一旦已向调用方派发过事件，中途失败不再重试——否则会产生重复的
    // token/工具调用片段，破坏 Agent 侧的消息拼接。
    bool delivered_any = false;
    int attempt = 0;
    while (true) {
        const auto started = std::chrono::steady_clock::now();
        aiagent::DefaultLogger().Log(
                aiagent::LogLevel::kDebug, "deepseek",
                "mode=stream attempt=" + std::to_string(attempt) +
                        " event=request_start messages=" +
                        std::to_string(messages.size()) +
                        " request_bytes=" + std::to_string(RequestBytes(messages, tools)));
        try {
            if (config_.api_key.empty()) {
                throw DeepSeekException(0, "DeepSeek api_key is not configured");
            }

            a2a::core::HttpClient http;
            http.SetTimeoutSeconds(config_.timeout_seconds);
            http.AddHeader("Authorization", "Bearer " + config_.api_key);
            http.AddHeader("Content-Type", "application/json");

            std::map<std::size_t, ToolCall> pending_tool_calls;
            bool parse_failed = false;
            std::string parse_error;

            a2a::core::SseParser parser([&](const std::string& data) {
                if (parse_failed || data == "[DONE]") {
                    return;
                }
                try {
                    const StreamChunk chunk = ParseStreamChunk(data);
                    if (!chunk.role.empty() || !chunk.content.empty() ||
                        !chunk.reasoning_content.empty() || !chunk.finish_reason.empty()) {
                        StreamEvent event;
                        event.role = chunk.role;
                        event.content = chunk.content;
                        event.reasoning_content = chunk.reasoning_content;
                        event.finish_reason = chunk.finish_reason;
                        delivered_any = true;
                        if (on_event) {
                            on_event(event);
                        }
                    }
                    for (const ToolCall& call : chunk.tool_calls) {
                        const std::size_t call_index =
                                call.has_index ? static_cast<std::size_t>(call.index)
                                               : pending_tool_calls.size();
                        ToolCall& pending = pending_tool_calls[call_index];
                        if (!call.id.empty()) {
                            pending.id = call.id;
                        }
                        if (!call.name.empty()) {
                            pending.name = call.name;
                        }
                        pending.arguments += call.arguments;
                    }
                    if (chunk.finish_reason == "tool_calls" && !pending_tool_calls.empty()) {
                        StreamEvent event;
                        event.finish_reason = chunk.finish_reason;
                        for (auto& [index, call] : pending_tool_calls) {
                            event.tool_calls.push_back(std::move(call));
                        }
                        pending_tool_calls.clear();
                        delivered_any = true;
                        if (on_event) {
                            on_event(event);
                        }
                    }
                } catch (const std::exception& e) {
                    parse_failed = true;
                    parse_error = e.what();
                }
            });

            const auto response = http.PostStream(
                    DeepSeekChatEndpoint(config_), RequestBody(messages, tools, true, std::nullopt),
                    "application/json", [&](std::string_view chunk) { parser.Feed(chunk); });
            if (!response.IsSuccess()) {
                throw DeepSeekException(response.status_code, DeepSeekErrorMessage(response.body));
            }
            if (parse_failed) {
                throw DeepSeekException(0, "failed to parse DeepSeek stream: " + parse_error);
            }
            const auto duration_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
            aiagent::DefaultLogger().Log(
                    aiagent::LogLevel::kInfo, "deepseek",
                    "mode=stream event=request_end status=200 duration_ms=" +
                            std::to_string(duration_ms) +
                            " delivered_events=" + (delivered_any ? "true" : "false"));
            return;
        } catch (const DeepSeekException& e) {
            const bool retry = !delivered_any &&
                               IsRetryableStatusCode(e.status_code()) &&
                               attempt < config_.max_retries;
            aiagent::DefaultLogger().Log(
                    retry ? aiagent::LogLevel::kWarning
                          : aiagent::LogLevel::kError,
                    "deepseek",
                    "mode=stream attempt=" + std::to_string(attempt) +
                            " event=error status=" + std::to_string(e.status_code()) +
                            " message=" + e.what());
            if (!retry) {
                throw;
            }
        } catch (const a2a::core::JsonRpcException& e) {
            const bool retry = !delivered_any && attempt < config_.max_retries;
            aiagent::DefaultLogger().Log(
                    retry ? aiagent::LogLevel::kWarning
                          : aiagent::LogLevel::kError,
                    "deepseek",
                    "mode=stream event=network_error message=" +
                            std::string(e.what()));
            if (!retry) {
                throw;
            }
        }
        SleepBackoff(attempt);
        ++attempt;
    }
}

}  // namespace aiagent::llm
