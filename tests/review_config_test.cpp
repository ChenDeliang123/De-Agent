#include <cmath>
#include <iostream>
#include <optional>
#include <string>

#include "aiagent/config.hpp"

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
    // 缺省段 = 默认值，且审核默认开启。
    {
        const auto config = aiagent::ParseReviewConfig("");
        Expect(config.has_value(), "empty config should parse");
        Expect(config->enabled, "review should default to enabled");
        Expect(config->dir == "docs/review", "default review dir");
        Expect(config->max_diff_bytes == 2 * 1024 * 1024, "default max_diff_bytes");
        Expect(config->verify_timeout_s == 900, "default verify timeout");
        Expect(config->max_tool_rounds == 192, "default max tool rounds");
        Expect(config->scout_max == 4, "default scout max");
        Expect(config->scout_timeout_s == 600, "default scout timeout");
        Expect(config->scout_max_rounds == 64, "default scout rounds");
        Expect(config->slice_total_max == 4, "default slice total max");
        Expect(config->slice_concurrency == 4, "default slice concurrency");
        Expect(config->slice_max_files == 20, "default slice max files");
        Expect(config->slice_max_rounds == 96, "default slice rounds");
        Expect(config->slice_timeout_s == 600, "default slice timeout");
        Expect(config->slice_result_max_chars == 16384,
               "default slice result max chars");
        Expect(config->slice_max_findings == 8, "default slice max findings");
        Expect(config->slice_profile == "deep", "default slice profile");
        Expect(!config->used_legacy_scout_keys, "default has no legacy scout keys");
        Expect(config->price_input_per_million_usd == 0 &&
                       config->price_cached_input_per_million_usd == 0 &&
                       config->price_output_per_million_usd == 0,
               "default review prices are zero");
        Expect(config->chunk_files == 20, "default chunk size");
    }

    // 合法覆盖。
    {
        std::string error;
        const auto config = aiagent::ParseReviewConfig(
                "[review]\n"
                "enabled = false\n"
                "dir = notes/reviews\n"
                "max_diff_bytes = 1048576\n"
                "max_files = 42\n"
                "verify_enabled = false\n"
                "verify_timeout_s = 120\n"
                "verify_output_bytes = 4096\n"
                "verify_mode = copy\n"
                "max_tool_rounds = 32\n"
                "scout_max = 4\n"
                "scout_timeout_s = 300\n"
                "scout_max_rounds = 48\n"
                "slice_total_max = 3\n"
                "slice_concurrency = 2\n"
                "slice_max_files = 10\n"
                "slice_max_rounds = 120\n"
                "slice_timeout_s = 900\n"
                "slice_result_max_chars = 32768\n"
                "slice_max_findings = 5\n"
                "slice_profile = balanced\n"
                "price_input_per_million_usd = 0.27\n"
                "price_cached_input_per_million_usd = 0.07\n"
                "price_output_per_million_usd = 1.1\n"
                "review_timeout_s = 600\n"
                "checkpoint = false\n"
                "draft_report = false\n"
                "chunk_files = 5\n"
                "chunk_diff_bytes = 8192\n"
                "max_concurrent_jobs = 2\n"
                "max_queued_jobs = 3\n",
                &error);
        Expect(config.has_value(), "valid review config: " + error);
        Expect(!config->enabled, "enabled override");
        Expect(config->dir == "notes/reviews", "dir override");
        Expect(config->max_diff_bytes == 1048576, "max_diff_bytes override");
        Expect(config->verify_output_bytes == 4096, "verify output override");
        Expect(config->max_tool_rounds == 32, "max rounds override");
        Expect(config->scout_max == 4, "scout max override");
        Expect(config->scout_timeout_s == 300, "scout timeout override");
        Expect(config->scout_max_rounds == 48, "scout rounds override");
        Expect(config->slice_total_max == 3, "slice total override");
        Expect(config->slice_concurrency == 2, "slice concurrency override");
        Expect(config->slice_max_files == 10, "slice max files override");
        Expect(config->slice_max_rounds == 120, "slice rounds override");
        Expect(config->slice_timeout_s == 900, "slice timeout override");
        Expect(config->slice_result_max_chars == 32768,
               "slice result max chars override");
        Expect(config->slice_max_findings == 5, "slice max findings override");
        Expect(config->slice_profile == "balanced", "slice profile override");
        Expect(config->used_legacy_scout_keys,
               "legacy scout keys should be marked for migration");
        Expect(std::abs(config->price_input_per_million_usd - 0.27) < 1e-9 &&
                       std::abs(config->price_cached_input_per_million_usd - 0.07) <
                               1e-9 &&
                       std::abs(config->price_output_per_million_usd - 1.1) < 1e-9,
               "price overrides");
        Expect(!config->checkpoint && !config->draft_report, "bool overrides");
        Expect(config->chunk_files == 5, "chunk override");
        Expect(config->max_queued_jobs == 3, "queue override");
    }

    // 旧 scout_* 别名映射到 slice_*；旧 scout_max=0 仍关闭工具。
    {
        std::string error;
        const auto config = aiagent::ParseReviewConfig(
                "[review]\n"
                "scout_max = 0\n"
                "scout_timeout_s = 120\n"
                "scout_max_rounds = 32\n",
                &error);
        Expect(config.has_value(), "legacy slice config: " + error);
        Expect(config->slice_total_max == 0 &&
                       config->slice_timeout_s == 120 &&
                       config->slice_max_rounds == 32,
               "legacy scout keys should map to slice fields");
        Expect(config->used_legacy_scout_keys,
               "legacy keys should set the migration flag");
    }

    // profile 只给默认值；显式 slice_total_max 优先于 fast 的 0。
    {
        std::string error;
        const auto fast = aiagent::ParseReviewConfig(
                "[review]\nslice_profile = fast\n", &error);
        Expect(fast.has_value() && fast->slice_total_max == 0 &&
                       fast->max_tool_rounds == 128,
               "fast profile should disable slices and use 128 rounds");
        const auto explicit_total = aiagent::ParseReviewConfig(
                "[review]\nslice_profile = fast\nslice_total_max = 2\n", &error);
        Expect(explicit_total.has_value() &&
                       explicit_total->slice_total_max == 2 &&
                       explicit_total->max_tool_rounds == 128,
               "explicit slice_total_max should override fast profile");
    }

    // 轮次上限边界：8 与 512 合法，7 与 513 非法。
    {
        std::string error;
        const auto high = aiagent::ParseReviewConfig("[review]\nmax_tool_rounds = 512\n",
                                                     &error);
        Expect(high.has_value() && high->max_tool_rounds == 512,
               "max rounds 512 accepted: " + error);
    }

    // 非法值必须 fail-fast，而不是静默回退。
    {
        const std::string bad_cases[] = {
                "[review]\ndir = ../outside\n",
                "[review]\ndir = /abs\n",
                "[review]\ndir = .\n",
                "[review]\nmax_diff_bytes = 1\n",
                "[review]\nmax_diff_bytes = 999999999\n",
                "[review]\nverify_timeout_s = 0\n",
                "[review]\nverify_timeout_s = 1801\n",
                "[review]\nverify_mode = rsync\n",
                "[review]\nmax_tool_rounds = 7\n",
                "[review]\nmax_tool_rounds = 513\n",
                "[review]\nscout_max = -1\n",
                "[review]\nscout_max = 9\n",
                "[review]\nscout_timeout_s = 59\n",
                "[review]\nscout_timeout_s = 1801\n",
                "[review]\nscout_max_rounds = 7\n",
                "[review]\nscout_max_rounds = 257\n",
                "[review]\nslice_total_max = 9\n",
                "[review]\nslice_concurrency = 0\n",
                "[review]\nslice_concurrency = 9\n",
                "[review]\nslice_max_files = 0\n",
                "[review]\nslice_max_files = 21\n",
                "[review]\nslice_max_rounds = 7\n",
                "[review]\nslice_max_rounds = 257\n",
                "[review]\nslice_timeout_s = 59\n",
                "[review]\nslice_timeout_s = 1801\n",
                "[review]\nslice_result_max_chars = 1023\n",
                "[review]\nslice_result_max_chars = 65537\n",
                "[review]\nslice_max_findings = 0\n",
                "[review]\nslice_max_findings = 9\n",
                "[review]\nslice_profile = turbo\n",
                "[review]\nprice_input_per_million_usd = -1\n",
                "[review]\nprice_cached_input_per_million_usd = abc\n",
                "[review]\nprice_output_per_million_usd = 1.2.3\n",
                "[review]\nreview_timeout_s = 59\n",
                "[review]\nsupersede_policy = queue\n",
                "[review]\nmax_queued_jobs = -1\n",
                "[review]\nmax_files = abc\n",
        };
        for (const auto& text : bad_cases) {
            std::string error;
            const auto config = aiagent::ParseReviewConfig(text, &error);
            Expect(!config.has_value() && !error.empty(),
                   "invalid review config should fail: " + text);
        }
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " review config test(s) failed\n";
        return 1;
    }
    return 0;
}
