#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "aiagent/review/review_state.hpp"

#include "review_test_support.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

}  // namespace

int main() {
    namespace review = aiagent::review;
    try {
        const std::string repo = review_test_support::UniqueDir("review_ledger_repo_");
        aiagent::ReviewConfig config;
        config.dir = "docs/review";
        const std::string review_id = "coverage-ledger-test";

        // 旧 state 懒迁移：reviewed_files -> parent_verified_files。
        {
            const std::filesystem::path state_path =
                    aiagent::platform::FromUtf8(review::ReviewStatePath(config, repo,
                                                                       review_id));
            std::filesystem::create_directories(state_path.parent_path());
            std::ofstream out(state_path, std::ios::binary | std::ios::trunc);
            out << R"({"review_id":"coverage-ledger-test","pack_path":"docs/review/p.json",
"reviewed_files":["src/old.cpp"],"updated_at":"2026-11-09T12:00:00+08:00"})";
            out.close();
            review::ReviewState state;
            std::string error;
            Expect(review::LoadReviewState(config, repo, review_id, &state, &error),
                   "load old state: " + error);
            Expect(state.coverage_ledger.parent_verified_files.size() == 1 &&
                           state.coverage_ledger.parent_verified_files[0] == "src/old.cpp",
                   "old reviewed_files should migrate to parent ledger");
        }

        // parent/scout 回执合并 + 兼容字段同步。
        {
            std::string error;
            Expect(review::UpdateReviewState(
                           config, repo, review_id, "docs/review/p.json",
                           [](review::ReviewState* state) {
                               review::AddCoverageReceipt(state, "", "review_read:full",
                                                          "src/parent.cpp");
                               review::AddCoverageReceipt(state, "slice-a",
                                                          "review_read:full",
                                                          "src/scout.cpp");
                               review::AddCoverageReceipt(state, "slice-a",
                                                          "review_context:diff",
                                                          "src/scout2.cpp");
                           },
                           &error),
                   "merge receipts: " + error);
            review::ReviewState state;
            Expect(review::LoadReviewState(config, repo, review_id, &state, &error),
                   "load merged state: " + error);
            const auto parent = review::ParentVerifiedFiles(state);
            Expect(parent.size() == 2 &&
                           std::find(parent.begin(), parent.end(), "src/old.cpp") !=
                                   parent.end() &&
                           std::find(parent.begin(), parent.end(), "src/parent.cpp") !=
                                   parent.end(),
                   "parent ledger should union old + new");
            const auto* slice =
                    review::FindCoverageSlice(state.coverage_ledger, "slice-a");
            Expect(slice != nullptr && slice->covered_files.size() == 2 &&
                           slice->methods.at("src/scout.cpp") == "review_read:full" &&
                           slice->methods.at("src/scout2.cpp") ==
                                   "review_context:diff",
                   "scout slice receipts and methods");
            Expect(std::find(state.reviewed_files.begin(), state.reviewed_files.end(),
                             "src/parent.cpp") != state.reviewed_files.end(),
                   "parent receipt should sync reviewed_files");
        }

        // 并发 merge：多线程 UpdateReviewState 不能丢更新。
        {
            std::vector<std::thread> workers;
            for (int i = 0; i < 8; ++i) {
                workers.emplace_back([&, i]() {
                    for (int j = 0; j < 5; ++j) {
                        const std::string path =
                                "src/concurrent_" + std::to_string(i) + "_" +
                                std::to_string(j) + ".cpp";
                        std::string error;
                        review::UpdateReviewState(
                                config, repo, review_id, "docs/review/p.json",
                                [&path](review::ReviewState* state) {
                                    review::AddCoverageReceipt(
                                            state, "slice-" + std::to_string(0),
                                            "review_read:full", path);
                                },
                                &error);
                    }
                });
            }
            for (auto& worker : workers) {
                worker.join();
            }
            review::ReviewState state;
            std::string error;
            Expect(review::LoadReviewState(config, repo, review_id, &state, &error),
                   "load concurrent state: " + error);
            const auto* slice =
                    review::FindCoverageSlice(state.coverage_ledger, "slice-0");
            Expect(slice != nullptr && slice->covered_files.size() == 40,
                   "concurrent merge should keep all 40 receipts");
        }

        // failed slice 派生与用量。
        {
            std::string error;
            review::ReviewUsage run;
            run.prompt_tokens = 1000000;
            run.completion_tokens = 100000;
            run.cache_hit_tokens = 500000;
            run.requests = 3;
            run.duration_ms = 1000;
            Expect(review::UpdateReviewState(
                           config, repo, review_id, "docs/review/p.json",
                           [&run](review::ReviewState* state) {
                               review::ReviewCoverageSlice slice;
                               slice.slice_id = "failed-slice";
                               slice.name = "failed-slice";
                               slice.status = "timed_out";
                               slice.assigned_files = {"src/missing.cpp"};
                               slice.error = "timeout";
                               state->coverage_ledger.slices.push_back(slice);
                               state->has_run_usage = true;
                               state->run_usage = run;
                           },
                           &error),
                   "merge failed slice: " + error);
            review::ReviewState state;
            Expect(review::LoadReviewState(config, repo, review_id, &state, &error),
                   "load failed state: " + error);
            Expect(state.coverage_ledger.failed_slices.size() == 1 &&
                           state.coverage_ledger.failed_slices[0] == "failed-slice",
                   "failed slice should be visible");
            Expect(state.has_run_usage &&
                           review::TotalUsage(state).prompt_tokens == 1000000,
                   "run usage should persist");
        }

        // 价格全 0 -> 不估算；有价格 -> 按 cache 子集计算。
        {
            review::ReviewState state;
            std::string error;
            Expect(review::LoadReviewState(config, repo, review_id, &state, &error),
                   "load cost state: " + error);
            Expect(!review::EstimateReviewCostUsd(state, config).has_value(),
                   "zero prices must not fabricate cost");
            config.price_input_per_million_usd = 1.0;
            config.price_cached_input_per_million_usd = 0.1;
            config.price_output_per_million_usd = 2.0;
            const auto cost = review::EstimateReviewCostUsd(state, config);
            Expect(cost.has_value() &&
                           std::abs(*cost - (0.5 * 1.0 + 0.5 * 0.1 + 0.1 * 2.0)) <
                                   1e-9,
                   "cost should split cached/uncached input");
        }

        std::filesystem::remove_all(repo);
    } catch (const std::exception& e) {
        std::cerr << "review coverage ledger test failed: " << e.what() << '\n';
        return 1;
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " review coverage ledger test(s) failed\n";
        return 1;
    }
    std::cout << "review coverage ledger test passed\n";
    return 0;
}
