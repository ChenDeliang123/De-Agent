#include "aiagent/app_paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace aiagent {
namespace {

std::string EnvOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

// 支持形如 "~" / "~/xxx" 的路径；其余原样返回。
std::string ExpandHome(const std::string& path) {
    if (path.empty() || path.front() != '~') {
        return path;
    }
    const std::string home = EnvOrEmpty("HOME");
    if (home.empty()) {
        return path;
    }
    if (path.size() == 1) {
        return home;
    }
    if (path[1] == '/') {
        return home + path.substr(1);
    }
    return path;
}

std::string ExecutablePath() {
    std::error_code ec;
    const std::filesystem::path exe =
            std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return {};
    }
    return exe.string();
}

bool FileExists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

}  // namespace

AppPaths ResolveAppPaths(const std::string& explicit_data_dir) {
    AppPaths paths;
    std::string dir = ExpandHome(explicit_data_dir);
    if (dir.empty()) {
        dir = ExpandHome(EnvOrEmpty("DE_HOME"));
    }
    if (dir.empty()) {
        const std::string xdg = EnvOrEmpty("XDG_DATA_HOME");
        if (!xdg.empty()) {
            dir = xdg + "/de";
        }
    }
    if (dir.empty()) {
        const std::string home = EnvOrEmpty("HOME");
        if (!home.empty()) {
            dir = home + "/.de";
        }
    }
    paths.data_dir = dir;
    if (!dir.empty()) {
        paths.permissions_file = dir + "/permissions.json";
        paths.bash_policy_file = dir + "/bash_policy.rules";
        paths.trust_file = dir + "/trust.json";
        paths.config_file = dir + "/conf/conf.ini";
        paths.runtime_dir = dir + "/runtime";
        paths.db_file = dir + "/aiagent_tasks.db";
    }
    return paths;
}

std::string ResolveShareDir(const std::string& explicit_share_dir,
                            const std::string& exe_path) {
    if (!explicit_share_dir.empty()) {
        return explicit_share_dir;
    }
    const std::string env = EnvOrEmpty("AIAGENT_SHARE_DIR");
    if (!env.empty()) {
        return env;
    }
    const std::string exe = exe_path.empty() ? ExecutablePath() : exe_path;
    if (exe.empty()) {
        return {};
    }
    const std::filesystem::path exe_dir = std::filesystem::path(exe).parent_path();
    return (exe_dir / ".." / "share" / "de").lexically_normal().string();
}

std::string ResolveConfigFile(const std::string& explicit_path,
                              const AppPaths& paths,
                              const std::string& share_dir,
                              const std::string& cwd) {
    if (!explicit_path.empty()) {
        return explicit_path;
    }
    const std::string base =
            cwd.empty() ? std::filesystem::current_path().string() : cwd;
    std::vector<std::string> candidates;
    if (!base.empty()) {
        candidates.push_back(base + "/conf/conf.ini");
    }
    if (!paths.data_dir.empty()) {
        candidates.push_back(paths.data_dir + "/conf/conf.ini");
    }
    if (!share_dir.empty()) {
        candidates.push_back(share_dir + "/conf/conf.ini");
    }
    for (const auto& candidate : candidates) {
        if (FileExists(candidate)) {
            return candidate;
        }
    }
    return {};
}

}  // namespace aiagent
