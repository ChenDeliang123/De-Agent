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
        Expect(config->max_tool_rounds == 128, "default max tool rounds");
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
        Expect(!config->checkpoint && !config->draft_report, "bool overrides");
        Expect(config->chunk_files == 5, "chunk override");
        Expect(config->max_queued_jobs == 3, "queue override");
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
