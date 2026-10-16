#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace aiagent::agent {

// steer 消息在模型上下文里的标记（日志/CLI 展示共用同一措辞，见
// docs/子Agent纠偏Steer规划.md 决策记录 ②）。
inline constexpr const char* kSteerMarker = "[steer]";

// 把一条 steer 文本包装成追加进模型上下文的消息：标记 + 一句"这不是新任务"的说明。
// 说明的目的：抑制子 Agent 把它当成新任务重开计划（子 Agent 的 system prompt 里
// 写的是"你被交给一个自包含的任务"，不解释会冲突）。
std::string FormatSteerMessage(const std::string& text);

// 运行中 Agent 的收件箱（M5a）：多生产者（接口/CLI 线程）单消费者（回合循环线程）。
//
// 不变式：
// - 有界：条数与单条字节都有上限，超限**拒绝**并回明确结果（不静默丢）；
// - 一次运行一个收件箱：运行结束（含异常/取消）由运行方 Close()，之后 Push 一律 kClosed；
// - 只在回合边界被 Drain()（见 AgentRuntime 的投递点）；投递回调在消费者线程上同步触发，
//   异常由本类吞掉（与 M4 的后台通知同约定）；
// - steer 不影响 timeout / max_rounds —— 闸门由调用方保持（收件箱只负责搬运消息）。
class MessageInbox {
public:
    struct Limits {
        std::size_t max_messages = 8;
        std::size_t max_text_bytes = 4096;
    };

    enum class PushResult {
        kAccepted,
        kFull,      // 条数已达上限
        kTooLarge,  // 空文本或超过单条字节上限（内容不合法）
        kClosed,    // 运行已结束
    };

    // 默认上限构造（8 条 / 4KB）：走 Limits 的默认成员初始化；显式上限走另一个重载。
    MessageInbox() = default;
    explicit MessageInbox(Limits limits);

    // 入队（任意线程）。纯空白文本按 kTooLarge 处理。
    PushResult Push(std::string text);

    // 取走全部待投递消息（单消费者）。Close 之后仍可取走剩余，便于统计未投递条数。
    std::vector<std::string> Drain();

    std::size_t Pending() const;
    bool Closed() const;

    // 投递观察（装配方用来发帧/记日志；可为空）。每条消息投递时同步回调一次。
    void SetDeliveryCallback(std::function<void(const std::string& text)> callback);

    // 运行结束时关闭：阻塞后续 Push（调用方据此报"任务已结束"）。
    void Close();

private:
    Limits limits_;
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
    bool closed_ = false;
    std::function<void(const std::string&)> delivery_callback_;
};

}  // namespace aiagent::agent
