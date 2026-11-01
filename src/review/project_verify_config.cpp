#include "aiagent/review/project_verify_config.hpp"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "aiagent/platform/paths.hpp"
#include "aiagent/platform/text_file.hpp"
#include "aiagent/util/sha256.hpp"

namespace aiagent::review {
namespace {

namespace fs = std::filesystem;

std::string Lower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

bool IsSimpleCommandId(const std::string& id) {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string NowIsoLocal() {
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

bool HasFile(const fs::path& root, const std::string& name) {
    std::error_code ec;
    return fs::is_regular_file(root / aiagent::platform::FromUtf8(name), ec) && !ec;
}

std::string ReadPrefix(const fs::path& path, std::size_t max_bytes) {
    std::string content;
    bool truncated = false;
    std::string error;
    if (!aiagent::platform::ReadFilePrefix(path, max_bytes, &content, &truncated, &error)) {
        return {};
    }
    return content;
}

ProjectVerifyCommand MakeCommand(std::string id, std::string kind,
                                 std::vector<std::string> argv,
                                 std::string cwd = ".", long timeout_s = 600) {
    ProjectVerifyCommand command;
    command.id = std::move(id);
    command.kind = std::move(kind);
    command.argv = std::move(argv);
    command.cwd = std::move(cwd);
    command.timeout_s = timeout_s;
    return command;
}

bool GlobMatchAt(const std::string& pattern, std::size_t p, const std::string& path,
                 std::size_t s) {
    while (p < pattern.size()) {
        if (pattern[p] == '*') {
            const bool double_star = p + 1 < pattern.size() && pattern[p + 1] == '*';
            const std::size_t next = double_star ? p + 2 : p + 1;
            if (double_star && next < pattern.size() && pattern[next] == '/') {
                // `**/` 可匹配零个或多个目录。
                if (GlobMatchAt(pattern, next + 1, path, s)) {
                    return true;
                }
            }
            for (std::size_t skip = s; skip <= path.size(); ++skip) {
                if (!double_star && skip > s && path[skip - 1] == '/') {
                    break;
                }
                if (GlobMatchAt(pattern, next, path, skip)) {
                    return true;
                }
            }
            return false;
        }
        if (s >= path.size()) {
            return false;
        }
        if (pattern[p] == '?') {
            if (path[s] == '/') {
                return false;
            }
            ++p;
            ++s;
            continue;
        }
        if (pattern[p] != path[s]) {
            return false;
        }
        ++p;
        ++s;
    }
    return s == path.size();
}

bool GlobMatch(const std::string& pattern, const std::string& path) {
    return GlobMatchAt(pattern, 0, path, 0);
}

bool LooksLikeTestGlob(const std::string& glob) {
    const std::string lower = Lower(glob);
    if (lower == "**/*" || lower == "*" || lower == "**" || lower == "src/**") {
        return false;
    }
    static const char* kMarkers[] = {"test",  "spec",   "fixture",
                                     "mock",  "__tests__", "testdata"};
    for (const char* marker : kMarkers) {
        if (lower.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool HasSuffix(const std::string& lower, const std::string& suffix) {
    return lower.size() >= suffix.size() &&
           lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::string NormalizeRelativePath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.rfind("./", 0) == 0) {
        path.erase(0, 2);
    }
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

bool IsSafeRelativePath(const std::string& path, bool allow_current) {
    const std::string normalized = NormalizeRelativePath(path);
    if (normalized.empty()) {
        return allow_current;
    }
    if (normalized.front() == '/' || normalized.find('/') == 0 ||
        normalized.find(':') != std::string::npos || normalized.find('\0') != std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= normalized.size()) {
        const std::size_t slash = normalized.find('/', start);
        const std::string part =
                normalized.substr(start, slash == std::string::npos ? std::string::npos
                                                                    : slash - start);
        if (part == ".." || part.empty()) {
            return false;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

bool IsTestPath(const std::string& relative_path,
                const std::vector<std::string>& test_globs) {
    const std::string path = NormalizeRelativePath(relative_path);
    if (path.empty()) {
        return false;
    }
    const std::string lower = Lower(path);

    std::size_t start = 0;
    while (start <= lower.size()) {
        const std::size_t slash = lower.find('/', start);
        const std::string segment =
                lower.substr(start, slash == std::string::npos ? std::string::npos
                                                               : slash - start);
        static const std::set<std::string> kTestDirs = {
                "test", "tests", "testing", "spec", "specs", "__tests__",
                "testdata", "fixtures", "mocks", "test_support"};
        if (kTestDirs.count(segment) > 0) {
            return true;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }

    static const char* kSuffixes[] = {
            "_test.go",  "_test.cpp", "_test.cc",  "_test.c",   "_test.py",
            "_test.rs",  ".test.js",  ".test.ts",  ".test.jsx", ".test.tsx",
            ".spec.js",  ".spec.ts",  ".spec.jsx", ".spec.tsx", "test.java",
            "tests.cs",  "test.php",  "_spec.rb",  "_test.exs"};
    for (const char* suffix : kSuffixes) {
        if (HasSuffix(lower, suffix)) {
            return true;
        }
    }
    for (const std::string& glob : test_globs) {
        if (GlobMatch(NormalizeRelativePath(glob), path)) {
            return true;
        }
    }
    return false;
}

bool IsPackCommandAllowed(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return false;
    }
    std::string exe = Lower(aiagent::platform::ToUtf8(
            fs::path(aiagent::platform::FromUtf8(argv[0])).filename()));
    if (exe == "cmake.exe") {
        exe = "cmake";
    }
    auto has_at = [&argv](std::size_t index, const std::string& value) {
        return index < argv.size() && argv[index] == value;
    };
    auto starts_with = [&argv](std::size_t index, const std::string& prefix) {
        return index < argv.size() && argv[index].rfind(prefix, 0) == 0;
    };
    if (exe == "cmake") {
        return has_at(1, "--build") || starts_with(1, "-S") || starts_with(1, "-B");
    }
    if (exe == "ctest" || exe == "pytest" || exe == "phpunit" || exe == "tox") {
        return true;
    }
    if (exe == "make" || exe == "ninja" || exe == "meson") {
        return !IsForbiddenProjectCommand(argv);
    }
    if (exe == "node" || exe == "node.exe") {
        return starts_with(1, "--test");
    }
    if (exe == "npm" || exe == "npm.cmd" || exe == "pnpm" || exe == "yarn") {
        return has_at(1, "test") || (has_at(1, "run") && has_at(2, "test"));
    }
    if (exe == "python3" || exe == "python" || exe == "python.exe") {
        return has_at(1, "-m") && has_at(2, "pytest");
    }
    if (exe == "cargo") {
        return has_at(1, "test") || has_at(1, "build") || has_at(1, "check");
    }
    if (exe == "go") {
        return has_at(1, "test") || has_at(1, "build") || has_at(1, "vet");
    }
    if (exe == "dotnet" || exe == "mvn" || exe == "gradle" || exe == "gradlew" ||
        exe == "gradlew.bat") {
        return has_at(1, "test") || (exe == "gradle" && has_at(1, "build"));
    }
    if (exe == "bazel" || exe == "bazelisk") {
        return has_at(1, "test") || has_at(1, "build");
    }
    if (exe == "swift" || exe == "xcodebuild") {
        return has_at(1, "build") || has_at(1, "test");
    }
    if (exe == "mix" || exe == "sbt") {
        return has_at(1, "compile") || has_at(1, "test");
    }
    if (exe == "bundle") {
        return has_at(1, "exec") &&
               (has_at(2, "rspec") || (has_at(2, "rake") && has_at(3, "test")));
    }
    if (exe == "deno" || exe == "bun") {
        return has_at(1, "test") || has_at(1, "task") || has_at(1, "run");
    }
    if (exe == "composer") {
        return has_at(1, "test") || (has_at(1, "run-script") && has_at(2, "test"));
    }
    if (exe == "r" || exe == "r.exe") {
        return has_at(1, "CMD") && (has_at(2, "build") || has_at(2, "check"));
    }
    return false;
}

bool IsForbiddenProjectCommand(const std::vector<std::string>& argv) {
    static const std::set<std::string> kForbidden = {
            "install", "uninstall", "publish", "deploy", "upload", "release"};
    const std::size_t limit = std::min<std::size_t>(argv.size(), 4);
    for (std::size_t i = 1; i < limit; ++i) {
        if (kForbidden.count(Lower(argv[i])) > 0) {
            return true;
        }
    }
    return false;
}

std::string ProjectVerifyConfigPath(const std::string& repo_root) {
    return aiagent::platform::ToUtf8(aiagent::platform::FromUtf8(repo_root) / ".de" /
                                     "review.json");
}

bool ValidateProjectVerifyConfig(const ProjectVerifyConfig& config,
                                 const std::string& repo_root, std::string* error) {
    auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (config.version != 1) {
        return fail("review.json version must be 1");
    }
    if (config.commands.size() > 8) {
        return fail("review.json may contain at most 8 commands");
    }
    std::set<std::string> ids;
    for (const auto& command : config.commands) {
        if (!IsSimpleCommandId(command.id)) {
            return fail("invalid review command id: " + command.id);
        }
        if (!ids.insert(command.id).second) {
            return fail("duplicate review command id: " + command.id);
        }
        if (command.kind != "build" && command.kind != "test") {
            return fail("review command kind must be build or test: " + command.id);
        }
        if (command.timeout_s < 1 || command.timeout_s > 900) {
            return fail("review command timeout_s out of range: " + command.id);
        }
        if (!IsSafeRelativePath(command.cwd, true)) {
            return fail("review command cwd must stay inside the project: " + command.id);
        }
        if (command.argv.empty() || command.argv.size() > 64) {
            return fail("review command argv must have 1..64 entries: " + command.id);
        }
        std::size_t total = 0;
        for (const auto& arg : command.argv) {
            if (arg.empty() || arg.size() > 4096 ||
                arg.find('\0') != std::string::npos || arg.find('\n') != std::string::npos ||
                arg.find('\r') != std::string::npos) {
                return fail("invalid review command argument: " + command.id);
            }
            total += arg.size();
        }
        if (total > 16384) {
            return fail("review command argv is too long: " + command.id);
        }
        if (IsForbiddenProjectCommand(command.argv)) {
            return fail("review command uses a forbidden install/publish subcommand: " +
                        command.id);
        }
    }
    if (config.test_globs.size() > 16) {
        return fail("review.json may contain at most 16 test_globs");
    }
    for (const auto& glob : config.test_globs) {
        if (!IsSafeRelativePath(glob, false)) {
            return fail("test_globs entry must be a relative path inside the project");
        }
        if (!LooksLikeTestGlob(glob)) {
            return fail("test_globs entry must look like a test path: " + glob);
        }
    }
    (void)repo_root;
    return true;
}

bool LoadProjectVerifyConfig(const std::string& repo_root, ProjectVerifyConfig* config,
                             std::string* error) {
    if (config == nullptr) {
        if (error != nullptr) {
            *error = "config output is null";
        }
        return false;
    }
    const std::string path = ProjectVerifyConfigPath(repo_root);
    std::error_code ec;
    if (!fs::is_regular_file(aiagent::platform::FromUtf8(path), ec) || ec) {
        if (error != nullptr) {
            error->clear();
        }
        return false;
    }
    std::string text;
    bool truncated = false;
    std::string read_error;
    if (!aiagent::platform::ReadFilePrefix(aiagent::platform::FromUtf8(path), 262144, &text,
                                           &truncated, &read_error) ||
        truncated) {
        if (error != nullptr) {
            *error = read_error.empty() ? "review.json is too large or unreadable"
                                        : read_error;
        }
        return false;
    }
    try {
        const auto json = nlohmann::json::parse(text);
        ProjectVerifyConfig parsed;
        parsed.version = json.value("version", 1);
        parsed.language = json.value("language", std::string());
        parsed.approved_at = json.value("approved_at", std::string());
        if (json.contains("detected_from") && json["detected_from"].is_array()) {
            parsed.detected_from = json["detected_from"].get<std::vector<std::string>>();
        }
        if (json.contains("test_globs") && json["test_globs"].is_array()) {
            parsed.test_globs = json["test_globs"].get<std::vector<std::string>>();
        }
        if (json.contains("commands") && json["commands"].is_array()) {
            for (const auto& item : json["commands"]) {
                ProjectVerifyCommand command;
                command.id = item.value("id", std::string());
                command.kind = item.value("kind", std::string("test"));
                command.cwd = item.value("cwd", std::string("."));
                command.timeout_s = item.value("timeout_s", 600L);
                if (item.contains("argv") && item["argv"].is_array()) {
                    command.argv = item["argv"].get<std::vector<std::string>>();
                }
                parsed.commands.push_back(std::move(command));
            }
        }
        std::string validation_error;
        if (!ValidateProjectVerifyConfig(parsed, repo_root, &validation_error)) {
            if (error != nullptr) {
                *error = validation_error;
            }
            return false;
        }
        parsed.sha256 = aiagent::util::Sha256Hex(text);
        *config = std::move(parsed);
        return true;
    } catch (const nlohmann::json::exception& e) {
        if (error != nullptr) {
            *error = std::string("invalid review.json: ") + e.what();
        }
        return false;
    }
}

bool WriteProjectVerifyConfig(const std::string& repo_root,
                              const ProjectVerifyConfig& config, std::string* error) {
    std::string validation_error;
    if (!ValidateProjectVerifyConfig(config, repo_root, &validation_error)) {
        if (error != nullptr) {
            *error = validation_error;
        }
        return false;
    }
    const fs::path de_dir = aiagent::platform::FromUtf8(repo_root) / ".de";
    std::error_code ec;
    fs::create_directories(de_dir, ec);
    if (ec) {
        if (error != nullptr) {
            *error = "cannot create .de directory: " + ec.message();
        }
        return false;
    }

    nlohmann::json json;
    json["version"] = 1;
    if (!config.language.empty()) {
        json["language"] = config.language;
    }
    json["detected_from"] = config.detected_from;
    json["approved_at"] = config.approved_at.empty() ? NowIsoLocal() : config.approved_at;
    json["test_globs"] = config.test_globs;
    json["commands"] = nlohmann::json::array();
    for (const auto& command : config.commands) {
        json["commands"].push_back({{"id", command.id},
                                    {"kind", command.kind},
                                    {"argv", command.argv},
                                    {"cwd", command.cwd},
                                    {"timeout_s", command.timeout_s}});
    }
    const fs::path target = de_dir / "review.json";
    const fs::path temp = de_dir / "review.json.tmp";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error != nullptr) {
                *error = "cannot write review.json";
            }
            return false;
        }
        output << json.dump(2) << '\n';
        if (!output.good()) {
            if (error != nullptr) {
                *error = "cannot write review.json";
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
            *error = "cannot replace review.json: " + ec.message();
        }
        return false;
    }

    const fs::path gitignore = de_dir / ".gitignore";
    bool has_star = false;
    if (fs::exists(gitignore, ec) && !ec) {
        const std::string content = ReadPrefix(gitignore, 65536);
        std::istringstream lines(content);
        std::string line;
        while (std::getline(lines, line)) {
            if (Trim(line) == "*") {
                has_star = true;
                break;
            }
        }
    }
    if (!has_star) {
        std::ofstream output(gitignore, std::ios::binary | std::ios::app);
        if (output) {
            output << "*\n";
        }
    }
    return true;
}

VerifyDetectResult DetectProjectVerifyConfig(const std::string& repo_root) {
    VerifyDetectResult result;
    const fs::path root = aiagent::platform::FromUtf8(repo_root);
    std::error_code ec;

    const auto add_common_globs = [&result](std::vector<std::string> globs) {
        result.test_globs = std::move(globs);
    };

    if (HasFile(root, "CMakeLists.txt")) {
        result.found = true;
        result.language = "cpp";
        result.detected_from = {"CMakeLists.txt"};
        result.commands = {
                MakeCommand("build", "build", {"cmake", "-S", ".", "-B", "build"}),
                MakeCommand("build_compile", "build",
                            {"cmake", "--build", "build", "-j"}),
                MakeCommand("test", "test",
                            {"ctest", "--test-dir", "build", "--output-on-failure"}, ".",
                            900)};
        add_common_globs({"tests/**", "test/**", "src/**/*_test.cpp",
                          "src/**/*_test.cc", "src/**/*_test.c"});
        return result;
    }
    if (HasFile(root, "meson.build")) {
        result.found = true;
        result.language = "meson";
        result.detected_from = {"meson.build"};
        result.commands = {MakeCommand("build", "build", {"meson", "setup", "build"}),
                           MakeCommand("compile", "build", {"meson", "compile", "-C", "build"}),
                           MakeCommand("test", "test", {"meson", "test", "-C", "build"}, ".",
                                       900)};
        add_common_globs({"tests/**", "test/**"});
        return result;
    }
    if (HasFile(root, "WORKSPACE") || HasFile(root, "WORKSPACE.bazel") ||
        HasFile(root, "MODULE.bazel")) {
        result.found = true;
        result.language = "bazel";
        result.detected_from = {"WORKSPACE"};
        result.commands = {MakeCommand("build", "build", {"bazel", "build", "//..."}),
                           MakeCommand("test", "test", {"bazel", "test", "//..."}, ".",
                                       900)};
        add_common_globs({"**/*_test.*", "**/*_test", "**/testdata/**"});
        return result;
    }
    if (HasFile(root, "Cargo.toml")) {
        result.found = true;
        result.language = "rust";
        result.detected_from = {"Cargo.toml"};
        result.commands = {MakeCommand("build", "build", {"cargo", "build"}),
                           MakeCommand("test", "test", {"cargo", "test"}, ".", 900)};
        add_common_globs({"tests/**", "src/**/*_test.rs"});
        return result;
    }
    if (HasFile(root, "go.mod")) {
        result.found = true;
        result.language = "go";
        result.detected_from = {"go.mod"};
        result.commands = {MakeCommand("build", "build", {"go", "build", "./..."}),
                           MakeCommand("test", "test", {"go", "test", "./..."}, ".", 900)};
        add_common_globs({"**/*_test.go"});
        return result;
    }
    if (HasFile(root, "package.json")) {
        result.found = true;
        result.language = "node";
        result.detected_from = {"package.json"};
        const std::string text = ReadPrefix(root / "package.json", 262144);
        bool has_build = false;
        bool has_test = false;
        try {
            const auto json = nlohmann::json::parse(text);
            if (json.contains("scripts") && json["scripts"].is_object()) {
                has_build = json["scripts"].contains("build");
                has_test = json["scripts"].contains("test");
            }
        } catch (const nlohmann::json::exception&) {
            // 坏 package.json：只给测试命令。
        }
        const std::string runner = HasFile(root, "pnpm-lock.yaml")
                                           ? "pnpm"
                                           : (HasFile(root, "yarn.lock") ? "yarn" : "npm");
        if (has_build) {
            result.commands.push_back(
                    MakeCommand("build", "build", {runner, "run", "build"}, ".", 600));
        }
        if (has_test) {
            result.commands.push_back(MakeCommand("test", "test", {runner, "test"}, ".", 900));
        }
        add_common_globs({"**/*.test.js", "**/*.test.ts", "**/*.test.jsx",
                          "**/*.test.tsx", "**/*.spec.js", "**/*.spec.ts", "tests/**",
                          "__tests__/**"});
        return result;
    }
    if (HasFile(root, "pyproject.toml") || HasFile(root, "tox.ini") ||
        HasFile(root, "pytest.ini")) {
        result.found = true;
        result.language = "python";
        result.detected_from = {HasFile(root, "pyproject.toml") ? "pyproject.toml"
                                                                : "pytest.ini"};
        if (HasFile(root, "pyproject.toml")) {
            result.commands.push_back(
                    MakeCommand("build", "build", {"python3", "-m", "build"}, ".", 600));
        }
        result.commands.push_back(
                MakeCommand("test", "test", {"python3", "-m", "pytest"}, ".", 900));
        add_common_globs({"tests/**", "test/**", "**/*_test.py", "**/test_*.py"});
        return result;
    }
    if (HasFile(root, "mix.exs")) {
        result.found = true;
        result.language = "elixir";
        result.detected_from = {"mix.exs"};
        result.commands = {MakeCommand("build", "build", {"mix", "compile"}),
                           MakeCommand("test", "test", {"mix", "test"}, ".", 900)};
        add_common_globs({"test/**", "**/*_test.exs"});
        return result;
    }
    if (HasFile(root, "build.sbt")) {
        result.found = true;
        result.language = "scala";
        result.detected_from = {"build.sbt"};
        result.commands = {MakeCommand("build", "build", {"sbt", "compile"}),
                           MakeCommand("test", "test", {"sbt", "test"}, ".", 900)};
        add_common_globs({"src/test/**", "**/*Test.scala"});
        return result;
    }
    if (HasFile(root, "DESCRIPTION")) {
        result.found = true;
        result.language = "r";
        result.detected_from = {"DESCRIPTION"};
        result.commands = {MakeCommand("build", "build", {"R", "CMD", "build", "."}),
                           MakeCommand("test", "test", {"R", "CMD", "check", "."}, ".",
                                       900)};
        add_common_globs({"tests/**", "testthat/**"});
        return result;
    }
    if (HasFile(root, "pom.xml")) {
        result.found = true;
        result.language = "java";
        result.detected_from = {"pom.xml"};
        result.commands = {MakeCommand("build", "build",
                                       {"mvn", "-q", "-DskipTests", "package"}, ".", 600),
                           MakeCommand("test", "test", {"mvn", "test"}, ".", 900)};
        add_common_globs({"src/test/**", "**/*Test.java", "**/*Tests.java"});
        return result;
    }
    if (HasFile(root, "build.gradle") || HasFile(root, "build.gradle.kts")) {
        result.found = true;
        result.language = "java";
        result.detected_from = {"build.gradle"};
        const std::string runner = HasFile(root, "gradlew") ? "./gradlew" : "gradle";
        result.commands = {MakeCommand("build", "build", {runner, "build"}, ".", 600),
                           MakeCommand("test", "test", {runner, "test"}, ".", 900)};
        add_common_globs({"src/test/**", "**/*Test.java", "**/*Test.kt"});
        return result;
    }
    if (HasFile(root, "Package.swift")) {
        result.found = true;
        result.language = "swift";
        result.detected_from = {"Package.swift"};
        result.commands = {MakeCommand("build", "build", {"swift", "build"}),
                           MakeCommand("test", "test", {"swift", "test"}, ".", 900)};
        add_common_globs({"Tests/**"});
        return result;
    }
    if (HasFile(root, "composer.json")) {
        result.found = true;
        result.language = "php";
        result.detected_from = {"composer.json"};
        result.commands.push_back(MakeCommand("test", "test", {"composer", "test"}, ".", 900));
        add_common_globs({"tests/**", "**/*Test.php"});
        return result;
    }
    // .NET：根目录的 sln/csproj。
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string name = aiagent::platform::ToUtf8(entry.path().filename());
        if (name.size() > 4 && (name.compare(name.size() - 4, 4, ".sln") == 0 ||
                                name.compare(name.size() - 5, 5, ".csproj") == 0)) {
            result.found = true;
            result.language = "dotnet";
            result.detected_from = {name};
            result.commands = {MakeCommand("build", "build", {"dotnet", "build"}, ".", 600),
                               MakeCommand("test", "test", {"dotnet", "test"}, ".", 900)};
            add_common_globs({"**/*Tests.cs", "**/*Test.cs", "tests/**"});
            return result;
        }
    }
    if (HasFile(root, "Makefile")) {
        const std::string makefile = ReadPrefix(root / "Makefile", 262144);
        result.found = true;
        result.language = "make";
        result.detected_from = {"Makefile"};
        result.commands.push_back(MakeCommand("build", "build", {"make", "-j"}, ".", 600));
        if (makefile.find("\ntest:") != std::string::npos ||
            makefile.rfind("test:", 0) == 0) {
            result.commands.push_back(MakeCommand("test", "test", {"make", "test"}, ".", 900));
        } else if (makefile.find("\ncheck:") != std::string::npos ||
                   makefile.rfind("check:", 0) == 0) {
            result.commands.push_back(
                    MakeCommand("test", "test", {"make", "check"}, ".", 900));
        }
        add_common_globs({"tests/**", "test/**"});
        return result;
    }
    return result;
}

ReviewConsentStore::ReviewConsentStore(std::string path) : path_(std::move(path)) {
    if (!path_.empty()) {
        LoadLocked();
    }
}

void ReviewConsentStore::LoadLocked() {
    std::ifstream input(aiagent::platform::FromUtf8(path_));
    if (!input) {
        return;
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    try {
        const auto json = nlohmann::json::parse(buffer.str());
        if (!json.is_object()) {
            return;
        }
        for (auto it = json.begin(); it != json.end(); ++it) {
            if (!it.value().is_object()) {
                continue;
            }
            Entry entry;
            entry.config_sha256 = it.value().value("config_sha256", std::string());
            entry.approved_at = it.value().value("approved_at", std::string());
            if (!entry.config_sha256.empty()) {
                entries_[it.key()] = std::move(entry);
            }
        }
    } catch (const nlohmann::json::exception&) {
        // 损坏的同意记录按空处理；下次保存会重建。
    }
}

bool ReviewConsentStore::SaveLocked() const {
    if (path_.empty()) {
        return true;
    }
    nlohmann::json json = nlohmann::json::object();
    for (const auto& [root, entry] : entries_) {
        json[root] = {{"config_sha256", entry.config_sha256},
                      {"approved_at", entry.approved_at}};
    }
    const fs::path target = aiagent::platform::FromUtf8(path_);
    std::error_code ec;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
    }
    const fs::path temp = aiagent::platform::FromUtf8(path_ + ".tmp");
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            return false;
        }
        output << json.dump(2) << '\n';
    }
    fs::rename(temp, target, ec);
    if (ec) {
        fs::remove(target, ec);
        fs::rename(temp, target, ec);
    }
    return !ec;
}

ConsentState ReviewConsentStore::Check(const std::string& repo_root,
                                       const std::string& current_config_sha256) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ConsentState state;
    const auto it = entries_.find(repo_root);
    if (it == entries_.end()) {
        state.status = "missing";
        return state;
    }
    state.approved_at = it->second.approved_at;
    state.config_sha256 = it->second.config_sha256;
    state.status =
            (!current_config_sha256.empty() &&
             it->second.config_sha256 == current_config_sha256)
                    ? "valid"
                    : "invalid";
    return state;
}

bool ReviewConsentStore::Approve(const std::string& repo_root,
                                 const std::string& config_sha256, std::string* error) {
    if (repo_root.empty() || config_sha256.empty()) {
        if (error != nullptr) {
            *error = "review consent requires repo_root and config sha256";
        }
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[repo_root] = {config_sha256, NowIsoLocal()};
    if (!SaveLocked()) {
        if (error != nullptr) {
            *error = "cannot write review consent file";
        }
        return false;
    }
    return true;
}

}  // namespace aiagent::review
