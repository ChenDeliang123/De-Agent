#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "aiagent/agent/agent_event.hpp"
#include "aiagent/agent/agent_runtime.hpp"
#include "aiagent/config.hpp"
#include "aiagent/tools/tool_provider.hpp"

namespace aiagent::agent {

// 一个可派发的子 Agent 定义。system_prompt 是「已解析的正文」，
// 提示词文件 → 正文的加载在 LoadAgentDefinitions 完成。
struct AgentDefinition {
    std::string name;
    std::string description;
    std::string system_prompt;
    // 空 = settings.default_model；再空 = 继承父模型。
    std::string model;
    // 工具白名单；空 = 继承父工具集（仍会剔除 task，除非深度允许）。
    std::vector<std::string> tools;
};

// 一次子 Agent 派发请求（由 task 工具从 ToolContext + 参数组装）。
struct SubAgentRequest {
    std::string agent_type = "general";
    std::string prompt;
    std::string model;          // 覆盖定义与 settings.default_model
    long timeout_seconds = 0;   // 0 = settings.default_timeout_s
    // 父运行上下文（任务归属、审批会话键、事件转发）。
    std::string parent_session_id;
    std::string working_directory;
    std::string parent_tool_call_id;
    AgentEventCallback emit;             // 可空：为空时子 Agent 事件不转发（静默汇总）
    std::function<bool()> should_cancel;  // 可空
};

// 一次子 Agent 运行结果。
struct SubAgentResult {
    bool ok = false;
    bool cancelled = false;
    bool timed_out = false;
    std::string agent_id;   // 如 explorer#3
    std::string text;       // 已按 result_max_chars 截断的报告正文
    std::string error;
    long duration_ms = 0;
    int tool_calls = 0;
    // 用量归集（口径与 deepseek 客户端的 request_end 日志一致）：
    // cache_hit_tokens 是 prompt_tokens 的子集。
    long long prompt_tokens = 0;
    long long completion_tokens = 0;
    long long cache_hit_tokens = 0;
};

// 运行统计（/agents 与日志用）。
struct SubAgentStats {
    std::size_t runs = 0;
    std::size_t failures = 0;
    std::size_t cancelled = 0;
    long total_ms = 0;
    // 累计用量（所有子 Agent 之和）。
    long long prompt_tokens = 0;
    long long completion_tokens = 0;
    long long cache_hit_tokens = 0;
};

// 后台任务状态机（见 docs/子Agent后台化与可干预规划.md §3.1）：
// running → done / failed / cancelled / timed_out，终态后不再变化。
enum class BackgroundState { kRunning, kDone, kFailed, kCancelled, kTimedOut };

// 状态名（日志、错误文本与 CLI 展示共用）。
const char* BackgroundStateName(BackgroundState state);

// 一条后台任务的对外快照：字段是取样时刻的副本，改动不回流。
struct BackgroundTaskSnapshot {
    std::string task_id;         // bg-3（父可见句柄）
    std::string agent_id;        // explorer#7（派发时即分配，与前台同一套编号）
    std::string agent_type;      // explorer
    std::string session_id;      // 派发它的父会话（结果归属；M4c 会话级取消按它过滤）
    BackgroundState state = BackgroundState::kRunning;
    std::string prompt_excerpt;  // 派发提示词前 80 字节（UTF-8 边界对齐）
    std::chrono::steady_clock::time_point started;  // 派发时刻（列表按它排序）
    long elapsed_ms = 0;         // 运行中 = 已耗时（含等槽位）；终态 = 实际运行耗时
    SubAgentResult result;       // 终态填充；运行中为空
};

// 后台派发的启动结果；ok=false 时没有创建任务，error 说明拒绝原因。
struct BackgroundStartResult {
    bool ok = false;
    std::string task_id;
    std::string agent_id;
    std::string error;
};

// Stop() 的结果：已置取消 / 未知 id / 已结束（终态不可再取消）。
enum class BackgroundStopOutcome { kRequested, kNotFound, kFinished };

// 多 Agent 协同的核心：定义装载、并发闸门、深度控制、超时/取消、结果截断、
// 事件归属转发。
//
// 不变式（见 docs/多Agent协同规划.md §3.6）：
// - 所有派发都经过 Run()；未来并列远程后端只替换这一处；
// - 对外只暴露「按定义跑一次子任务，返回结果文本 + 元数据」，不泄漏
//   in-process 细节（不返回 AgentRuntime、不暴露线程语义）。
class SubAgentManager : public std::enable_shared_from_this<SubAgentManager> {
public:
    // 子 runtime 的构建参数：模型与工具集由 Manager 决定，其余依赖（权限、
    // 审批、压缩、工作目录）由 factory 注入，Manager 不感知。
    struct ChildRuntimeSpec {
        std::string model;                  // 已解析（可能为空 = 继承父）
        std::shared_ptr<tools::IToolProvider> tools;
        std::string system_prompt;
        std::string child_context_id;       // 子会话 id
        std::string parent_session_id;      // 审批 session 键
        std::string tool_id_prefix;         // 事件与审批共用的 tool_call_id 前缀
        std::string working_directory;
        int max_tool_rounds = 0;
        // 该子 Agent 是否也拿到了 task 工具（depth+1 < max_depth）；决定是否注入派发准则。
        bool delegation_enabled = false;
        // 用量归集 sink：装配方创建子模型客户端时套 UsageTrackingModel 并转发到这里。
        // 与「会话 usage」同一口径（prompt/completion/cache_hit），用于统计与日志。
        std::function<void(const llm::ChatUsage&)> on_usage;
    };
    using ChildRuntimeFactory =
            std::function<std::shared_ptr<AgentRuntime>(const ChildRuntimeSpec& spec)>;

    SubAgentManager(AgentsConfig settings, std::vector<AgentDefinition> definitions,
                    ChildRuntimeFactory runtime_factory,
                    std::shared_ptr<tools::IToolProvider> parent_tools);

    const AgentsConfig& Settings() const { return settings_; }
    const std::vector<AgentDefinition>& Definitions() const { return definitions_; }
    const AgentDefinition* FindDefinition(const std::string& name) const;

    // 是否可用：开关打开、有定义、factory 与父工具集都已注入。
    bool Enabled() const;

    // 单发派发：阻塞到子 Agent 结束/取消/超时。depth = 请求方（父）的深度，
    // 根 Agent 的 task 工具 depth=0；子 Agent 深度 = depth+1。
    SubAgentResult Run(const SubAgentRequest& request, int depth);

    // 批量扇出：并发受 max_concurrent 闸门；返回顺序与输入一致。
    std::vector<SubAgentResult> RunBatch(const std::vector<SubAgentRequest>& requests, int depth);

    // ---- 后台任务（M4a：状态机/结果表/停止/关闭；送达与接口接入见 M4b/M4c）----

    // 后台派发：立即返回句柄（bg-N），子 Agent 在独立线程里跑，结果写进结果表。
    // 拒绝路径（不创建任务，error 说明原因）：子 Agent 关闭 / 未知类型 / 深度上限 /
    // 空 prompt / max_background=0 / 未收敛的后台任务已达 max_background（直接拒绝，
    // 不排队）/ Manager 正在关闭。
    // 后台任务不继承请求里的 emit 与 should_cancel：二者都是回合作用域的，回合结束
    // 后调用即悬垂（后台任务按要求跨回合存活）。终止只走 Stop() 与 ShutdownBackground()；
    // 会话级取消联动在 M4c 接入（按快照里的 session_id 过滤）。
    BackgroundStartResult RunBackground(const SubAgentRequest& request, int depth);

    // 查询一条后台任务（id 为 bg-N 或 agent_id）；未知（含已被回收）返回 nullopt。
    std::optional<BackgroundTaskSnapshot> PollResult(const std::string& id) const;

    // 请求停止一条后台任务：置取消标志，子 Agent 在下一个检查点退出（与超时/父取消
    // 同一条通道）。message 非空时写入人类可读结果。
    BackgroundStopOutcome Stop(const std::string& id, std::string* message = nullptr);

    // 运行中的后台任务快照，按派发顺序（含正在等 max_concurrent 槽位的任务）。
    std::vector<BackgroundTaskSnapshot> RunningSnapshot() const;

    // 结果表里全部未回收的任务快照（含终态），按派发顺序。
    std::vector<BackgroundTaskSnapshot> BackgroundSnapshot() const;

    // 关闭：拒绝新的后台任务，取消所有未收敛任务，等到结果就绪（有界）。
    // 线程在结果就绪时自行 detach，因此这里不 join；超时只记日志并返回，不阻塞退出。
    void ShutdownBackground(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    // 结果表保留的已完成任务上限（超出按完成顺序回收最旧的，避免无界增长）。
    static constexpr std::size_t kBackgroundResultRetention = 20;

    SubAgentStats Stats() const;

private:
    // 持有并发名额的 RAII 租约：任何退出路径（含异常）都会释放名额。
    class SlotLease {
    public:
        explicit SlotLease(SubAgentManager* manager) : manager_(manager) {}
        ~SlotLease() { manager_->ReleaseSlot(); }

        SlotLease(const SlotLease&) = delete;
        SlotLease& operator=(const SlotLease&) = delete;

    private:
        SubAgentManager* manager_;
    };

    // 子 Agent 的工具集：父工具集 ∩ 定义白名单，剔除 task；
    // 若 depth+1 < max_depth 再叠加一个 depth+1 的 task 工具（受控递归）。
    std::shared_ptr<tools::IToolProvider> BuildChildTools(const AgentDefinition& definition,
                                                          int depth);

    bool AcquireSlot(const std::function<bool()>& should_cancel, SubAgentResult* result);
    void ReleaseSlot();

    // 一次派发的运行身份：agent_id / 工具事件前缀 / 子上下文 id 由同一个递增序号
    // 派生，前台后台共用（编号全局唯一）。
    struct RunIdentity {
        std::string agent_id;        // explorer#7
        std::string tool_prefix;     // sub7:
        std::string child_context;   // <parent session>#explorer#7
    };

    // 结果表记录：只存在于 .cpp，对外只暴露 BackgroundTaskSnapshot。
    struct BackgroundTask;

    // 请求的静态校验（开关/定义/深度/提示词/装配）。通过时 *definition 指向
    // definitions_ 中的定义（生命周期内稳定）；否则返回拒绝原因。
    std::optional<std::string> ValidateRequest(const SubAgentRequest& request, int depth,
                                               const AgentDefinition** definition) const;

    RunIdentity MakeRunIdentity(const AgentDefinition& definition,
                                const SubAgentRequest& request);

    // 校验通过后的实际运行；身份由调用方给定（前台 = Run，后台 = RunBackground）。
    SubAgentResult RunWithIdentity(const SubAgentRequest& request, int depth,
                                   const AgentDefinition& definition,
                                   const RunIdentity& identity);

    // 结果表（background_mutex_ 保护）。
    const BackgroundTask* FindBackgroundLocked(const std::string& id) const;
    BackgroundTaskSnapshot SnapshotLocked(const BackgroundTask& task) const;

    // 后台任务收敛：写结果与终态、置 delivered（ShutdownBackground 的有界等待以它
    // 为准）、detach 自己的线程句柄（记录被回收时不能再持有可 join 句柄）、
    // 按完成顺序回收超限的已完成记录。
    void ConvergeBackground(const std::shared_ptr<BackgroundTask>& task,
                            const SubAgentResult& result);

    AgentsConfig settings_;
    std::vector<AgentDefinition> definitions_;
    ChildRuntimeFactory runtime_factory_;
    std::shared_ptr<tools::IToolProvider> parent_tools_;

    mutable std::mutex gate_mutex_;
    std::condition_variable gate_cv_;
    int active_ = 0;

    mutable std::mutex stats_mutex_;
    SubAgentStats stats_;

    std::atomic<unsigned long long> run_counter_{0};

    mutable std::mutex background_mutex_;
    std::condition_variable background_cv_;
    std::map<std::string, std::shared_ptr<BackgroundTask>> background_tasks_;
    std::deque<std::string> background_completion_order_;  // 回收顺序（最旧在前）
    // 关闭标志：RunBackground 据此拒绝新任务；在跑任务把它并入取消谓词。
    // shared_ptr 让 worker 无需持锁即可读到（与 Manager 生命周期解耦）。
    std::shared_ptr<std::atomic<bool>> background_shutdown_ =
            std::make_shared<std::atomic<bool>>(false);
    std::size_t background_running_ = 0;  // 已派发未收敛（含等槽位）的后台任务数
    std::atomic<unsigned long long> background_counter_{0};
};

// 定义装载结果：可用定义 + 因缺提示词被跳过的内置定义名。
struct AgentDefinitionLoadResult {
    std::vector<AgentDefinition> definitions;
    // 内置定义缺提示词（安装不完整）时跳过并记录，由调用方决定是否降级为
    // 「关闭子 Agent」；用户显式配置的定义缺提示词则直接报错（error 返回空）。
    std::vector<std::string> skipped_builtins;
};

// 从 [agents] 配置 + 提示词文件解析运行期定义。
//
// - 内置目录：general（继承父工具集，提示词 prompts/general.md）、
//   explorer（read/grep/glob/ls，提示词 prompts/explorer.md）；
// - [agents.<name>] 按名字覆盖或追加内置定义；
// - 提示词解析：system_prompt_file 含 '/' 时按显式路径（绝对或相对 cwd）；
//   否则依次在 prompt_dirs 中查找 <file>；内置定义的文件名固定为 <name>.md。
//   内置定义找不到文件 → 记入 skipped_builtins（安装不完整，可降级）；
//   配置的定义找不到文件 → 返回 nullopt 并在 error 中写明搜索过的路径。
std::optional<AgentDefinitionLoadResult> LoadAgentDefinitions(
        const AgentsConfig& config, const std::vector<std::string>& prompt_dirs,
        std::string* error);

}  // namespace aiagent::agent
