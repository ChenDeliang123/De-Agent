#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "aiagent/config.hpp"

namespace aiagent::review {

struct ReviewJobProgress {
    int files_done = 0;
    int files_total = 0;
    int findings = 0;
    std::string current_command;
};

// 后台审核任务。状态机：
// queued -> running -> done|failed|cancelled|superseded|timed_out
//                  -> awaiting_approval（保留给后续 attach 审批）
// 后端重启时 running/awaiting_approval -> interrupted。
struct ReviewJob {
    std::string job_id;
    std::string review_id;
    std::string pack_path;  // 绝对路径
    std::string repo_root;
    std::string feature;
    std::string branch;
    std::string session_id;         // reviewer 会话 id（review-job-<job_id>）
    std::string parent_session_id;  // 可选：完成后通知的 coding 会话
    std::string state = "queued";
    ReviewJobProgress progress;
    std::string report_path;  // 相对 repo_root
    std::string verdict;
    std::string error;
    long long created_at = 0;
    long long updated_at = 0;
};

struct ReviewJobRunResult {
    bool ok = false;
    bool cancelled = false;
    bool timed_out = false;
    std::string error;
    std::string report_path;
    std::string verdict;
    int findings = 0;
};

class ReviewJobManager {
public:
    using CancelFlag = std::shared_ptr<std::atomic<bool>>;
    using Runner = std::function<ReviewJobRunResult(
            const ReviewJob& job, const std::function<bool()>& should_cancel,
            const std::function<void(const ReviewJobProgress&)>& on_progress)>;
    using CompletionCallback = std::function<void(const ReviewJob& job)>;

    // state_file 为空时退化为纯内存（测试）。
    ReviewJobManager(ReviewConfig config, std::string state_file, Runner runner);
    ~ReviewJobManager();

    ReviewJobManager(const ReviewJobManager&) = delete;
    ReviewJobManager& operator=(const ReviewJobManager&) = delete;

    // 提交一个 pack。失败时 error 写原因（pack 非法/队列满/禁用）。
    bool Submit(const std::string& pack_path, const std::string& parent_session_id,
                ReviewJob* job, std::string* error);

    std::vector<ReviewJob> List() const;
    std::optional<ReviewJob> Get(const std::string& job_id) const;

    bool Cancel(const std::string& job_id, std::string* error);
    // 返回 job 与报告正文（若已写出）。不改变任务状态。
    bool Attach(const std::string& job_id, ReviewJob* job, std::string* report_text,
                std::string* error) const;

    void SetCompletionCallback(CompletionCallback callback);
    void Shutdown();

    const ReviewConfig& config() const { return config_; }

private:
    bool PopRunnableLocked(ReviewJob* job);
    bool HasRunnableLocked() const;
    void WorkerLoop();
    void Load();
    void SaveLocked() const;
    void FinishLocked(const std::string& job_id, const ReviewJobRunResult& result);
    static std::string NewJobId();

    ReviewConfig config_;
    std::string state_file_;
    Runner runner_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, ReviewJob> jobs_;
    std::map<std::string, CancelFlag> cancel_flags_;
    std::deque<std::string> queue_;
    std::map<std::string, int> running_repos_;
    std::vector<std::thread> workers_;
    CompletionCallback completion_;
    bool stopping_ = false;
};

std::string ReviewJobStateName(const std::string& state);

}  // namespace aiagent::review
