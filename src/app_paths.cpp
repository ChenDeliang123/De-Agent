#include "aiagent/app_paths.hpp"

#include <cstdlib>
#include <string>

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
    }
    return paths;
}

}  // namespace aiagent
