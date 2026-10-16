#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aiagent/agent/agent_event.hpp"
#include "aiagent/agent/compaction.hpp"

namespace a2a::server {
class AgentServer;
class ITaskStore;
}  // namespace a2a::server

namespace aiagent::agent {
class HistoryCache;
class SessionMetaStore;
}  // namespace aiagent::agent

namespace aiagent::web {

// 工具清单条目（GET /api/v1/tools）。
struct ToolInfo {
    std::string name;
    std::string description;
};

// 权限条目（GET /api/v1/permissions）。
struct PermissionInfo {
    std::string path;
    std::string mode;
};

// 权限快照：默认模式 + 全部显式配置。
struct PermissionsSnapshot {
    std::string default_mode;
    std::vector<PermissionInfo> entries;
};

// 项目级权限信息（GET /api/v1/permissions/project）。
struct ProjectPermissionInfo {
    std::string path;
    std::string root;
    bool has_config = false;
    bool trusted = false;
};

// 子 Agent 定义（GET /api/v1/agents 的 definitions 项）。
struct SubAgentInfo {
    std::string name;
    std::string description;
    std::string model;               // 空 = 继承父模型
    std::vector<std::string> tools;  // 空 = 继承父工具集
};

// 一条后台任务在接口上的表示（GET /api/v1/agents 的 running[]/pending_results[]）。
struct BackgroundTaskInfo {
    std::string task_id;         // bg-3
    std::string agent_id;        // explorer#7
    std::string agent_type;      // explorer
    std::string session_id;      // 派发它的父会话
    std::string state;           // running/done/failed/cancelled/timed_out
    long elapsed_ms = 0;
    std::string prompt_excerpt;  // running[] 用
    // pending_results[] 才有内容：结果与报告正文。
    bool ok = false;
    long duration_ms = 0;
    int tool_calls = 0;
    long long prompt_tokens = 0;
    long long completion_tokens = 0;
    long long cache_hit_tokens = 0;
    std::string text;
    std::string error;
};

// 子 Agent 运行时快照（GET /api/v1/agents）。
struct SubAgentSnapshot {
    bool enabled = false;
    int max_depth = 0;
    int max_concurrent = 0;
    // 累计统计：运行次数 / 失败次数 / 取消次数 / 总耗时。
    std::size_t runs = 0;
    std::size_t failures = 0;
    std::size_t cancelled = 0;
    long total_ms = 0;
    // 累计 token 用量（所有子 Agent 之和；cache_hit 是 prompt 的子集）。
    long long prompt_tokens = 0;
    long long completion_tokens = 0;
    long long cache_hit_tokens = 0;
    std::vector<SubAgentInfo> definitions;
    // 后台任务（M4c）：受理上限 + 运行中列表 + 未打印（未 ack）的终态结果。
    int max_background = 0;
    std::vector<BackgroundTaskInfo> running;
    std::vector<BackgroundTaskInfo> pending_results;
};

// 会话最近一次加载的项目指令来源（只含路径/大小，不含指令正文）。
struct InstructionSourceInfo {
    std::string path;
    std::size_t bytes = 0;
    bool truncated = false;
};

// 面向 Node/浏览器前端的 REST + SSE Gateway。
// 只做协议转换与会话编排，业务逻辑仍由 AgentServer/TaskManager 承担。
class WebGateway {
public:
    using TokenCallback = std::function<void(const std::string& token)>;
    using CancelCallback = std::function<bool()>;
    using StreamChatHandler =
            std::function<void(const std::string& session_id,
                               const std::string& message,
                               const TokenCallback& on_token)>;

    // 新：结构化事件版流式回调（token + tool_start/tool_output/tool_end）。
    using StreamEventHandler = std::function<void(
            const std::string& session_id,
            const std::string& message,
            const std::string& cwd,
            const aiagent::agent::AgentEventCallback& on_event,
            const CancelCallback& should_cancel)>;

    // 手动压缩某个会话（POST /api/v1/sessions/:id/compact）。
    // 返回 false 时写入 error。
    using CompactHandler =
            std::function<bool(const std::string& session_id,
                               const std::string& cwd,
                               std::string* error)>;

    // 查询某个会话最近一次压缩记录（GET /api/v1/sessions/:id/compaction）。
    // 返回 false 表示该会话没有记录。
    using CompactionInfoHandler =
            std::function<bool(const std::string& session_id,
                               aiagent::agent::CompactionRecord* record)>;

    // 删除会话（DELETE /api/v1/sessions/:id）。返回 false 时写入 error。
    using DeleteHandler =
            std::function<bool(const std::string& session_id, std::string* error)>;

    // 复制会话（POST /api/v1/sessions/:id/clone）。成功时写入新会话 id。
    using CloneHandler = std::function<bool(const std::string& source_id,
                                            std::string* new_id,
                                            std::string* error)>;

    // 工具清单（GET /api/v1/tools）。
    using ToolsHandler = std::function<std::vector<ToolInfo>()>;

    // 子 Agent 运行时快照（GET /api/v1/agents）。未设置时返回 enabled=false。
    // session_id 非空时，running[]/pending_results[] 只含该会话的后台任务。
    using AgentsInfoHandler = std::function<SubAgentSnapshot(const std::string& session_id)>;

    // 停止一条后台任务（POST /api/v1/agents/:id/stop）。返回 false → 404（id 未知）；
    // 否则 *requested 表示本次是否真的置了取消（false = 已结束）。
    using BackgroundStopHandler = std::function<bool(const std::string& id, bool* requested,
                                                     std::string* message)>;

    // 把已打印的后台结果标记为已送出（POST /api/v1/agents/ack）。
    using BackgroundAckHandler =
            std::function<std::size_t(const std::vector<std::string>& task_ids)>;

    // 向运行中的后台任务投递纠偏（POST /api/v1/agents/:id/steer，M5b）。
    enum class BackgroundSteerStatus { kAccepted, kNotFound, kFinished, kFull, kTooLarge };
    struct BackgroundSteerResult {
        BackgroundSteerStatus status = BackgroundSteerStatus::kNotFound;
        std::string message;  // 人类可读（直接回给调用方）
    };
    // session_id 为空表示不做会话校验；非空时由装配方比对（不匹配按 404 处理）。
    using BackgroundSteerHandler = std::function<BackgroundSteerResult(
            const std::string& id, const std::string& text, const std::string& session_id)>;

    // 权限清单（GET /api/v1/permissions）。
    using PermissionsListHandler = std::function<PermissionsSnapshot()>;

    // 解析某路径的有效权限（GET /api/v1/permissions/effective?path=）。
    // source 取 exact / inherited / default。
    using PermissionResolveHandler =
            std::function<bool(const std::string& path,
                               std::string* mode,
                               std::string* source)>;

    // 设置权限（PUT /api/v1/permissions）。成功时写回实际生效模式（可能被夹取）。
    using PermissionSetHandler =
            std::function<bool(const std::string& path,
                               const std::string& mode,
                               std::string* effective_mode,
                               std::string* error)>;

    // 删除权限（DELETE /api/v1/permissions?path=）。
    using PermissionRemoveHandler =
            std::function<bool(const std::string& path, std::string* error)>;

    // 项目级权限信息（GET /api/v1/permissions/project?path=）。
    using ProjectPermissionHandler =
            std::function<ProjectPermissionInfo(const std::string& path)>;

    // 设置项目信任（PUT /api/v1/permissions/trust）。
    using TrustSetHandler =
            std::function<bool(const std::string& path, bool trusted,
                               std::string* root, std::string* error)>;

    // 初始化项目 `.de/`（POST /api/v1/permissions/init）。
    using PermissionInitHandler = std::function<bool(
            const std::string& path, bool seed_from_global, std::string* root,
            std::vector<std::string>* created, std::string* error)>;

    // 工具审批（POST /api/v1/chat/approve）。
    // decision 取 allow_once / allow_session / deny；命中等待中的请求返回 true。
    using ApproveHandler = std::function<bool(const std::string& session_id,
                                              const std::string& tool_call_id,
                                              const std::string& decision,
                                              std::string* error)>;

    // 会话取消时的额外通知（用于唤醒阻塞中的审批等待）。
    using CancelObserver = std::function<void(const std::string& session_id)>;

    // 会话详情补充（GET /api/v1/sessions/:id/stats）：最近 cwd 与项目指令来源。
    // 返回 false 时 stats 不附带 cwd/instructions。
    using SessionDetailsHandler =
            std::function<bool(const std::string& session_id,
                               std::string* cwd,
                               std::vector<InstructionSourceInfo>* instructions)>;

    WebGateway(a2a::server::AgentServer& agent_server,
               a2a::server::ITaskStore& task_store);
    ~WebGateway();

    WebGateway(const WebGateway&) = delete;
    WebGateway& operator=(const WebGateway&) = delete;

    bool Start(const std::string& host, int port);
    void Stop();
    int bound_port() const noexcept;

    // 注入真正的逐 token 流式处理逻辑（由 main 接线到 AgentRuntime）。
    void SetStreamHandler(StreamChatHandler handler);

    // 注入结构化事件版流式处理逻辑（token + 工具事件）。设置后优先于
    // SetStreamHandler 使用；未设置时保留旧的 token-only 行为。
    void SetStreamEventHandler(StreamEventHandler handler);

    // 注入手动压缩处理逻辑。未设置时 /compact 接口返回 501。
    void SetCompactHandler(CompactHandler handler);

    // 注入会话详情补充逻辑（cwd / 项目指令来源）。
    void SetSessionDetailsHandler(SessionDetailsHandler handler);

    // 注入压缩记录查询逻辑。未设置时 /compaction 接口返回 404。
    void SetCompactionInfoHandler(CompactionInfoHandler handler);

    // 注入删除会话处理逻辑。未设置时 DELETE 接口返回 501。
    void SetDeleteHandler(DeleteHandler handler);

    // 注入复制会话处理逻辑。未设置时 clone 接口返回 501。
    void SetCloneHandler(CloneHandler handler);

    // 注入工具清单处理逻辑。未设置时 /tools 接口返回 501。
    void SetToolsHandler(ToolsHandler handler);

    // 注入子 Agent 快照处理逻辑。未设置时 /agents 接口返回 enabled=false。
    void SetAgentsInfoHandler(AgentsInfoHandler handler);

    // 注入后台任务停止/结果回执逻辑（未设置时对应接口返回 501）。
    void SetBackgroundStopHandler(BackgroundStopHandler handler);
    void SetBackgroundSteerHandler(BackgroundSteerHandler handler);
    void SetBackgroundAckHandler(BackgroundAckHandler handler);

    // 向某个会话的活跃 SSE 流推送一帧（后台任务的派发/结果通知）。
    // 无活跃流时丢弃（CLI 有轮询兜底）；线程安全，可在任意线程调用。
    void PublishSessionEvent(const std::string& session_id, const std::string& event,
                             const nlohmann::json& payload);

    // 注入权限相关处理逻辑。未设置时对应接口返回 501。
    void SetPermissionsListHandler(PermissionsListHandler handler);
    void SetPermissionResolveHandler(PermissionResolveHandler handler);
    void SetPermissionSetHandler(PermissionSetHandler handler);
    void SetPermissionRemoveHandler(PermissionRemoveHandler handler);
    void SetProjectPermissionHandler(ProjectPermissionHandler handler);
    void SetTrustSetHandler(TrustSetHandler handler);
    void SetPermissionInitHandler(PermissionInitHandler handler);

    // 注入工具审批处理逻辑。未设置时 /chat/approve 返回 501。
    void SetApproveHandler(ApproveHandler handler);

    // 注入取消观察者（可选）。每次 /chat/cancel 命中时调用，可用于唤醒
    // 阻塞中的审批等待。
    void SetCancelObserver(CancelObserver observer);

    // 注入会话历史内存缓存（可选）。命中后，读取会话历史/标题的 REST 接口
    // 不再每次全表扫描 task store；未注入时行为与旧实现一致。
    void SetHistoryCache(aiagent::agent::HistoryCache* history_cache);

    // 注入会话显式标题存储（可选）。注入后 /sessions 返回的 title 优先取
    // 显式标题，并启用 PATCH /api/v1/sessions/:id。
    void SetSessionMetaStore(aiagent::agent::SessionMetaStore* session_meta_store);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aiagent::web
