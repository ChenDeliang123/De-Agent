#include "aiagent/agent/message_inbox.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "aiagent/log.hpp"

namespace aiagent::agent {
namespace {

}  // namespace

bool IsSteerTextEmpty(const std::string& text) {
    return text.empty() || std::all_of(text.begin(), text.end(), [](unsigned char ch) {
               return std::isspace(ch) != 0;
           });
}

std::string FormatSteerMessage(const std::string& text) {
    std::string message = std::string(kSteerMarker) + " " + text;
    message +=
            "\n\n(This is a mid-task correction from the delegating side; it refines the task "
            "already in progress. Keep the work you have done and fold it in — do not restart "
            "your plan.)";
    return message;
}

MessageInbox::MessageInbox(Limits limits) : limits_(limits) {}

MessageInbox::PushResult MessageInbox::Push(std::string text) {
    if (IsSteerTextEmpty(text) || text.size() > limits_.max_text_bytes) {
        return PushResult::kTooLarge;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return PushResult::kClosed;
    }
    if (messages_.size() >= limits_.max_messages) {
        return PushResult::kFull;
    }
    messages_.push_back(std::move(text));
    return PushResult::kAccepted;
}

std::vector<std::string> MessageInbox::Drain() {
    std::vector<std::string> drained;
    std::function<void(const std::string&)> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drained.swap(messages_);
        callback = delivery_callback_;
    }
    if (callback) {
        for (const auto& text : drained) {
            try {
                callback(text);
            } catch (const std::exception& e) {
                // 观察者失败不能影响投递：消息已经被取走去喂模型了。
                aiagent::DefaultLogger().Log(
                        aiagent::LogLevel::kWarning, "agent_runtime",
                        "event=steer_delivery_callback_failed message=" + std::string(e.what()));
            } catch (...) {
                aiagent::DefaultLogger().Log(
                        aiagent::LogLevel::kWarning, "agent_runtime",
                        "event=steer_delivery_callback_failed message=unknown error");
            }
        }
    }
    return drained;
}

std::size_t MessageInbox::Pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_.size();
}

bool MessageInbox::Closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

void MessageInbox::SetDeliveryCallback(std::function<void(const std::string& text)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    delivery_callback_ = std::move(callback);
}

void MessageInbox::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
}

}  // namespace aiagent::agent
