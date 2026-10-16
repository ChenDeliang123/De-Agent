#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "a2a/models/message.hpp"
#include "a2a/server/task_store.hpp"

#include "aiagent/agent/history_cache.hpp"
#include "aiagent/agent/sub_agent.hpp"

namespace aiagent::agent {

// 后台结果 → 会话历史注入（docs/子Agent后台化与可干预规划.md §3.3 路径 2）。
//
// 语义与不变式：
// - 一条后台结果只注入一次：调用方在收敛通知里对每条任务调用一次 Deliver()；
// - 注入是 **user 角色 + 显式前缀** 的消息（`[sub-agent explorer#7 finished] …`），
//   不伪造 tool 消息——父上下文里的 tool_call 配对关系不受影响；
// - 与回合写库串行：回合进行中到达的结果先排队，下一个回合开始时（BeginTurn）
//   补写进最新历史。回合结束时会用自己那份 history 整体覆盖写，回合中途写进去
//   的注入会被吃掉（读改写竞态），所以必须排队；
// - 无活跃回合时立即注入：父下一回合读到的历史里必然有这条通知。
//
// 线程安全：内部互斥；Deliver 可能在后台 worker 线程上被调用。
class BackgroundResultInjector {
public:
    BackgroundResultInjector(a2a::server::ITaskStore& task_store, HistoryCache& history_cache);

    // 回合边界：回合开始时 BeginTurn（会先补写排队中的注入，再让调用方读历史）；
    // 回合结束时 EndTurn。
    void BeginTurn(const std::string& session_id);
    void EndTurn(const std::string& session_id);

    // 该会话当前是否有活跃回合（回合外审批策略用：无回合时直接拒绝，见决策 ②）。
    bool TurnActive(const std::string& session_id) const;

    // 注入一条后台结果；session_id 为空时不做任何事并返回 false。
    // 有活跃回合时排队，否则立即写入会话历史。
    bool Deliver(const BackgroundTaskSnapshot& snapshot);

    // 注入消息的标题行（注入正文与 CLI 打印共用同一措辞）。
    static std::string FormatHeadline(const BackgroundTaskSnapshot& snapshot);
    // 注入消息全文 = 标题行 + 报告正文（失败时标题行已带原因）+ 免回复说明。
    static std::string FormatMessage(const BackgroundTaskSnapshot& snapshot);

private:
    // 追加一条消息到该会话最新历史并落库（task_store + history_cache 同写）。
    void AppendLocked(const std::string& session_id, const a2a::models::Message& message);

    a2a::server::ITaskStore& task_store_;
    HistoryCache& history_cache_;

    mutable std::mutex mutex_;
    std::map<std::string, int> active_turns_;  // session → 活跃回合数
    std::map<std::string, std::vector<a2a::models::Message>> queued_;  // 回合中到达的注入
};

// 回合边界守卫：按作用域 BeginTurn/EndTurn。回合中途抛异常也必须 EndTurn，
// 否则该会话永远算"回合进行中"，后台结果会一直排队不注入。
class BackgroundTurnGuard {
public:
    BackgroundTurnGuard(BackgroundResultInjector* injector, std::string session_id)
        : injector_(injector), session_id_(std::move(session_id)) {
        if (injector_ != nullptr) {
            injector_->BeginTurn(session_id_);
        }
    }

    ~BackgroundTurnGuard() {
        if (injector_ != nullptr) {
            injector_->EndTurn(session_id_);
        }
    }

    BackgroundTurnGuard(const BackgroundTurnGuard&) = delete;
    BackgroundTurnGuard& operator=(const BackgroundTurnGuard&) = delete;

private:
    BackgroundResultInjector* injector_ = nullptr;
    std::string session_id_;
};

}  // namespace aiagent::agent
