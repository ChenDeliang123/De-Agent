#pragma once

#include <json.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "aiagent/config.hpp"
#include "aiagent/review/review_pack.hpp"
#include "aiagent/review/review_state.hpp"
#include "aiagent/tools/tool_provider.hpp"

namespace aiagent::review {

struct ReviewToolOptions {
    ReviewConfig config;
    std::string data_dir;
};

// 审核工具提供者。Mode::kCodingHandoff 只暴露 review_handoff（挂在 coding
// 根工具集）；Mode::kReviewer 暴露审核热路径工具（挂在 reviewer runtime）。
class ReviewTools final : public tools::IToolProvider {
public:
    enum class Mode { kCodingHandoff, kReviewer };

    // 返回 false 表示会话不存在；成功时写 title、user_messages、message_count。
    using HistoryProvider = std::function<bool(const std::string& session_id,
                                               std::string* title,
                                               std::vector<std::string>* user_messages,
                                               int* message_count)>;

    ReviewTools(ReviewToolOptions options, Mode mode,
                HistoryProvider history_provider = {});

    std::vector<llm::ToolSpec> ListTools() override;
    std::vector<llm::ToolSpec> ListToolsFor(const std::string& working_directory) override;
    tools::ToolExecutionResult Execute(const std::string& name,
                                       const nlohmann::json& arguments,
                                       const tools::ToolContext& context) override;

private:
    tools::ToolExecutionResult Handoff(const nlohmann::json& arguments,
                                       const tools::ToolContext& context);
    tools::ToolExecutionResult Context(const nlohmann::json& arguments,
                                       const tools::ToolContext& context);
    tools::ToolExecutionResult Read(const nlohmann::json& arguments,
                                    const tools::ToolContext& context);
    tools::ToolExecutionResult Verify(const nlohmann::json& arguments,
                                      const tools::ToolContext& context);
    tools::ToolExecutionResult TestWrite(const nlohmann::json& arguments,
                                         const tools::ToolContext& context);
    tools::ToolExecutionResult Setup(const nlohmann::json& arguments,
                                     const tools::ToolContext& context);
    tools::ToolExecutionResult Report(const nlohmann::json& arguments,
                                      const tools::ToolContext& context);

    ReviewToolOptions options_;
    Mode mode_;
    HistoryProvider history_provider_;
    void EnsureClock(const ReviewPack& pack, const std::string& pack_relative,
                     ReviewState* state);
    bool DeadlineExceeded() const;

    ReviewPackService pack_service_;
    mutable std::mutex last_pack_mutex_;
    std::atomic<long long> deadline_epoch_s_{0};
    // review_context/verify/report 记住当前 pack，供没有 pack 参数的
    // review_test_write 关联 state；多轮审核仍以显式 pack 参数为准。
    std::string last_pack_path_;
};

}  // namespace aiagent::review
