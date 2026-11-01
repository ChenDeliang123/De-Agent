#include "aiagent/review/review_report.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "aiagent/platform/paths.hpp"
#include "aiagent/review/project_verify_config.hpp"
#include "aiagent/util/sha256.hpp"

namespace aiagent::review {
namespace {

namespace fs = std::filesystem;

std::string YamlPlain(const std::string& value) {
    if (value.empty()) {
        return "\"\"";
    }
    for (const char c : value) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                          c == '-' || c == '/' || c == '@' || c == ':';
        if (!safe) {
            std::string quoted = "\"";
            for (const char inner : value) {
                if (inner == '\\' || inner == '"') {
                    quoted.push_back('\\');
                }
                quoted.push_back(inner);
            }
            quoted.push_back('"');
            return quoted;
        }
    }
    return value;
}

bool IsSeverity(const std::string& value) {
    return value == "blocker" || value == "major" || value == "minor" || value == "nit";
}

bool IsConfidence(const std::string& value) {
    return value == "high" || value == "medium" || value == "low";
}

bool IsFindingStatus(const std::string& value) {
    return value == "open" || value == "resolved" || value == "still_open" ||
           value == "regressed";
}

bool IsFinalVerdict(const std::string& value) {
    return value == "approved" || value == "approved_with_notes" ||
           value == "changes_requested" || value == "blocked";
}

std::string ReviewDirRelative(const ReviewConfig& config) {
    return NormalizeRelativePath(config.dir);
}

bool PathInsideReviewDir(const std::string& path, const std::string& dir) {
    const std::string normalized = NormalizeRelativePath(path);
    const std::string normalized_dir = NormalizeRelativePath(dir);
    return normalized == normalized_dir ||
           normalized.rfind(normalized_dir + "/", 0) == 0;
}

bool WriteAtomic(const fs::path& target, const std::string& content, std::string* error) {
    std::error_code ec;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
    }
    const fs::path temp = target.string() + ".tmp";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error != nullptr) {
                *error = "cannot create " + aiagent::platform::ToUtf8(target);
            }
            return false;
        }
        output << content;
        if (!output.good()) {
            if (error != nullptr) {
                *error = "cannot write " + aiagent::platform::ToUtf8(target);
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
            *error = "cannot replace " + aiagent::platform::ToUtf8(target) + ": " + ec.message();
        }
        return false;
    }
    return true;
}

}  // namespace

std::string ReviewVerdictName(const std::string& verdict) {
    return verdict;
}

ReviewReportWriter::ReviewReportWriter(ReviewConfig config) : config_(std::move(config)) {}

ReviewReportResult ReviewReportWriter::Write(
        const std::string& repo_root, const ReviewPack& pack,
        const std::string& pack_relative_path, const std::string& pack_sha256,
        const ReviewStaleResult& stale, const ReviewState& state,
        const ReviewReportInput& input) const {
    ReviewReportResult result;
    auto fail = [&result](const std::string& message) {
        result.ok = false;
        result.error = message;
        return result;
    };

    if (input.status != "draft" && input.status != "final") {
        return fail("report status must be draft or final");
    }
    if (input.status == "final") {
        if (!IsFinalVerdict(input.verdict)) {
            return fail("final report requires verdict approved/approved_with_notes/"
                        "changes_requested/blocked");
        }
        if (stale.stale && input.verdict != "blocked") {
            return fail("stale pack only allows verdict=blocked");
        }
    } else if (!input.verdict.empty() && !IsFinalVerdict(input.verdict)) {
        return fail("invalid verdict");
    }

    const std::string review_dir = ReviewDirRelative(config_);
    if (!PathInsideReviewDir(pack_relative_path, review_dir)) {
        return fail("pack path is not inside the review directory");
    }
    const fs::path pack_file =
            aiagent::platform::FromUtf8(repo_root) /
            aiagent::platform::FromUtf8(pack_relative_path);
    const std::string stem = aiagent::platform::ToUtf8(pack_file.stem());
    if (stem.empty() || stem.find('/') != std::string::npos ||
        stem.find('\\') != std::string::npos) {
        return fail("cannot derive report name from pack path");
    }
    const fs::path report_path =
            aiagent::platform::FromUtf8(repo_root) /
            aiagent::platform::FromUtf8(review_dir) /
            aiagent::platform::FromUtf8(stem + ".md");
    const std::string report_relative =
            aiagent::platform::ToGenericUtf8(report_path.lexically_relative(
                    aiagent::platform::FromUtf8(repo_root)));

    // 验证结果只能引用 pack 中声明的命令，且必须有真实执行记录。
    std::set<std::string> declared;
    for (const auto& command : pack.verification) {
        declared.insert(command.source + ":" + command.id);
    }
    for (const auto& ref : input.verification) {
        std::string source = ref.source;
        if (source.empty()) {
            for (const auto& command : pack.verification) {
                if (command.id == ref.command_id) {
                    source = command.source;
                    break;
                }
            }
        }
        const std::string key = source + ":" + ref.command_id;
        if (declared.count(key) == 0) {
            return fail("verification command is not declared in the pack: " + key);
        }
        if (FindVerification(state, ref.command_id, source) == nullptr) {
            return fail("verification command was not executed: " + key);
        }
    }

    std::set<std::string> finding_ids;
    bool has_blocker_or_major = false;
    bool has_source_modified = false;
    for (const auto& record : state.verification) {
        if (record.source_modified || record.invalid) {
            has_source_modified = true;
        }
    }
    for (const auto& finding : input.findings) {
        if (finding.id.empty() || !finding_ids.insert(finding.id).second) {
            return fail("finding ids must be non-empty and unique: " + finding.id);
        }
        if (!IsSeverity(finding.severity)) {
            return fail("finding " + finding.id + " has invalid severity");
        }
        if (input.status == "final") {
            if (finding.title.empty() || finding.file.empty() ||
                finding.evidence.empty() || finding.impact.empty() ||
                finding.trigger.empty() || finding.suggestion.empty()) {
                return fail("finding " + finding.id +
                            " is missing title/file/evidence/impact/trigger/suggestion");
            }
            if (!IsConfidence(finding.confidence)) {
                return fail("finding " + finding.id + " has invalid confidence");
            }
            if (!IsFindingStatus(finding.status)) {
                return fail("finding " + finding.id + " has invalid status");
            }
        }
        if ((finding.severity == "blocker" || finding.severity == "major") &&
            finding.status != "resolved") {
            has_blocker_or_major = true;
        }
    }
    if (input.status == "final") {
        if (has_blocker_or_major && input.verdict != "changes_requested" &&
            input.verdict != "blocked") {
            return fail("blocker/major findings require changes_requested or blocked");
        }
        if ((input.verdict == "approved" || input.verdict == "approved_with_notes") &&
            has_blocker_or_major) {
            return fail("approved verdict cannot contain open blocker/major findings");
        }
        if ((input.verdict == "approved" || input.verdict == "approved_with_notes") &&
            has_source_modified) {
            return fail("verification changed source files; verdict cannot be approved");
        }
    }

    // reviewer-owned 测试文件：必须是状态记录里的文件，且内容未被外部改动。
    ProjectVerifyConfig project_config;
    std::string project_error;
    std::vector<std::string> test_globs;
    if (LoadProjectVerifyConfig(repo_root, &project_config, &project_error)) {
        test_globs = project_config.test_globs;
    }
    std::set<std::string> reported_tests;
    for (const auto& path : input.test_files) {
        const std::string normalized = NormalizeRelativePath(path);
        if (!reported_tests.insert(normalized).second) {
            return fail("duplicate test_files entry: " + path);
        }
        if (!IsTestPath(normalized, test_globs)) {
            return fail("test_files entry is not a classified test path: " + path);
        }
        const auto it = std::find_if(state.reviewer_tests.begin(), state.reviewer_tests.end(),
                                     [&normalized](const ReviewerTestRecord& record) {
                                         return record.path == normalized;
                                     });
        if (it == state.reviewer_tests.end()) {
            return fail("test_files entry was not written by review_test_write: " + path);
        }
        std::string hash_error;
        const std::string current = aiagent::util::Sha256HexFile(
                aiagent::platform::FromUtf8(repo_root) /
                        aiagent::platform::FromUtf8(normalized),
                &hash_error);
        if (current.empty() || current != it->sha256) {
            return fail("test_files entry changed after reviewer wrote it: " + path);
        }
    }
    if (input.status == "final") {
        for (const auto& record : state.reviewer_tests) {
            if (reported_tests.count(record.path) == 0) {
                return fail("report is missing reviewer-owned test file: " + record.path);
            }
        }
    }

    std::ostringstream report;
    report << "---\n";
    report << "review_id: " << YamlPlain(pack.review_id) << '\n';
    report << "feature: " << YamlPlain(pack.feature) << '\n';
    std::string producer = pack.producer.kind;
    if (!pack.producer.agent.empty()) {
        producer += ":" + pack.producer.agent;
    }
    report << "producer: " << YamlPlain(producer) << '\n';
    report << "intent_source: " << YamlPlain(pack.intent_source) << '\n';
    report << "pack: " << YamlPlain(pack_relative_path) << '\n';
    report << "pack_sha256: " << YamlPlain(pack_sha256) << '\n';
    report << "base_commit: " << YamlPlain(pack.git.base_commit) << '\n';
    report << "head_commit: " << YamlPlain(pack.git.head_commit) << '\n';
    report << "diff_sha256: " << YamlPlain(pack.git.diff_sha256) << '\n';
    report << "dirty: " << (pack.git.dirty ? "true" : "false") << '\n';
    report << "stale: " << (stale.stale ? "true" : "false") << '\n';
    report << "round: " << (pack.previous_report.empty() ? 1 : 2) << '\n';
    if (pack.previous_report.empty()) {
        report << "previous_report: null\n";
    } else {
        report << "previous_report: " << YamlPlain(pack.previous_report) << '\n';
    }
    report << "status: " << input.status << '\n';
    report << "verdict: " << (input.verdict.empty() ? "null" : YamlPlain(input.verdict))
           << '\n';
    report << "reviewed_at: " << YamlPlain(LocalTimestampIso()) << '\n';
    report << "---\n\n";

    report << "# 代码审核：" << pack.feature << "\n\n";
    report << "## 结论\n\n";
    report << (input.summary.empty() ? "(not provided)" : input.summary) << "\n\n";

    report << "## 发现\n\n";
    if (input.findings.empty()) {
        report << "(未发现问题)\n\n";
    } else {
        for (const auto& finding : input.findings) {
            report << "### " << finding.id << " [" << finding.severity << "] "
                   << finding.title << "\n\n";
            report << "- 位置：`" << finding.file;
            if (finding.line > 0) {
                report << ':' << finding.line;
            }
            report << "`\n";
            if (!finding.evidence.empty()) {
                report << "- 证据：`" << finding.evidence << "`\n";
            }
            if (!finding.impact.empty()) {
                report << "- 影响：" << finding.impact << '\n';
            }
            if (!finding.trigger.empty()) {
                report << "- 触发/复现：" << finding.trigger << '\n';
            }
            if (!finding.suggestion.empty()) {
                report << "- 建议：" << finding.suggestion << '\n';
            }
            if (!finding.confidence.empty()) {
                report << "- 置信度：" << finding.confidence << '\n';
            }
            report << "- 状态：" << (finding.status.empty() ? "open" : finding.status)
                   << "\n\n";
        }
    }

    report << "## 已核对\n\n";
    if (input.checked.empty()) {
        report << "(none)\n\n";
    } else {
        for (const auto& item : input.checked) {
            report << "- " << item << '\n';
        }
        report << '\n';
    }

    report << "## 验证执行\n\n";
    if (state.verification.empty()) {
        report << "(none)\n\n";
    } else {
        for (const auto& record : state.verification) {
            report << "- " << record.source << ":" << record.command_id
                   << "：exit=" << record.exit_code
                   << "，duration_ms=" << record.duration_ms
                   << "，timed_out=" << (record.timed_out ? "true" : "false")
                   << "，source_modified=" << (record.source_modified ? "true" : "false")
                   << "，invalid=" << (record.invalid ? "true" : "false") << '\n';
        }
        report << '\n';
    }

    report << "## 审核测试\n\n";
    if (input.test_files.empty()) {
        report << "(none)\n\n";
    } else {
        for (const auto& path : input.test_files) {
            const auto it = std::find_if(state.reviewer_tests.begin(), state.reviewer_tests.end(),
                                         [&path](const ReviewerTestRecord& record) {
                                             return record.path == NormalizeRelativePath(path);
                                         });
            report << "- `" << path << "`："
                   << (it != state.reviewer_tests.end() ? it->mode : "unknown");
            if (it != state.reviewer_tests.end() && !it->reason.empty()) {
                report << "；" << it->reason;
            }
            report << '\n';
        }
        report << '\n';
    }

    report << "## 未验证\n\n";
    if (input.unverified.empty()) {
        report << "(none)\n\n";
    } else {
        for (const auto& item : input.unverified) {
            report << "- " << item << '\n';
        }
        report << '\n';
    }

    report << "## 待澄清\n\n";
    if (input.open_questions.empty()) {
        report << "(none)\n\n";
    } else {
        for (const auto& item : input.open_questions) {
            report << "- " << item << '\n';
        }
        report << '\n';
    }

    if (!input.notes.empty()) {
        report << "## 备注\n\n" << input.notes << "\n\n";
    }

    report << "## 范围与时效\n\n";
    report << "- base_ref: `" << pack.git.base_ref << "`\n";
    report << "- base..head: `" << pack.git.base_commit << ".." << pack.git.head_commit << "`\n";
    report << "- branch: `" << pack.git.branch << "`\n";
    report << "- dirty: " << (pack.git.dirty ? "true" : "false") << '\n';
    report << "- stale: " << (stale.stale ? "true" : "false") << '\n';
    if (stale.head_changed) {
        report << "- head_changed: true (content digest unchanged)\n";
    }
    for (const auto& reason : stale.reasons) {
        report << "- stale_reason: " << reason << '\n';
    }
    if (pack.context_missing) {
        report << "- context_missing: true（意图未完整对齐）\n";
    }

    const std::string content = report.str();
    if (content.size() > config_.max_report_bytes) {
        return fail("rendered report exceeds [review] max_report_bytes");
    }
    std::string write_error;
    if (!WriteAtomic(report_path, content, &write_error)) {
        return fail(write_error);
    }

    result.ok = true;
    result.path = report_relative;
    result.bytes = content.size();
    result.verdict = input.verdict;
    result.findings = input.findings.size();
    (void)pack_sha256;
    return result;
}

}  // namespace aiagent::review
