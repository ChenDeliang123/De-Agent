#pragma once

#include <json.hpp>

#include <atomic>
#include <deque>
#include <filesystem>
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
    ~ReviewTools() override;

    std::vector<llm::ToolSpec> ListTools() override;
    std::vector<llm::ToolSpec> ListToolsFor(const std::string& working_directory) override;
    tools::ToolExecutionResult Execute(const std::string& name,
                                       const nlohmann::json& arguments,
                                       const tools::ToolContext& context) override;

    // 父级 run summary 落盘到当前 review state，供 manifest/report/日志读取。
    // 无当前 pack 时静默忽略（例如非审核会话）。
    void RecordRunUsage(const ReviewUsage& usage);

    // 当前 reviewer 最近一次 review_context/report 使用的 pack（绝对路径）。
    std::string CurrentPackPath() const;

private:
    tools::ToolExecutionResult Handoff(const nlohmann::json& arguments,
                                       const tools::ToolContext& context);
    tools::ToolExecutionResult Context(const nlohmann::json& arguments,
                                       const tools::ToolContext& context);
    tools::ToolExecutionResult Read(const nlohmann::json& arguments,
                                    const tools::ToolContext& context);
    tools::ToolExecutionResult Git(const nlohmann::json& arguments,
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
    bool LoadPack(const std::string& pack_arg, const tools::ToolContext& context,
                  ReviewPack* pack, std::string* abs_path, std::string* rel_path,
                  std::string* error);

    struct PackCacheEntry {
        std::string path;
        std::filesystem::file_time_type mtime;
        std::uintmax_t size = 0;
        std::string relative_path;
        ReviewPack pack;
    };

    struct ReadCacheEntry {
        std::string path;
        std::filesystem::file_time_type mtime;
        std::uintmax_t size = 0;
        std::size_t bytes = 0;
        std::vector<std::string> lines;
    };

    bool LoadCachedLines(const std::string& path,
                         std::vector<std::string>* lines, bool* cache_hit,
                         std::string* error);
    bool EnsureCopyWorktree(const ReviewPack& pack, std::string* path,
                            std::string* error);
    void CleanupCopyWorktree();

    ReviewPackService pack_service_;
    mutable std::mutex last_pack_mutex_;
    std::atomic<long long> deadline_epoch_s_{0};
    mutable std::mutex cache_mutex_;
    mutable std::deque<PackCacheEntry> pack_cache_;
    mutable std::mutex read_cache_mutex_;
    mutable std::deque<ReadCacheEntry> read_cache_;
    mutable std::size_t read_cache_bytes_ = 0;
    mutable std::mutex copy_mutex_;
    std::string copy_worktree_path_;
    std::string copy_worktree_repo_;
    std::string copy_worktree_review_id_;
    // review_context/verify/report 记住当前 pack，供没有 pack 参数的
    // review_test_write 关联 state；多轮审核仍以显式 pack 参数为准。
    std::string last_pack_path_;
};

}  // namespace aiagent::review
