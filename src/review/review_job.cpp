#include "aiagent/review/review_job.hpp"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "aiagent/platform/paths.hpp"
#include "aiagent/platform/text_file.hpp"
#include "aiagent/review/project_verify_config.hpp"
#include "aiagent/review/review_pack.hpp"

namespace aiagent::review {
namespace {

namespace fs = std::filesystem;

long long NowSeconds() {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

nlohmann::json ProgressToJson(const ReviewJobProgress& progress) {
    return {{"files_done", progress.files_done},
            {"files_total", progress.files_total},
            {"findings", progress.findings},
            {"current_command", progress.current_command}};
}

nlohmann::json JobToJson(const ReviewJob& job) {
    return {{"job_id", job.job_id},
            {"review_id", job.review_id},
            {"pack_path", job.pack_path},
            {"repo_root", job.repo_root},
            {"feature", job.feature},
            {"branch", job.branch},
            {"session_id", job.session_id},
            {"parent_session_id", job.parent_session_id},
            {"state", job.state},
            {"progress", ProgressToJson(job.progress)},
            {"report_path", job.report_path},
            {"verdict", job.verdict},
            {"error", job.error},
            {"created_at", job.created_at},
            {"updated_at", job.updated_at}};
}

bool JobFromJson(const nlohmann::json& json, ReviewJob* job) {
    if (!json.is_object()) {
        return false;
    }
    job->job_id = json.value("job_id", std::string());
    if (job->job_id.empty()) {
        return false;
    }
    job->review_id = json.value("review_id", std::string());
    job->pack_path = json.value("pack_path", std::string());
    job->repo_root = json.value("repo_root", std::string());
    job->feature = json.value("feature", std::string());
    job->branch = json.value("branch", std::string());
    job->session_id = json.value("session_id", std::string());
    job->parent_session_id = json.value("parent_session_id", std::string());
    job->state = json.value("state", std::string("queued"));
    job->report_path = json.value("report_path", std::string());
    job->verdict = json.value("verdict", std::string());
    job->error = json.value("error", std::string());
    job->created_at = json.value("created_at", 0LL);
    job->updated_at = json.value("updated_at", 0LL);
    if (json.contains("progress") && json["progress"].is_object()) {
        job->progress.files_done = json["progress"].value("files_done", 0);
        job->progress.files_total = json["progress"].value("files_total", 0);
        job->progress.findings = json["progress"].value("findings", 0);
        job->progress.current_command =
                json["progress"].value("current_command", std::string());
    }
    return true;
}

bool WriteAtomic(const std::string& path, const std::string& content,
                 std::string* error) {
    if (path.empty()) {
        return true;
    }
    const fs::path target = aiagent::platform::FromUtf8(path);
    std::error_code ec;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
    }
    const fs::path temp = aiagent::platform::FromUtf8(path + ".tmp");
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error != nullptr) {
                *error = "cannot write review jobs file";
            }
            return false;
        }
        output << content;
        if (!output.good()) {
            if (error != nullptr) {
                *error = "cannot write review jobs file";
            }
            return false;
        }
    }
    fs::rename(temp, target, ec);
    if (ec) {
        fs::remove(target, ec);
        fs::rename(temp, target, ec);
    }
    if (ec) {
        if (error != nullptr) {
            *error = "cannot replace review jobs file";
        }
        return false;
    }
    return true;
}

bool IsTerminalState(const std::string& state) {
    return state == "done" || state == "failed" || state == "cancelled" ||
           state == "superseded" || state == "timed_out" || state == "interrupted";
}

}  // namespace

std::string ReviewJobStateName(const std::string& state) {
    return state;
}

std::string ReviewJobManager::NewJobId() {
    const std::string review_id = NewReviewId();
    return "rvjob-" + review_id.substr(3);
}

ReviewJobManager::ReviewJobManager(ReviewConfig config, std::string state_file,
                                   Runner runner)
        : config_(std::move(config)),
          state_file_(std::move(state_file)),
          runner_(std::move(runner)) {
    Load();
    const int workers = std::clamp(config_.max_concurrent_jobs, 1, 8);
    for (int i = 0; i < workers; ++i) {
        workers_.emplace_back([this] { WorkerLoop(); });
    }
}

ReviewJobManager::~ReviewJobManager() {
    Shutdown();
}

void ReviewJobManager::Load() {
    if (state_file_.empty()) {
        return;
    }
    std::ifstream input(aiagent::platform::FromUtf8(state_file_));
    if (!input) {
        return;
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    try {
        const auto json = nlohmann::json::parse(buffer.str());
        if (!json.is_object() || !json.contains("jobs") || !json["jobs"].is_array()) {
            return;
        }
        for (const auto& item : json["jobs"]) {
            ReviewJob job;
            if (!JobFromJson(item, &job)) {
                continue;
            }
            if (job.state == "running" || job.state == "awaiting_approval") {
                job.state = "interrupted";
                job.updated_at = NowSeconds();
            }
            if (job.state == "queued") {
                queue_.push_back(job.job_id);
            }
            cancel_flags_[job.job_id] =
                    std::make_shared<std::atomic<bool>>(false);
            jobs_[job.job_id] = std::move(job);
        }
    } catch (const nlohmann::json::exception&) {
        // 损坏状态按空处理；不阻塞后端启动。
    }
}

void ReviewJobManager::SaveLocked() const {
    if (state_file_.empty()) {
        return;
    }
    nlohmann::json json;
    json["jobs"] = nlohmann::json::array();
    for (const auto& [id, job] : jobs_) {
        (void)id;
        json["jobs"].push_back(JobToJson(job));
    }
    std::string error;
    WriteAtomic(state_file_, json.dump(2), &error);
}

bool ReviewJobManager::Submit(const std::string& pack_path,
                              const std::string& parent_session_id,
                              ReviewJob* job, std::string* error) {
    auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (!config_.background_enabled) {
        return fail("background review is disabled ([review] background_enabled=false)");
    }
    const fs::path path = aiagent::platform::ResolveCanonicalPath({}, pack_path);
    ReviewPack pack;
    std::string load_error;
    if (!LoadReviewPack(aiagent::platform::ToUtf8(path), &pack, &load_error)) {
        return fail("cannot load review pack: " + load_error);
    }
    const std::string repo = aiagent::platform::ToUtf8(
            fs::weakly_canonical(aiagent::platform::FromUtf8(pack.repo_root)));
    const std::string rel = aiagent::platform::ToGenericUtf8(
            path.lexically_relative(aiagent::platform::FromUtf8(repo)));
    if (rel.empty() || (rel != NormalizeRelativePath(config_.dir) &&
                        rel.rfind(NormalizeRelativePath(config_.dir) + "/", 0) != 0)) {
        return fail("review pack must live inside the review directory");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::string normalized_repo =
            aiagent::platform::NormalizePathKey(repo);
    for (auto& [id, existing] : jobs_) {
        if (aiagent::platform::NormalizePathKey(existing.repo_root) != normalized_repo ||
            existing.feature != pack.feature ||
            existing.branch != pack.git.branch) {
            continue;
        }
        if (existing.state == "queued" || existing.state == "running" ||
            existing.state == "awaiting_approval") {
            existing.state = "superseded";
            existing.updated_at = NowSeconds();
            if (auto flag = cancel_flags_.find(id); flag != cancel_flags_.end()) {
                flag->second->store(true);
            }
            queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end());
        }
    }

    int queued = 0;
    for (const auto& [id, existing] : jobs_) {
        (void)id;
        if (existing.state == "queued") {
            ++queued;
        }
    }
    if (config_.max_queued_jobs > 0 && queued >= config_.max_queued_jobs) {
        return fail("review queue is full (" + std::to_string(config_.max_queued_jobs) +
                    ")");
    }

    ReviewJob created;
    created.job_id = NewJobId();
    created.review_id = pack.review_id;
    created.pack_path = aiagent::platform::ToUtf8(path);
    created.repo_root = repo;
    created.feature = pack.feature;
    created.branch = pack.git.branch;
    created.session_id = "review-job-" + created.job_id;
    created.parent_session_id = parent_session_id;
    created.state = "queued";
    created.created_at = NowSeconds();
    created.updated_at = created.created_at;
    cancel_flags_[created.job_id] = std::make_shared<std::atomic<bool>>(false);
    jobs_[created.job_id] = created;
    queue_.push_back(created.job_id);
    SaveLocked();
    cv_.notify_all();
    if (job != nullptr) {
        *job = created;
    }
    return true;
}

std::vector<ReviewJob> ReviewJobManager::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ReviewJob> output;
    output.reserve(jobs_.size());
    for (const auto& [id, job] : jobs_) {
        (void)id;
        output.push_back(job);
    }
    std::sort(output.begin(), output.end(), [](const ReviewJob& left, const ReviewJob& right) {
        if (left.created_at != right.created_at) {
            return left.created_at > right.created_at;
        }
        return left.job_id > right.job_id;
    });
    return output;
}

std::optional<ReviewJob> ReviewJobManager::Get(const std::string& job_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool ReviewJobManager::Cancel(const std::string& job_id, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
        if (error != nullptr) {
            *error = "unknown review job: " + job_id;
        }
        return false;
    }
    ReviewJob& job = it->second;
    if (IsTerminalState(job.state) && job.state != "interrupted") {
        if (error != nullptr) {
            *error = "review job already finished: " + job.state;
        }
        return false;
    }
    queue_.erase(std::remove(queue_.begin(), queue_.end(), job_id), queue_.end());
    if (job.state == "running") {
        if (auto flag = cancel_flags_.find(job_id); flag != cancel_flags_.end()) {
            flag->second->store(true);
        }
    }
    job.state = "cancelled";
    job.updated_at = NowSeconds();
    SaveLocked();
    cv_.notify_all();
    return true;
}

bool ReviewJobManager::Attach(const std::string& job_id, ReviewJob* job,
                              std::string* report_text, std::string* error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
        if (error != nullptr) {
            *error = "unknown review job: " + job_id;
        }
        return false;
    }
    if (job != nullptr) {
        *job = it->second;
    }
    if (report_text != nullptr) {
        report_text->clear();
        if (!it->second.report_path.empty()) {
            const fs::path path = aiagent::platform::ResolveCanonicalPath(
                    aiagent::platform::FromUtf8(it->second.repo_root),
                    it->second.report_path);
            std::string text;
            bool truncated = false;
            std::string read_error;
            if (aiagent::platform::ReadFilePrefix(path, 4 * 1024 * 1024, &text,
                                                  &truncated, &read_error)) {
                *report_text = std::move(text);
            } else if (!read_error.empty() && error != nullptr) {
                *error = read_error;
            }
        }
    }
    return true;
}

void ReviewJobManager::SetCompletionCallback(CompletionCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    completion_ = std::move(callback);
}

bool ReviewJobManager::HasRunnableLocked() const {
    for (const auto& id : queue_) {
        const auto it = jobs_.find(id);
        if (it == jobs_.end() || it->second.state != "queued") {
            continue;
        }
        const auto running = running_repos_.find(it->second.repo_root);
        if (running == running_repos_.end() || running->second <= 0) {
            return true;
        }
    }
    return false;
}

bool ReviewJobManager::PopRunnableLocked(ReviewJob* job) {
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        const auto job_it = jobs_.find(*it);
        if (job_it == jobs_.end() || job_it->second.state != "queued") {
            continue;
        }
        const auto running = running_repos_.find(job_it->second.repo_root);
        if (running != running_repos_.end() && running->second > 0) {
            continue;
        }
        ReviewJob selected = job_it->second;
        selected.state = "running";
        selected.updated_at = NowSeconds();
        job_it->second = selected;
        running_repos_[selected.repo_root] += 1;
        queue_.erase(it);
        *job = selected;
        return true;
    }
    return false;
}

void ReviewJobManager::FinishLocked(const std::string& job_id,
                                    const ReviewJobRunResult& result) {
    const auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
        return;
    }
    ReviewJob& job = it->second;
    if (job.state == "cancelled" || job.state == "superseded" ||
        job.state == "interrupted") {
        job.updated_at = NowSeconds();
        return;
    }
    if (result.cancelled) {
        job.state = "cancelled";
    } else if (result.timed_out) {
        job.state = "timed_out";
    } else if (result.ok) {
        job.state = "done";
    } else {
        job.state = "failed";
        if (job.error.empty()) {
            job.error = "review failed";
        }
    }
    if (!result.report_path.empty()) {
        job.report_path = result.report_path;
    }
    if (!result.verdict.empty()) {
        job.verdict = result.verdict;
    }
    if (!result.error.empty()) {
        job.error = result.error;
    }
    job.progress.findings = result.findings;
    job.updated_at = NowSeconds();
}

void ReviewJobManager::WorkerLoop() {
    while (true) {
        ReviewJob job;
        CompletionCallback callback;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || HasRunnableLocked(); });
            if (stopping_) {
                return;
            }
            if (!PopRunnableLocked(&job)) {
                continue;
            }
            SaveLocked();
        }

        CancelFlag flag;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = cancel_flags_.find(job.job_id);
            flag = it == cancel_flags_.end() ? std::make_shared<std::atomic<bool>>(false)
                                             : it->second;
        }
        ReviewJobRunResult result;
        if (runner_) {
            result = runner_(
                    job, [flag] { return flag->load(); },
                    [this, id = job.job_id](const ReviewJobProgress& progress) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        const auto it = jobs_.find(id);
                        if (it == jobs_.end() || it->second.state != "running") {
                            return;
                        }
                        it->second.progress = progress;
                        it->second.updated_at = NowSeconds();
                        SaveLocked();
                    });
        } else {
            result.error = "review runner is not configured";
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto running = running_repos_.find(job.repo_root);
            if (running != running_repos_.end() && running->second > 0) {
                running->second -= 1;
                if (running->second == 0) {
                    running_repos_.erase(running);
                }
            }
            FinishLocked(job.job_id, result);
            SaveLocked();
            const auto it = jobs_.find(job.job_id);
            if (it != jobs_.end()) {
                job = it->second;
            }
            callback = completion_;
        }
        cv_.notify_all();
        if (callback) {
            callback(job);
        }
    }
}

void ReviewJobManager::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        for (auto& [id, job] : jobs_) {
            if (job.state == "running" || job.state == "awaiting_approval") {
                job.state = "interrupted";
                job.updated_at = NowSeconds();
            }
            if (auto flag = cancel_flags_.find(id); flag != cancel_flags_.end()) {
                flag->second->store(true);
            }
        }
        SaveLocked();
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

}  // namespace aiagent::review
