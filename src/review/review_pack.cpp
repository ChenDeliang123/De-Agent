#include "aiagent/review/review_pack.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>

#include "aiagent/platform/paths.hpp"
#include "aiagent/platform/text_file.hpp"
#include "aiagent/review/project_verify_config.hpp"
#include "aiagent/review/review_process.hpp"
#include "aiagent/util/sha256.hpp"

namespace aiagent::review {
namespace {

namespace fs = std::filesystem;

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

std::string Lower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

std::string GenericRelative(const fs::path& root, const fs::path& path) {
    std::error_code ec;
    const fs::path canonical_root = fs::weakly_canonical(root, ec);
    const fs::path canonical_path = fs::weakly_canonical(path, ec);
    if (ec) {
        return {};
    }
    if (!aiagent::platform::IsPathWithin(canonical_path, canonical_root)) {
        return {};
    }
    return aiagent::platform::ToGenericUtf8(canonical_path.lexically_relative(canonical_root));
}

bool IsUnderDir(const std::string& relative_path, const std::string& dir) {
    const std::string normalized_dir = NormalizeRelativePath(dir);
    const std::string path = NormalizeRelativePath(relative_path);
    if (normalized_dir.empty() || path.empty()) {
        return false;
    }
    return path == normalized_dir ||
           path.rfind(normalized_dir + "/", 0) == 0;
}

bool IsSensitivePath(const std::string& relative_path) {
    const std::string lower = Lower(relative_path);
    const std::string name = Lower(aiagent::platform::ToUtf8(
            fs::path(aiagent::platform::FromUtf8(lower)).filename()));
    if (name == ".env" || name.rfind(".env.", 0) == 0 || name == "id_rsa" ||
        name == "id_ed25519" || name == "credentials" ||
        name.rfind("credentials.", 0) == 0) {
        return true;
    }
    static const char* kSuffixes[] = {".pem", ".key", ".p12", ".pfx", ".secret"};
    for (const char* suffix : kSuffixes) {
        if (name.size() >= std::char_traits<char>::length(suffix) &&
            name.compare(name.size() - std::char_traits<char>::length(suffix),
                         std::char_traits<char>::length(suffix), suffix) == 0) {
            return true;
        }
    }
    return false;
}

ArgvRunResult RunGit(const std::string& cwd, const std::vector<std::string>& args,
                     std::size_t max_bytes = 4 * 1024 * 1024,
                     long timeout_s = 60) {
    std::vector<std::string> argv = {"git", "-c", "core.quotePath=false"};
    argv.insert(argv.end(), args.begin(), args.end());
    return RunArgvCapture(argv, cwd, timeout_s, max_bytes, false);
}

std::string TrimmedOutput(const ArgvRunResult& result) {
    std::string output = result.output;
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

bool IsValidRevision(const std::string& base) {
    if (base.empty() || base.size() > 256) {
        return false;
    }
    for (const char c : base) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '/' ||
                        c == '^' || c == '~' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string HexSuffix(std::size_t bytes) {
    std::random_device device;
    std::mt19937_64 generator(device());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output;
    for (std::size_t i = 0; i < bytes; ++i) {
        output.push_back(kHex[generator() & 0x0fU]);
    }
    return output;
}

std::string PathFromDiffSection(const std::string& section) {
    std::istringstream lines(section);
    std::string line;
    std::string minus_path;
    while (std::getline(lines, line)) {
        auto clean = [](std::string value) {
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            std::string unescaped;
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '\\' && i + 1 < value.size()) {
                    unescaped.push_back(value[++i]);
                } else {
                    unescaped.push_back(value[i]);
                }
            }
            return value;
        };
        if (line.rfind("+++ b/", 0) == 0) {
            return clean(line.substr(6));
        }
        if (line.rfind("--- a/", 0) == 0) {
            minus_path = clean(line.substr(6));
        }
    }
    return minus_path;
}

std::vector<std::string> SplitDiffSections(const std::string& diff) {
    std::vector<std::string> sections;
    std::size_t pos = 0;
    while (pos < diff.size()) {
        const std::size_t next = diff.find("\ndiff --git ", pos + 1);
        const std::size_t end = next == std::string::npos ? diff.size() : next + 1;
        sections.push_back(diff.substr(pos, end - pos));
        pos = end;
    }
    return sections;
}

std::string FilterDiff(const std::string& raw, const std::string& review_dir,
                       const std::vector<std::string>& excluded_paths,
                       bool drop_sensitive) {
    const std::set<std::string> excluded(excluded_paths.begin(), excluded_paths.end());
    std::string output;
    for (const auto& section : SplitDiffSections(raw)) {
        const std::string path = PathFromDiffSection(section);
        if (!path.empty() &&
            (IsUnderDir(path, review_dir) || excluded.count(path) > 0 ||
             (drop_sensitive && IsSensitivePath(path)))) {
            continue;
        }
        output += section;
    }
    return output;
}

std::vector<std::string> ChangedPathsFromNameStatus(const std::string& text) {
    std::vector<std::string> paths;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        const std::size_t first_tab = line.find('\t');
        if (first_tab == std::string::npos) {
            continue;
        }
        std::string rest = line.substr(first_tab + 1);
        const std::size_t last_tab = rest.rfind('\t');
        if (last_tab != std::string::npos) {
            rest = rest.substr(last_tab + 1);
        }
        if (!rest.empty()) {
            paths.push_back(rest);
        }
    }
    return paths;
}

std::string NormalizeRenamePath(std::string path) {
    const std::size_t arrow = path.find(" => ");
    if (arrow != std::string::npos) {
        path = path.substr(arrow + 4);
    }
    path.erase(std::remove(path.begin(), path.end(), '{'), path.end());
    path.erase(std::remove(path.begin(), path.end(), '}'), path.end());
    return Trim(path);
}

std::string ReadFileLimited(const fs::path& path, std::size_t limit, bool* truncated,
                            std::string* error) {
    std::string content;
    bool was_truncated = false;
    if (!aiagent::platform::ReadFilePrefix(path, limit, &content, &was_truncated, error)) {
        return {};
    }
    if (truncated != nullptr) {
        *truncated = was_truncated;
    }
    return content;
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

std::string RecentCommits(const std::string& repo_root) {
    const ArgvRunResult result =
            RunGit(repo_root, {"log", "-n", "10", "--format=%s"}, 65536);
    if (!result.started || result.exit_code != 0) {
        return {};
    }
    std::ostringstream output;
    output << "Recent commits (weak intent; no request file was provided):\n";
    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty()) {
            output << "- " << line << '\n';
        }
    }
    return output.str();
}

void TrimSessionExcerpt(ReviewSessionExcerpt* session, std::size_t limit) {
    if (session == nullptr || limit == 0) {
        return;
    }
    std::size_t total = 0;
    for (const auto& message : session->user_messages) {
        total += message.size();
    }
    while (total > limit && session->user_messages.size() > 2) {
        total -= session->user_messages[1].size();
        session->user_messages.erase(session->user_messages.begin() + 1);
    }
    if (total <= limit || session->user_messages.size() != 2) {
        return;
    }
    const std::size_t half = limit / 2;
    if (session->user_messages[0].size() > half) {
        session->user_messages[0] =
                aiagent::platform::TruncateUtf8(session->user_messages[0], half) + "\n[truncated]";
    }
    if (session->user_messages[1].size() > half) {
        session->user_messages[1] =
                aiagent::platform::TruncateUtf8(session->user_messages[1], half) + "\n[truncated]";
    }
}

ReviewWorkspaceDigest ComputeDigestInternal(
        const std::string& repo_root, const std::string& base_commit,
        const ReviewConfig& config, const std::string& review_dir,
        const std::vector<std::string>& excluded_paths) {
    ReviewWorkspaceDigest digest;
    const ArgvRunResult diff = RunGit(
            repo_root, {"diff", "--no-color", "--no-ext-diff", "--unified=3", base_commit},
            config.max_diff_bytes, config.review_timeout_s);
    if (!diff.started) {
        digest.error = diff.error;
        return digest;
    }
    if (diff.exit_code != 0) {
        digest.error = "git diff failed: " + TrimmedOutput(diff);
        return digest;
    }
    // digest 必须与 pack 创建时同口径：排除 review 产物与 reviewer-owned 测试，
    // 但不因敏感文件而从摘要中消失（内容变化仍要触发 stale）。
    digest.diff_sha256 = aiagent::util::Sha256Hex(
            FilterDiff(diff.output, review_dir, excluded_paths, /*drop_sensitive=*/false));

    const ArgvRunResult head = RunGit(repo_root, {"rev-parse", "HEAD"}, 4096);
    if (head.started && head.exit_code == 0) {
        digest.head_commit = TrimmedOutput(head);
    }

    const ArgvRunResult untracked =
            RunGit(repo_root, {"ls-files", "--others", "--exclude-standard"}, 4 * 1024 * 1024);
    if (!untracked.started || untracked.exit_code != 0) {
        digest.error = "git ls-files failed: " + TrimmedOutput(untracked);
        return digest;
    }
    std::vector<std::string> paths;
    {
        std::istringstream lines(untracked.output);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.empty()) {
                continue;
            }
            if (IsUnderDir(line, review_dir) ||
                std::find(excluded_paths.begin(), excluded_paths.end(), line) !=
                        excluded_paths.end()) {
                continue;
            }
            paths.push_back(line);
        }
    }
    std::sort(paths.begin(), paths.end());
    std::ostringstream manifest;
    for (const auto& path : paths) {
        std::string hash_error;
        const std::string content_hash =
                aiagent::util::Sha256HexFile(
                        aiagent::platform::FromUtf8(repo_root) / aiagent::platform::FromUtf8(path),
                        &hash_error);
        manifest << path << '\0' << (content_hash.empty() ? "missing" : content_hash) << '\n';
    }
    digest.untracked_sha256 = aiagent::util::Sha256Hex(manifest.str());
    digest.ok = true;
    return digest;
}

}  // namespace

std::string LocalTimestampCompact() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M", &local);
    return buffer;
}

std::string LocalTimestampIso() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local);
    return buffer;
}

std::string NewReviewId() {
    return "rv-" + LocalTimestampCompact() + "-" + HexSuffix(8);
}

std::string NormalizeFeature(const std::string& feature, std::string* error) {
    std::string value = Trim(feature);
    if (value.empty()) {
        value = "review";
    }
    std::string output;
    bool last_dash = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!output.empty() && !last_dash) {
                output.push_back('-');
                last_dash = true;
            }
            continue;
        }
        if (c < 0x20U || c == 0x7fU) {
            if (error != nullptr) {
                *error = "feature contains control characters";
            }
            return {};
        }
        const bool forbidden = c == '/' || c == '\\' || c == ':' || c == '*' ||
                               c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
        if (forbidden) {
            if (error != nullptr) {
                *error = std::string("feature contains a forbidden character: ") +
                         static_cast<char>(c);
            }
            return {};
        }
        const bool allowed_ascii =
                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!allowed_ascii && c < 0x80U) {
            if (error != nullptr) {
                *error = std::string("feature contains an unsupported character: ") +
                         static_cast<char>(c);
            }
            return {};
        }
        output.push_back(static_cast<char>(c));
        last_dash = false;
    }
    while (!output.empty() && (output.back() == '-' || output.back() == ' ')) {
        output.pop_back();
    }
    if (output.empty() || output == "." || output == "..") {
        if (error != nullptr) {
            *error = "feature must not be empty or a dot path";
        }
        return {};
    }
    std::string upper;
    for (const char c : output) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    const std::string base = upper.substr(0, upper.find('.'));
    static const std::set<std::string> reserved = {
            "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
            "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
            "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    if (reserved.count(base) > 0) {
        if (error != nullptr) {
            *error = "feature is a Windows reserved name: " + output;
        }
        return {};
    }
    if (output.size() > 80) {
        if (error != nullptr) {
            *error = "feature is longer than 80 bytes";
        }
        return {};
    }
    return output;
}

std::string ReviewPackDir(const ReviewConfig& config, const std::string& repo_root) {
    return aiagent::platform::ToUtf8(aiagent::platform::FromUtf8(repo_root) /
                                     aiagent::platform::FromUtf8(config.dir));
}

std::string ReviewInboxDir(const ReviewConfig& config, const std::string& repo_root) {
    return aiagent::platform::ToUtf8(aiagent::platform::FromUtf8(ReviewPackDir(config, repo_root)) /
                                     "inbox");
}

std::string ReviewStateDir(const ReviewConfig& config, const std::string& repo_root) {
    return aiagent::platform::ToUtf8(aiagent::platform::FromUtf8(ReviewPackDir(config, repo_root)) /
                                     ".state");
}

nlohmann::json ReviewPackToJson(const ReviewPack& pack) {
    nlohmann::json json;
    json["schema"] = pack.schema;
    json["review_id"] = pack.review_id;
    json["feature"] = pack.feature;
    json["created_at"] = pack.created_at;
    json["cwd"] = pack.cwd;
    json["repo_root"] = pack.repo_root;
    json["git"] = {{"base_ref", pack.git.base_ref},
                   {"base_commit", pack.git.base_commit},
                   {"head_commit", pack.git.head_commit},
                   {"branch", pack.git.branch},
                   {"dirty", pack.git.dirty},
                   {"diff_sha256", pack.git.diff_sha256},
                   {"untracked_sha256", pack.git.untracked_sha256}};
    json["producer"] = {{"kind", pack.producer.kind},
                        {"agent", pack.producer.agent},
                        {"request_file", pack.producer.request_file},
                        {"request_sha256", pack.producer.request_sha256}};
    json["intent_source"] = pack.intent_source;
    json["request_text"] = pack.request_text;
    if (pack.session.has_value()) {
        json["session"] = {{"id", pack.session->id},
                           {"title", pack.session->title},
                           {"user_messages", pack.session->user_messages},
                           {"message_count", pack.session->message_count}};
    } else {
        json["session"] = nullptr;
    }
    json["notes"] = pack.notes;
    json["context_paths"] = nlohmann::json::array();
    for (const auto& path : pack.context_paths) {
        json["context_paths"].push_back({{"path", path.path}, {"sha256", path.sha256}});
    }
    json["project_verify_config_sha256"] = pack.project_verify_config_sha256;
    json["verification"] = nlohmann::json::array();
    for (const auto& command : pack.verification) {
        json["verification"].push_back({{"id", command.id},
                                        {"source", command.source},
                                        {"argv", command.argv},
                                        {"cwd", command.cwd},
                                        {"timeout_s", command.timeout_s},
                                        {"declared_exit_code", command.declared_exit_code},
                                        {"summary", command.summary}});
    }
    json["changed_files"] = nlohmann::json::array();
    for (const auto& file : pack.changed_files) {
        json["changed_files"].push_back({{"path", file.path},
                                         {"status", file.status},
                                         {"added", file.added},
                                         {"removed", file.removed},
                                         {"binary", file.binary},
                                         {"diff_bytes", file.diff_bytes},
                                         {"truncated", file.truncated}});
    }
    json["untracked"] = nlohmann::json::array();
    for (const auto& file : pack.untracked) {
        json["untracked"].push_back({{"path", file.path},
                                     {"content", file.content},
                                     {"truncated", file.truncated},
                                     {"redacted", file.redacted},
                                     {"reason", file.reason}});
    }
    json["omitted"] = nlohmann::json::array();
    for (const auto& file : pack.omitted) {
        json["omitted"].push_back({{"path", file.path}, {"reason", file.reason}});
    }
    json["diff"] = pack.diff;
    json["diff_truncated"] = pack.diff_truncated;
    json["previous_report"] = pack.previous_report;
    json["context_missing"] = pack.context_missing;
    return json;
}

bool LoadReviewPack(const std::string& path, ReviewPack* pack, std::string* error) {
    if (pack == nullptr) {
        if (error != nullptr) {
            *error = "pack output is null";
        }
        return false;
    }
    std::string text;
    bool truncated = false;
    std::string read_error;
    if (!aiagent::platform::ReadFilePrefix(aiagent::platform::FromUtf8(path), 64 * 1024 * 1024,
                                           &text, &truncated, &read_error)) {
        if (error != nullptr) {
            *error = read_error.empty() ? "cannot read review pack" : read_error;
        }
        return false;
    }
    if (truncated) {
        if (error != nullptr) {
            *error = "review pack is too large";
        }
        return false;
    }
    try {
        const auto json = nlohmann::json::parse(text);
        ReviewPack parsed;
        parsed.schema = json.value("schema", 0);
        if (parsed.schema != 1) {
            if (error != nullptr) {
                *error = "unsupported review pack schema";
            }
            return false;
        }
        parsed.review_id = json.value("review_id", std::string());
        parsed.feature = json.value("feature", std::string());
        parsed.created_at = json.value("created_at", std::string());
        parsed.cwd = json.value("cwd", std::string());
        parsed.repo_root = json.value("repo_root", std::string());
        if (json.contains("git") && json["git"].is_object()) {
            const auto& git = json["git"];
            parsed.git.base_ref = git.value("base_ref", std::string("HEAD"));
            parsed.git.base_commit = git.value("base_commit", std::string());
            parsed.git.head_commit = git.value("head_commit", std::string());
            parsed.git.branch = git.value("branch", std::string());
            parsed.git.dirty = git.value("dirty", false);
            parsed.git.diff_sha256 = git.value("diff_sha256", std::string());
            parsed.git.untracked_sha256 = git.value("untracked_sha256", std::string());
        }
        if (json.contains("producer") && json["producer"].is_object()) {
            const auto& producer = json["producer"];
            parsed.producer.kind = producer.value("kind", std::string("manual"));
            parsed.producer.agent = producer.value("agent", std::string());
            parsed.producer.request_file = producer.value("request_file", std::string());
            parsed.producer.request_sha256 = producer.value("request_sha256", std::string());
        }
        parsed.intent_source = json.value("intent_source", std::string("none"));
        parsed.request_text = json.value("request_text", std::string());
        if (json.contains("session") && json["session"].is_object()) {
            ReviewSessionExcerpt session;
            session.id = json["session"].value("id", std::string());
            session.title = json["session"].value("title", std::string());
            session.message_count = json["session"].value("message_count", 0);
            if (json["session"].contains("user_messages") &&
                json["session"]["user_messages"].is_array()) {
                session.user_messages =
                        json["session"]["user_messages"].get<std::vector<std::string>>();
            }
            parsed.session = std::move(session);
        }
        parsed.notes = json.value("notes", std::string());
        if (json.contains("context_paths") && json["context_paths"].is_array()) {
            for (const auto& item : json["context_paths"]) {
                parsed.context_paths.push_back(
                        {item.value("path", std::string()),
                         item.value("sha256", std::string())});
            }
        }
        parsed.project_verify_config_sha256 =
                json.value("project_verify_config_sha256", std::string());
        if (json.contains("verification") && json["verification"].is_array()) {
            for (const auto& item : json["verification"]) {
                ReviewCommand command;
                command.id = item.value("id", std::string());
                command.source = item.value("source", std::string("pack"));
                command.cwd = item.value("cwd", std::string("."));
                command.timeout_s = item.value("timeout_s", 300L);
                command.declared_exit_code = item.value("declared_exit_code", 0);
                command.summary = item.value("summary", std::string());
                if (item.contains("argv") && item["argv"].is_array()) {
                    command.argv = item["argv"].get<std::vector<std::string>>();
                }
                parsed.verification.push_back(std::move(command));
            }
        }
        if (json.contains("changed_files") && json["changed_files"].is_array()) {
            for (const auto& item : json["changed_files"]) {
                ReviewChangedFile file;
                file.path = item.value("path", std::string());
                file.status = item.value("status", std::string());
                file.added = item.value("added", 0);
                file.removed = item.value("removed", 0);
                file.binary = item.value("binary", false);
                file.diff_bytes = item.value("diff_bytes", 0ULL);
                file.truncated = item.value("truncated", false);
                parsed.changed_files.push_back(std::move(file));
            }
        }
        if (json.contains("untracked") && json["untracked"].is_array()) {
            for (const auto& item : json["untracked"]) {
                ReviewUntrackedFile file;
                file.path = item.value("path", std::string());
                file.content = item.value("content", std::string());
                file.truncated = item.value("truncated", false);
                file.redacted = item.value("redacted", false);
                file.reason = item.value("reason", std::string());
                parsed.untracked.push_back(std::move(file));
            }
        }
        if (json.contains("omitted") && json["omitted"].is_array()) {
            for (const auto& item : json["omitted"]) {
                parsed.omitted.push_back(
                        {item.value("path", std::string()), item.value("reason", std::string())});
            }
        }
        parsed.diff = json.value("diff", std::string());
        parsed.diff_truncated = json.value("diff_truncated", false);
        parsed.previous_report = json.value("previous_report", std::string());
        parsed.context_missing = json.value("context_missing", false);
        *pack = std::move(parsed);
        return true;
    } catch (const nlohmann::json::exception& e) {
        if (error != nullptr) {
            *error = std::string("invalid review pack: ") + e.what();
        }
        return false;
    }
}

ReviewWorkspaceDigest ComputeWorkspaceDigest(
        const std::string& repo_root, const std::string& base_commit,
        const ReviewConfig& config, const std::vector<std::string>& excluded_paths) {
    return ComputeDigestInternal(repo_root, base_commit, config,
                                 NormalizeRelativePath(config.dir), excluded_paths);
}

ReviewStaleResult CheckReviewStale(const ReviewPack& pack, const ReviewConfig& config,
                                   const std::string& repo_root,
                                   const std::vector<std::string>& reviewer_owned_paths) {
    ReviewStaleResult result;
    if (aiagent::platform::NormalizePathKey(pack.repo_root) !=
        aiagent::platform::NormalizePathKey(repo_root)) {
        result.stale = true;
        result.reasons.push_back("review pack belongs to a different repository root");
        return result;
    }
    if (pack.git.base_commit.empty()) {
        result.stale = true;
        result.reasons.push_back("review pack has no base commit");
        return result;
    }
    result.digest = ComputeWorkspaceDigest(repo_root, pack.git.base_commit, config,
                                           reviewer_owned_paths);
    if (!result.digest.ok) {
        result.stale = true;
        result.reasons.push_back(result.digest.error);
        return result;
    }
    if (result.digest.diff_sha256 != pack.git.diff_sha256) {
        result.stale = true;
        result.reasons.push_back("tracked diff changed since the pack was created");
    }
    if (result.digest.untracked_sha256 != pack.git.untracked_sha256) {
        result.stale = true;
        result.reasons.push_back("untracked files changed since the pack was created");
    }
    if (!result.digest.head_commit.empty() && !pack.git.head_commit.empty() &&
        result.digest.head_commit != pack.git.head_commit) {
        result.head_changed = true;
    }
    return result;
}

ReviewPackService::ReviewPackService(ReviewPackServiceOptions options)
        : options_(std::move(options)) {}

ReviewPackResult ReviewPackService::CreatePack(const ReviewPackRequest& request) const {
    ReviewPackResult result;
    auto fail = [&result](const std::string& message) {
        result.ok = false;
        result.error = message;
        return result;
    };

    std::error_code ec;
    fs::path cwd = aiagent::platform::FromUtf8(request.cwd);
    if (cwd.empty()) {
        cwd = fs::current_path(ec);
    } else if (!cwd.is_absolute()) {
        cwd = fs::absolute(cwd, ec);
    }
    cwd = fs::weakly_canonical(cwd, ec);
    if (ec || !fs::is_directory(cwd)) {
        return fail("review cwd is not a directory: " + request.cwd);
    }
    const ArgvRunResult top = RunGit(aiagent::platform::ToUtf8(cwd),
                                     {"rev-parse", "--show-toplevel"}, 65536);
    if (!top.started || top.exit_code != 0) {
        return fail("review requires a git repository: " + TrimmedOutput(top));
    }
    const fs::path repo_root =
            fs::weakly_canonical(aiagent::platform::FromUtf8(TrimmedOutput(top)), ec);
    if (ec) {
        return fail("cannot resolve repository root");
    }
    if (!aiagent::platform::IsPathWithin(cwd, repo_root)) {
        return fail("review cwd must be inside the repository root");
    }
    const std::string repo = aiagent::platform::ToUtf8(repo_root);
    const std::string cwd_relative = GenericRelative(repo_root, cwd);
    if (cwd_relative.empty() && aiagent::platform::NormalizePathKey(
                                       aiagent::platform::ToUtf8(cwd)) !=
                                       aiagent::platform::NormalizePathKey(repo)) {
        return fail("review cwd must be inside the repository root");
    }

    std::string feature_error;
    const std::string feature = NormalizeFeature(request.feature, &feature_error);
    if (feature.empty()) {
        return fail("invalid feature: " + feature_error);
    }

    const std::string base = request.base.empty() ? "HEAD" : request.base;
    if (!IsValidRevision(base)) {
        return fail("invalid base revision: " + base);
    }
    const ArgvRunResult base_commit =
            RunGit(repo, {"rev-parse", "--verify", base + "^{commit}"}, 65536);
    if (!base_commit.started || base_commit.exit_code != 0) {
        return fail("cannot resolve base revision '" + base + "': " +
                    TrimmedOutput(base_commit));
    }
    const std::string base_hash = TrimmedOutput(base_commit);
    const ArgvRunResult head = RunGit(repo, {"rev-parse", "HEAD"}, 65536);
    if (!head.started || head.exit_code != 0) {
        return fail("git repository has no HEAD commit");
    }
    const std::string head_hash = TrimmedOutput(head);
    const ArgvRunResult branch = RunGit(repo, {"rev-parse", "--abbrev-ref", "HEAD"}, 65536);
    const std::string branch_name = TrimmedOutput(branch);
    const ArgvRunResult status = RunGit(repo, {"status", "--porcelain"}, 4 * 1024 * 1024);
    const bool dirty = status.started && status.exit_code == 0 && !TrimmedOutput(status).empty();

    const ArgvRunResult raw_diff =
            RunGit(repo, {"diff", "--no-color", "--no-ext-diff", "--unified=3", base_hash},
                   options_.config.max_diff_bytes, options_.config.review_timeout_s);
    if (!raw_diff.started || raw_diff.exit_code != 0) {
        return fail("git diff failed: " + TrimmedOutput(raw_diff));
    }
    const std::string diff_sha = aiagent::util::Sha256Hex(
            FilterDiff(raw_diff.output, options_.config.dir, {},
                       /*drop_sensitive=*/false));

    const ArgvRunResult name_status =
            RunGit(repo, {"diff", "--name-status", base_hash}, 4 * 1024 * 1024);
    if (!name_status.started || name_status.exit_code != 0) {
        return fail("git diff --name-status failed: " + TrimmedOutput(name_status));
    }
    const ArgvRunResult numstat =
            RunGit(repo, {"diff", "--numstat", base_hash}, 4 * 1024 * 1024);
    if (!numstat.started || numstat.exit_code != 0) {
        return fail("git diff --numstat failed: " + TrimmedOutput(numstat));
    }

    std::map<std::string, std::pair<int, int>> stats;
    std::map<std::string, bool> binary;
    {
        std::istringstream lines(numstat.output);
        std::string line;
        while (std::getline(lines, line)) {
            const std::size_t first = line.find('\t');
            if (first == std::string::npos) {
                continue;
            }
            const std::size_t second = line.find('\t', first + 1);
            if (second == std::string::npos) {
                continue;
            }
            const std::string added = line.substr(0, first);
            const std::string removed = line.substr(first + 1, second - first - 1);
            const std::string path = NormalizeRenamePath(line.substr(second + 1));
            if (added == "-" || removed == "-") {
                binary[path] = true;
            } else {
                try {
                    stats[path] = {std::stoi(added), std::stoi(removed)};
                } catch (const std::exception&) {
                    stats[path] = {0, 0};
                }
            }
        }
    }

    std::map<std::string, std::size_t> diff_bytes;
    for (const auto& section : SplitDiffSections(raw_diff.output)) {
        const std::string path = PathFromDiffSection(section);
        if (!path.empty()) {
            diff_bytes[path] = section.size();
        }
    }

    std::vector<ReviewChangedFile> changed_files;
    std::vector<ReviewOmittedFile> omitted;
    for (const auto& path : ChangedPathsFromNameStatus(name_status.output)) {
        if (IsUnderDir(path, options_.config.dir)) {
            continue;  // 审核产物不属于待审代码
        }
        if (changed_files.size() >= options_.config.max_files) {
            omitted.push_back({path, "max_files"});
            continue;
        }
        ReviewChangedFile file;
        file.path = path;
        file.status = "M";
        {
            std::istringstream lines(name_status.output);
            std::string line;
            while (std::getline(lines, line)) {
                if (line.size() > 2 && line.back() == '\n') {
                    line.pop_back();
                }
                const std::size_t first_tab = line.find('\t');
                if (first_tab == std::string::npos) {
                    continue;
                }
                std::string candidate = line.substr(first_tab + 1);
                const std::size_t last_tab = candidate.rfind('\t');
                if (last_tab != std::string::npos) {
                    candidate = candidate.substr(last_tab + 1);
                }
                if (candidate == path) {
                    file.status = line.substr(0, first_tab).substr(0, 1);
                    break;
                }
            }
        }
        if (const auto it = stats.find(path); it != stats.end()) {
            file.added = it->second.first;
            file.removed = it->second.second;
        }
        file.binary = binary.count(path) > 0;
        if (const auto it = diff_bytes.find(path); it != diff_bytes.end()) {
            file.diff_bytes = it->second;
        }
        file.truncated = raw_diff.truncated;
        changed_files.push_back(std::move(file));
    }

    const ArgvRunResult untracked_list =
            RunGit(repo, {"ls-files", "--others", "--exclude-standard"}, 4 * 1024 * 1024);
    if (!untracked_list.started || untracked_list.exit_code != 0) {
        return fail("git ls-files --others failed: " + TrimmedOutput(untracked_list));
    }
    std::vector<std::string> untracked_paths;
    {
        std::istringstream lines(untracked_list.output);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && !IsUnderDir(line, options_.config.dir)) {
                untracked_paths.push_back(line);
            }
        }
    }
    std::sort(untracked_paths.begin(), untracked_paths.end());

    std::vector<ReviewUntrackedFile> untracked;
    std::size_t untracked_bytes = 0;
    std::ostringstream untracked_manifest;
    for (const auto& path : untracked_paths) {
        std::string hash_error;
        const std::string content_hash = aiagent::util::Sha256HexFile(
                repo_root / aiagent::platform::FromUtf8(path), &hash_error);
        untracked_manifest << path << '\0' << (content_hash.empty() ? "missing" : content_hash)
                           << '\n';
        ReviewUntrackedFile file;
        file.path = path;
        if (IsSensitivePath(path)) {
            file.redacted = true;
            file.reason = "secret";
            omitted.push_back({path, "secret"});
            untracked.push_back(std::move(file));
            continue;
        }
        const fs::path full = repo_root / aiagent::platform::FromUtf8(path);
        if (!fs::is_regular_file(full, ec) || ec) {
            omitted.push_back({path, "not_a_file"});
            continue;
        }
        const auto size = fs::file_size(full, ec);
        if (ec || size > options_.config.max_file_bytes) {
            omitted.push_back({path, "too_large"});
            continue;
        }
        std::string read_error;
        std::string content = ReadFileLimited(full, options_.config.max_file_bytes,
                                              &file.truncated, &read_error);
        if (content.empty() && !read_error.empty()) {
            omitted.push_back({path, "unreadable"});
            continue;
        }
        if (aiagent::platform::LooksBinary(content)) {
            omitted.push_back({path, "binary"});
            continue;
        }
        if (untracked_bytes + content.size() > options_.config.max_untracked_bytes) {
            file.reason = "untracked_limit";
            omitted.push_back({path, "untracked_limit"});
            untracked.push_back(std::move(file));
            continue;
        }
        untracked_bytes += content.size();
        file.content = std::move(content);
        untracked.push_back(std::move(file));
    }
    const std::string untracked_sha = aiagent::util::Sha256Hex(untracked_manifest.str());
    if (changed_files.empty() && untracked_paths.empty()) {
        return fail("no changes to review (tracked and untracked are both empty)");
    }

    std::string request_text = request.request_text;
    std::string request_file_rel;
    std::string request_sha;
    if (!request.request_file.empty() && !request.request_text.empty()) {
        return fail("request_file and request_text are mutually exclusive");
    }
    if (!request.request_file.empty()) {
        const fs::path request_path = aiagent::platform::ResolveCanonicalPath(
                repo_root, request.request_file);
        request_file_rel = GenericRelative(repo_root, request_path);
        if (request_file_rel.empty()) {
            return fail("request_file must be inside the repository root");
        }
        std::string read_error;
        bool request_truncated = false;
        std::string content = ReadFileLimited(request_path, 65536, &request_truncated,
                                              &read_error);
        if (!read_error.empty()) {
            return fail("cannot read request_file: " + read_error);
        }
        if (request_truncated) {
            return fail("request_file is larger than 64 KiB");
        }
        if (aiagent::platform::LooksBinary(content)) {
            return fail("request_file must be a text file");
        }
        request_text = std::move(content);
        request_sha = aiagent::util::Sha256Hex(request_text);
    }
    if (request_text.size() > 65536) {
        return fail("request_text is larger than 64 KiB");
    }
    if (!request_text.empty() && request.request_file.empty()) {
        request_sha = aiagent::util::Sha256Hex(request_text);
    }

    std::vector<ReviewContextPath> context_paths;
    if (request.context_paths.size() > 64) {
        return fail("too many context_paths (max 64)");
    }
    for (const auto& raw : request.context_paths) {
        const fs::path path = aiagent::platform::ResolveCanonicalPath(repo_root, raw);
        const std::string rel = GenericRelative(repo_root, path);
        if (rel.empty() || !fs::is_regular_file(path, ec) || ec) {
            return fail("context path is not a file inside the repository: " + raw);
        }
        std::string hash_error;
        const std::string hash = aiagent::util::Sha256HexFile(path, &hash_error);
        if (hash.empty()) {
            return fail("cannot hash context path: " + raw);
        }
        context_paths.push_back({rel, hash});
    }

    std::string previous_report;
    if (!request.previous_report.empty()) {
        const fs::path path =
                aiagent::platform::ResolveCanonicalPath(repo_root, request.previous_report);
        const std::string rel = GenericRelative(repo_root, path);
        if (rel.empty() || !IsUnderDir(rel, options_.config.dir) ||
            !fs::is_regular_file(path, ec) || ec) {
            return fail("previous_report must be a file inside the review directory");
        }
        previous_report = rel;
    }

    ReviewPack pack;
    pack.review_id = NewReviewId();
    pack.feature = feature;
    pack.created_at = LocalTimestampIso();
    pack.cwd = aiagent::platform::ToUtf8(cwd);
    pack.repo_root = repo;
    pack.git.base_ref = base;
    pack.git.base_commit = base_hash;
    pack.git.head_commit = head_hash;
    pack.git.branch = branch_name;
    pack.git.dirty = dirty;
    pack.git.diff_sha256 = diff_sha;
    pack.git.untracked_sha256 = untracked_sha;
    pack.producer = request.producer;
    pack.producer.request_file = request_file_rel;
    pack.producer.request_sha256 = request_sha;
    if (pack.producer.kind.empty()) {
        pack.producer.kind = "manual";
    }
    pack.request_text = request_text;
    pack.notes = request.notes;
    pack.context_paths = std::move(context_paths);
    pack.changed_files = std::move(changed_files);
    pack.untracked = std::move(untracked);
    pack.omitted = std::move(omitted);
    pack.diff = FilterDiff(raw_diff.output, options_.config.dir, {},
                           /*drop_sensitive=*/true);
    pack.diff_truncated = raw_diff.truncated;
    pack.previous_report = previous_report;

    if (request.session.has_value()) {
        pack.session = request.session;
        TrimSessionExcerpt(&pack.session.value(), options_.config.session_excerpt_bytes);
    }

    if (!request.verification.empty()) {
        std::set<std::string> ids;
        for (auto command : request.verification) {
            command.source = "pack";
            if (command.id.empty() || !ids.insert(command.id).second) {
                return fail("verification command ids must be non-empty and unique");
            }
            if (command.timeout_s < 1) {
                command.timeout_s = options_.config.verify_timeout_s;
            }
            if (command.timeout_s > options_.config.verify_timeout_s) {
                return fail("verification timeout exceeds [review] verify_timeout_s: " +
                            command.id);
            }
            if (!IsSafeRelativePath(command.cwd, true)) {
                return fail("verification cwd must stay inside the repository: " +
                            command.id);
            }
            if (!IsPackCommandAllowed(command.argv) ||
                IsForbiddenProjectCommand(command.argv)) {
                return fail("verification command is not allowed in a pack: " + command.id);
            }
            pack.verification.push_back(std::move(command));
        }
    }

    ReviewConsentStore consent(
            options_.data_dir.empty()
                    ? std::string()
                    : aiagent::platform::ToUtf8(
                              aiagent::platform::FromUtf8(options_.data_dir) /
                              "review_consent.json"));
    ProjectVerifyConfig project_config;
    std::string project_error;
    if (LoadProjectVerifyConfig(repo, &project_config, &project_error)) {
        pack.project_verify_config_sha256 = project_config.sha256;
        const ConsentState state = consent.Check(repo, project_config.sha256);
        if (state.status == "valid") {
            for (const auto& command : project_config.commands) {
                ReviewCommand merged;
                merged.id = command.id;
                merged.source = "project";
                merged.argv = command.argv;
                merged.cwd = command.cwd;
                merged.timeout_s = command.timeout_s;
                pack.verification.push_back(std::move(merged));
            }
        }
    } else if (!project_error.empty()) {
        // 坏配置不拖垮 pack：reviewer 会在 manifest 中看到没有 project 命令。
        pack.project_verify_config_sha256.clear();
    }

    if (!request_text.empty()) {
        pack.intent_source = "request-file";
        pack.context_missing = false;
    } else if (pack.session.has_value() && !pack.session->user_messages.empty()) {
        pack.intent_source = "de-session";
        pack.context_missing = false;
    } else {
        pack.intent_source = "git-log";
        pack.request_text = RecentCommits(repo);
        // 只有 git log 不构成完整验收标准：报告必须标注“需求未对齐”。
        pack.context_missing = true;
    }

    const std::string inbox = ReviewInboxDir(options_.config, repo);
    fs::create_directories(aiagent::platform::FromUtf8(inbox), ec);
    if (ec) {
        return fail("cannot create review inbox: " + ec.message());
    }
    std::string basename = feature + "-" + LocalTimestampCompact();
    fs::path pack_path = aiagent::platform::FromUtf8(inbox) /
                         aiagent::platform::FromUtf8(basename + ".json");
    int suffix = 2;
    while (fs::exists(pack_path, ec) && !ec) {
        basename = feature + "-" + LocalTimestampCompact() + "-" + std::to_string(suffix++);
        pack_path = aiagent::platform::FromUtf8(inbox) /
                    aiagent::platform::FromUtf8(basename + ".json");
    }
    const std::string pack_text = ReviewPackToJson(pack).dump(2);
    std::string write_error;
    if (!WriteAtomic(pack_path, pack_text, &write_error)) {
        return fail(write_error);
    }

    result.ok = true;
    result.review_id = pack.review_id;
    result.pack_path = aiagent::platform::ToGenericUtf8(
            pack_path.lexically_relative(repo_root));
    result.report_path_hint = aiagent::platform::ToGenericUtf8(
            (aiagent::platform::FromUtf8(options_.config.dir) /
             aiagent::platform::FromUtf8(basename + ".md")));
    result.intent_source = pack.intent_source;
    result.context_missing = pack.context_missing;
    result.changed_files = pack.changed_files.size();
    result.diff_bytes = raw_diff.output.size();
    result.truncated = raw_diff.truncated;
    return result;
}

}  // namespace aiagent::review
