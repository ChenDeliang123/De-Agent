#include "aiagent/review/review_report.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "aiagent/platform/paths.hpp"
#include "aiagent/platform/text_file.hpp"
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

std::string TrimText(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

std::string UnquoteYaml(std::string value) {
    value = TrimText(std::move(value));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        std::string output;
        for (std::size_t i = 1; i + 1 < value.size(); ++i) {
            if (value[i] == '\\' && i + 2 < value.size()) {
                output.push_back(value[++i]);
            } else {
                output.push_back(value[i]);
            }
        }
        return output;
    }
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

int NumericFindingId(const std::string& id) {
    if (id.size() < 2 || id.front() != 'R') {
        return 0;
    }
    int value = 0;
    for (std::size_t i = 1; i < id.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(id[i]))) {
            return 0;
        }
        value = value * 10 + (id[i] - '0');
    }
    return value;
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
bool LoadPreviousReviewSummary(const std::string& repo_root,
                               const std::string& report_relative_path,
                               PreviousReviewSummary* summary,
                               std::string* error) {
    if (summary == nullptr) {
        if (error != nullptr) {
            *error = "previous review summary output is null";
        }
        return false;
    }
    if (!IsSafeRelativePath(report_relative_path, false)) {
        if (error != nullptr) {
            *error = "previous report path must stay inside the repository";
        }
        return false;
    }
    const fs::path repo = aiagent::platform::FromUtf8(repo_root);
    const fs::path path =
            aiagent::platform::ResolveCanonicalPath(repo, report_relative_path);
    if (!aiagent::platform::IsPathWithin(path, repo)) {
        if (error != nullptr) {
            *error = "previous report escapes the repository";
        }
        return false;
    }
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) {
        if (error != nullptr) {
            *error = "previous report not found: " + report_relative_path;
        }
        return false;
    }
    std::string text;
    bool truncated = false;
    std::string read_error;
    if (!aiagent::platform::ReadFilePrefix(path, 4 * 1024 * 1024, &text, &truncated,
                                           &read_error) ||
        truncated) {
        if (error != nullptr) {
            *error = read_error.empty() ? "previous report is too large" : read_error;
        }
        return false;
    }

    PreviousReviewSummary parsed;
    parsed.report_path = NormalizeRelativePath(report_relative_path);
    bool saw_frontmatter = false;
    bool in_frontmatter = false;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string trimmed = TrimText(line);
        if (trimmed == "---") {
            if (!saw_frontmatter) {
                saw_frontmatter = true;
                in_frontmatter = true;
            } else if (in_frontmatter) {
                in_frontmatter = false;
            }
            continue;
        }
        if (in_frontmatter) {
            const std::size_t colon = trimmed.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const std::string key = TrimText(trimmed.substr(0, colon));
            const std::string value = UnquoteYaml(trimmed.substr(colon + 1));
            if (key == "pack") {
                parsed.pack_path = NormalizeRelativePath(value);
            } else if (key == "review_id") {
                parsed.review_id = value;
            } else if (key == "verdict") {
                parsed.verdict = value;
            }
            continue;
        }
        if (trimmed.rfind("### ", 0) == 0) {
            const std::string rest = trimmed.substr(4);
            const std::size_t space = rest.find(' ');
            const std::string id = rest.substr(0, space);
            if (!id.empty()) {
                parsed.finding_ids.push_back(id);
            }
        }
    }
    if (!saw_frontmatter || parsed.pack_path.empty()) {
        if (error != nullptr) {
            *error = "previous report has no frontmatter pack field";
        }
        return false;
    }
    *summary = std::move(parsed);
    return true;
}


ReviewReportWriter::ReviewReportWriter(ReviewConfig config, std::string data_dir)
        : config_(std::move(config)), data_dir_(std::move(data_dir)) {}

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
    // pack 创建后才通过 review_setup(save) 同意的 project 命令也应可引用；
    // 仍然要求 state 里存在真实执行记录。
    ProjectVerifyConfig project_config;
    std::string project_error;
    const bool has_project_config =
            LoadProjectVerifyConfig(repo_root, &project_config, &project_error);
    {
        ReviewConsentStore consent(
                data_dir_.empty()
                        ? std::string()
                        : aiagent::platform::ToUtf8(
                                  aiagent::platform::FromUtf8(data_dir_) /
                                  "review_consent.json"));
        if (has_project_config &&
            consent.Check(repo_root, project_config.sha256).status == "valid") {
            for (const auto& command : project_config.commands) {
                declared.insert("project:" + command.id);
            }
        }
    }
    for (const auto& ref : input.verification) {
        if (ref.command_id.empty()) {
            return fail("verification entry has an empty command id");
        }
        std::string source = ref.source;
        if (source.empty()) {
            for (const auto& command : pack.verification) {
                if (command.id == ref.command_id) {
                    source = command.source;
                    break;
                }
            }
        }
        if (source.empty() || (source != "pack" && source != "project")) {
            return fail("verification entry must be a pack:<id> or project:<id> "
                        "reference: " + ref.command_id);
        }
        const std::string key = source + ":" + ref.command_id;
        if (declared.count(key) == 0) {
            return fail("verification command is not declared or consented: " + key);
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
        if (finding.origin.empty()) {
            return fail("finding " + finding.id + " has empty origin");
        }
        if (finding.origin != "parent" &&
            finding.origin.rfind("scout:", 0) != 0) {
            return fail("finding " + finding.id +
                        " origin must be parent or scout:<slice_id>");
        }
        if (finding.origin.rfind("scout:", 0) == 0 && !finding.parent_verified) {
            return fail("finding " + finding.id +
                        " is not parent-verified; move it to unverified");
        }
        if ((finding.severity == "blocker" || finding.severity == "major") &&
            finding.status != "resolved") {
            has_blocker_or_major = true;
        }
    }
    PreviousReviewSummary previous;
    if (!pack.previous_report.empty()) {
        std::string previous_error;
        if (!LoadPreviousReviewSummary(repo_root, pack.previous_report, &previous,
                                       &previous_error)) {
            return fail("cannot read previous report: " + previous_error);
        }
        std::set<std::string> old_ids(previous.finding_ids.begin(),
                                      previous.finding_ids.end());
        int max_old_id = 0;
        for (const auto& id : previous.finding_ids) {
            max_old_id = std::max(max_old_id, NumericFindingId(id));
        }
        for (const auto& finding : input.findings) {
            if (old_ids.count(finding.id) > 0) {
                if (input.status == "final" && finding.status != "resolved" &&
                    finding.status != "still_open" && finding.status != "regressed") {
                    return fail("finding " + finding.id +
                                " from the previous round must be resolved/still_open/"
                                "regressed");
                }
            } else if (input.status == "final" && max_old_id > 0) {
                const int numeric = NumericFindingId(finding.id);
                if (numeric > 0 && numeric <= max_old_id) {
                    return fail("new finding " + finding.id +
                                " reuses the previous round's id space (max R" +
                                std::to_string(max_old_id) + ")");
                }
            }
        }
        if (input.status == "final") {
            for (const auto& id : previous.finding_ids) {
                if (finding_ids.count(id) == 0) {
                    return fail("final report is missing previous finding: " + id);
                }
            }
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
    std::vector<std::string> test_globs;
    if (has_project_config) {
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
    if (input.status == "final" && input.verdict != "blocked" && !state.timed_out) {
        for (const auto& record : state.reviewer_tests) {
            if (reported_tests.count(record.path) == 0) {
                return fail("report is missing reviewer-owned test file: " + record.path);
            }
        }
    }

    // 报告落盘前二次校验：审核期间代码可能又被改动，不能把旧结论写进 final。
    const ReviewStaleResult rechecked = CheckReviewStale(
            pack, config_, repo_root, ReviewerOwnedTestPaths(state, repo_root));
    const ReviewStaleResult effective_stale = rechecked.stale ? rechecked : stale;
    if (input.status == "final" && effective_stale.stale &&
        input.verdict != "blocked") {
        return fail("pack changed while the review was running; only verdict=blocked "
                    "can be written");
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
    report << "stale: " << (effective_stale.stale ? "true" : "false") << '\n';
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
    if (input.coverage.required_files > 0 || input.coverage.failed_slices > 0 ||
        input.coverage.has_usage) {
        report << "coverage:\n";
        report << "  required_files: " << input.coverage.required_files << '\n';
        report << "  covered_files: " << input.coverage.covered_files << '\n';
        report << "  scout_covered_files: " << input.coverage.scout_covered_files << '\n';
        report << "  parent_verified_files: " << input.coverage.parent_verified_files
               << '\n';
        report << "  omitted_files: " << input.coverage.omitted_files << '\n';
        report << "  parent_verified_findings: "
               << input.coverage.parent_verified_findings << '\n';
        report << "failed_slices: " << input.coverage.failed_slices << '\n';
    }
    if (input.coverage.has_usage) {
        report << "usage:\n";
        report << "  prompt_tokens: " << input.coverage.prompt_tokens << '\n';
        report << "  completion_tokens: " << input.coverage.completion_tokens << '\n';
        report << "  cache_hit_tokens: " << input.coverage.cache_hit_tokens << '\n';
        if (input.coverage.has_cost) {
            report << "  estimated_cost_usd: " << input.coverage.estimated_cost_usd
                   << '\n';
        }
    }
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
            report << "- 来源：" << finding.origin;
            if (finding.origin.rfind("scout:", 0) == 0) {
                report << "（父级已核验）";
            }
            report << '\n';
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
    report << "- stale: " << (effective_stale.stale ? "true" : "false") << '\n';
    if (effective_stale.head_changed) {
        report << "- head_changed: true (content digest unchanged)\n";
    }
    for (const auto& reason : effective_stale.reasons) {
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
