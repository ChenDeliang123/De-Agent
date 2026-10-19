#include "aiagent/app_paths.hpp"

#include "aiagent/platform/paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace aiagent {
namespace {

std::string EnvOrEmpty(const char* name) {
    return platform::GetEnvUtf8(name).value_or("");
}

std::string JoinPath(const std::string& base, const std::string& child) {
    return platform::ToUtf8(platform::FromUtf8(base) / platform::FromUtf8(child));
}

// 支持形如 "~" / "~/xxx" / "~\xxx" 的路径；其余原样返回。
std::string ExpandHome(const std::string& path) {
    if (path.empty() || path.front() != '~') {
        return path;
    }
#ifdef _WIN32
    std::string home = EnvOrEmpty("USERPROFILE");
    if (home.empty()) {
        home = EnvOrEmpty("HOME");
    }
#else
    const std::string home = EnvOrEmpty("HOME");
#endif
    if (home.empty()) {
        return path;
    }
    if (path.size() == 1) {
        return home;
    }
    if (path[1] == '/' || path[1] == '\\') {
        return home + path.substr(1);
    }
    return path;
}

bool FileExists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(platform::FromUtf8(path), ec) && !ec;
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
            dir = JoinPath(xdg, "de");
        }
    }
    if (dir.empty()) {
#ifdef _WIN32
        // 与 Linux 的 ~/.de 保持一致；USERPROFILE 不可用时退回 LOCALAPPDATA。
        const std::string profile = EnvOrEmpty("USERPROFILE");
        if (!profile.empty()) {
            dir = JoinPath(profile, ".de");
        } else {
            const std::string local_app_data = EnvOrEmpty("LOCALAPPDATA");
            if (!local_app_data.empty()) {
                dir = JoinPath(local_app_data, "de");
            }
        }
#else
        const std::string home = EnvOrEmpty("HOME");
        if (!home.empty()) {
            dir = JoinPath(home, ".de");
        }
#endif
    }
    paths.data_dir = dir;
    if (!dir.empty()) {
        paths.permissions_file = JoinPath(dir, "permissions.json");
        paths.bash_policy_file = JoinPath(dir, "bash_policy.rules");
        paths.trust_file = JoinPath(dir, "trust.json");
        paths.config_file = JoinPath(dir, "conf/conf.ini");
        paths.runtime_dir = JoinPath(dir, "runtime");
        paths.db_file = JoinPath(dir, "aiagent_tasks.db");
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
    const std::string exe = exe_path.empty() ? platform::ExecutablePath() : exe_path;
    if (exe.empty()) {
        return {};
    }
    const std::filesystem::path exe_dir = platform::FromUtf8(exe).parent_path();
    return platform::ToUtf8((exe_dir / ".." / "share" / "de").lexically_normal());
}

std::string ResolveConfigFile(const std::string& explicit_path,
                              const AppPaths& paths,
                              const std::string& share_dir,
                              const std::string& cwd) {
    if (!explicit_path.empty()) {
        return explicit_path;
    }
    const std::string base =
            cwd.empty() ? platform::ToUtf8(std::filesystem::current_path()) : cwd;
    std::vector<std::string> candidates;
    if (!base.empty()) {
        candidates.push_back(JoinPath(base, "conf/conf.ini"));
    }
    if (!paths.data_dir.empty()) {
        candidates.push_back(JoinPath(paths.data_dir, "conf/conf.ini"));
    }
    if (!share_dir.empty()) {
        candidates.push_back(JoinPath(share_dir, "conf/conf.ini"));
    }
    for (const auto& candidate : candidates) {
        if (FileExists(candidate)) {
            return candidate;
        }
    }
    return {};
}

}  // namespace aiagent
