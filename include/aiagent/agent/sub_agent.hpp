#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
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
