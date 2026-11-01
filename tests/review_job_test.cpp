#include <json.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "aiagent/review/review_job.hpp"

#include "review_test_support.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

std::string PackJson(const std::string& feature, const std::string& review_id,
                     const std::string& repo) {
    return nlohmann::json{{"schema", 1},
                          {"review_id", review_id},
                          {"feature", feature},
                          {"repo_root", repo},
                          {"git", {{"branch", "main"}}}}
            .dump(2);
}

bool WaitForState(aiagent::review::ReviewJobManager& manager,
                  const std::string& job_id, const std::string& state,
                  int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto job = manager.Get(job_id); job.has_value() && job->state == state) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

struct RunnerState {
    std::mutex mutex;
    std::condition_variable cv;
    bool release = false;
    int started = 0;
    std::vector<std::string> completed;
};

}  // namespace

int main() {
    namespace review = aiagent::review;
    try {
        const std::string repo = review_test_support::UniqueDir("review_job_repo_");
        std::filesystem::create_directories(repo + "/docs/review/inbox");
        const std::string pack_a = repo + "/docs/review/inbox/a.json";
        const std::string pack_b = repo + "/docs/review/inbox/b.json";
        const std::string pack_c = repo + "/docs/review/inbox/c.json";
        const std::string pack_fail = repo + "/docs/review/inbox/fail.json";
        review_test_support::WriteFile(pack_a, PackJson("a", "rv-a", repo));
        review_test_support::WriteFile(pack_b, PackJson("b", "rv-b", repo));
        review_test_support::WriteFile(pack_c, PackJson("c", "rv-c", repo));
        review_test_support::WriteFile(pack_fail, PackJson("fail", "rv-fail", repo));
        const std::string state_file = repo + "/review_jobs.json";

        auto runner_state = std::make_shared<RunnerState>();
        aiagent::ReviewConfig job_config;
        job_config.background_enabled = true;
        job_config.max_concurrent_jobs = 1;
        job_config.max_queued_jobs = 1;
        review::ReviewJobManager manager(
                job_config,
                state_file,
                [runner_state](const review::ReviewJob& job,
                               const std::function<bool()>& should_cancel,
                               const std::function<void(const review::ReviewJobProgress&)>&
                                       on_progress) {
                    {
                        std::unique_lock<std::mutex> lock(runner_state->mutex);
                        ++runner_state->started;
                        on_progress({1, 1, 0, "ctest"});
                        while (!runner_state->release && !should_cancel()) {
                            runner_state->cv.wait_for(lock, std::chrono::milliseconds(10));
                        }
                    }
                    review::ReviewJobRunResult result;
                    if (should_cancel()) {
                        result.cancelled = true;
                        result.error = "cancelled";
                        return result;
                    }
                    if (job.feature == "fail") {
                        result.error = "runner exploded";
                        return result;
                    }
                    result.ok = true;
                    result.report_path = "docs/review/" + job.feature + ".md";
                    result.verdict = "approved";
                    result.findings = 0;
                    review_test_support::WriteFile(
                            job.repo_root + "/" + result.report_path,
                            "---\nverdict: approved\n---\n# review\n");
                    {
                        std::lock_guard<std::mutex> lock(runner_state->mutex);
                        runner_state->completed.push_back(job.job_id);
                    }
                    return result;
                });

        std::mutex completed_mutex;
        std::vector<review::ReviewJob> completed_jobs;
        manager.SetCompletionCallback([&](const review::ReviewJob& job) {
            std::lock_guard<std::mutex> lock(completed_mutex);
            completed_jobs.push_back(job);
        });

        review::ReviewJob job_a;
        std::string error;
        Expect(manager.Submit(pack_a, "coding-session", &job_a, &error),
               "submit a: " + error);
        Expect(WaitForState(manager, job_a.job_id, "running"), "job a running");

        review::ReviewJob job_b;
        Expect(manager.Submit(pack_b, "", &job_b, &error), "submit b: " + error);
        Expect(WaitForState(manager, job_b.job_id, "queued"), "job b queued");

        review::ReviewJob job_c;
        Expect(!manager.Submit(pack_c, "", &job_c, &error) &&
                       error.find("queue is full") != std::string::npos,
               "queue limit enforced: " + error);

        Expect(manager.Cancel(job_b.job_id, &error), "cancel queued b: " + error);
        Expect(WaitForState(manager, job_b.job_id, "cancelled"), "b cancelled");

        // 同一 repo+feature+branch 的新 pack 顶掉正在运行的旧任务。
        review::ReviewJob job_a2;
        Expect(manager.Submit(pack_a, "", &job_a2, &error), "submit a2: " + error);
        Expect(WaitForState(manager, job_a.job_id, "superseded"),
               "old a superseded");
        Expect(WaitForState(manager, job_a2.job_id, "running"), "a2 running");
        Expect(manager.Cancel(job_a2.job_id, &error), "cancel a2: " + error);
        Expect(WaitForState(manager, job_a2.job_id, "cancelled"), "a2 cancelled");

        // 释放 runner，跑一个正常完成的任务并 attach 报告。
        {
            std::lock_guard<std::mutex> lock(runner_state->mutex);
            runner_state->release = true;
        }
        runner_state->cv.notify_all();
        review::ReviewJob done;
        Expect(manager.Submit(pack_c, "", &done, &error), "submit done: " + error);
        Expect(WaitForState(manager, done.job_id, "done"), "job done");
        review::ReviewJob attached;
        std::string report;
        Expect(manager.Attach(done.job_id, &attached, &report, &error),
               "attach done: " + error);
        Expect(attached.state == "done" && attached.verdict == "approved",
               "attached job state/verdict");
        Expect(report.find("verdict: approved") != std::string::npos,
               "attached report text");

        review::ReviewJob failed;
        Expect(manager.Submit(pack_fail, "", &failed, &error), "submit fail: " + error);
        Expect(WaitForState(manager, failed.job_id, "failed"), "job failed");
        const auto failed_job = manager.Get(failed.job_id);
        Expect(failed_job.has_value() && failed_job->error == "runner exploded",
               "failed job keeps error: " +
                       (failed_job.has_value() ? failed_job->error : std::string()));

        const auto list = manager.List();
        Expect(!list.empty(), "job list non-empty");
        {
            std::lock_guard<std::mutex> lock(completed_mutex);
            Expect(completed_jobs.size() >= 3, "completion callback fired");
        }
        manager.Shutdown();

        // 重启：running -> interrupted。
        {
            const std::string reload_state = repo + "/reload_jobs.json";
            nlohmann::json reload_json;
            reload_json["jobs"] = nlohmann::json::array();
            reload_json["jobs"].push_back({{"job_id", "rvjob-old"},
                                           {"review_id", "rv-old"},
                                           {"repo_root", repo},
                                           {"feature", "old"},
                                           {"state", "running"},
                                           {"created_at", 1},
                                           {"updated_at", 1}});
            review_test_support::WriteFile(reload_state, reload_json.dump(2));
            review::ReviewJobManager reloaded(
                    aiagent::ReviewConfig{}, reload_state,
                    [](const review::ReviewJob&,
                       const std::function<bool()>&,
                       const std::function<void(const review::ReviewJobProgress&)>&) {
                        return review::ReviewJobRunResult{};
                    });
            const auto old = reloaded.Get("rvjob-old");
            Expect(old.has_value() && old->state == "interrupted",
                   "running job becomes interrupted after reload");
            reloaded.Shutdown();
        }

        // background_enabled=false 时拒绝提交。
        {
            aiagent::ReviewConfig disabled;
            disabled.background_enabled = false;
            review::ReviewJobManager memory(disabled, "",
                                            [](const review::ReviewJob&,
                                               const std::function<bool()>&,
                                               const std::function<void(
                                                       const review::ReviewJobProgress&)>&) {
                                                return review::ReviewJobRunResult{};
                                            });
            review::ReviewJob rejected;
            Expect(!memory.Submit(pack_a, "", &rejected, &error) &&
                           error.find("disabled") != std::string::npos,
                   "background disabled rejected");
            memory.Shutdown();
        }

        std::filesystem::remove_all(repo);
    } catch (const std::exception& e) {
        std::cerr << "review job test failed: " << e.what() << '\n';
        return 1;
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " review job test(s) failed\n";
        return 1;
    }
    std::cout << "review job test passed\n";
    return 0;
}
