// AppPaths 用户级数据目录与安装路径解析的单元测试。

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "aiagent/app_paths.hpp"
#include "aiagent/platform/paths.hpp"

#include "test_support.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

std::string Join(const std::string& base, const std::string& child) {
    return aiagent::platform::ToUtf8(
            (aiagent::platform::FromUtf8(base) /
             aiagent::platform::FromUtf8(child))
                    .lexically_normal());
}

void ClearEnv() {
    test_support::UnsetEnv("DE_HOME");
    test_support::UnsetEnv("XDG_DATA_HOME");
    test_support::UnsetEnv("HOME");
    test_support::UnsetEnv("USERPROFILE");
    test_support::UnsetEnv("LOCALAPPDATA");
    test_support::UnsetEnv("AIAGENT_SHARE_DIR");
}

void WriteFile(const std::string& path) {
    const std::filesystem::path file = aiagent::platform::FromUtf8(path);
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "[deepseek]\n";
}

void TestExplicitWins() {
    ClearEnv();
    const std::string explicit_dir = test_support::TempPath("aiagent_paths_explicit");
    test_support::SetEnv("DE_HOME", test_support::TempPath("aiagent_paths_home").c_str());
    const auto paths = aiagent::ResolveAppPaths(explicit_dir);
    Expect(paths.data_dir == explicit_dir, "explicit data_dir wins");
    Expect(paths.permissions_file == Join(explicit_dir, "permissions.json"),
           "permissions file derives from data_dir");
    Expect(paths.bash_policy_file == Join(explicit_dir, "bash_policy.rules"),
           "bash policy file derives from data_dir");
    Expect(paths.config_file == Join(explicit_dir, "conf/conf.ini"),
           "config file derives from data_dir");
    Expect(paths.runtime_dir == Join(explicit_dir, "runtime"),
           "runtime dir derives from data_dir");
    Expect(paths.db_file == Join(explicit_dir, "aiagent_tasks.db"),
           "db file derives from data_dir");
}

void TestEnvFallbacks() {
    ClearEnv();
    {
        const std::string de_home = test_support::TempPath("aiagent_paths_de_home");
        test_support::SetEnv("DE_HOME", de_home.c_str());
        test_support::SetEnv("XDG_DATA_HOME",
                             test_support::TempPath("aiagent_paths_xdg").c_str());
        const auto paths = aiagent::ResolveAppPaths("");
        Expect(paths.data_dir == de_home, "DE_HOME beats XDG");
    }
    ClearEnv();
    {
        const std::string xdg = test_support::TempPath("aiagent_paths_xdg_only");
        test_support::SetEnv("XDG_DATA_HOME", xdg.c_str());
        const auto paths = aiagent::ResolveAppPaths("");
        Expect(paths.data_dir == Join(xdg, "de"), "XDG_DATA_HOME -> /de");
    }
    ClearEnv();
    {
#ifdef _WIN32
        const std::string profile = test_support::TempPath("aiagent_paths_profile");
        test_support::SetEnv("USERPROFILE", profile.c_str());
        const auto paths = aiagent::ResolveAppPaths("");
        Expect(paths.data_dir == Join(profile, ".de"), "USERPROFILE -> ~/.de");
#else
        test_support::SetEnv("HOME", "/home/tester");
        const auto paths = aiagent::ResolveAppPaths("");
        Expect(paths.data_dir == "/home/tester/.de", "HOME -> ~/.de");
#endif
    }
    ClearEnv();
    {
        const auto paths = aiagent::ResolveAppPaths("");
        Expect(paths.data_dir.empty(), "no env -> empty data_dir");
        Expect(paths.permissions_file.empty(), "empty data_dir -> empty file");
        Expect(paths.db_file.empty(), "empty data_dir -> empty db file");
    }
#ifndef _WIN32
    // Windows 的 HOME 不是数据目录来源；这里单独验证 LOCALAPPDATA 兜底。
#else
    ClearEnv();
    {
        const std::string local = test_support::TempPath("aiagent_paths_local");
        test_support::SetEnv("LOCALAPPDATA", local.c_str());
        const auto paths = aiagent::ResolveAppPaths("");
        Expect(paths.data_dir == Join(local, "de"), "LOCALAPPDATA fallback");
    }
#endif
}

void TestHomeExpansion() {
    ClearEnv();
#ifdef _WIN32
    const std::string home = test_support::TempPath("aiagent_paths_tilde_home");
    test_support::SetEnv("USERPROFILE", home.c_str());
#else
    const std::string home = "/home/tester";
    test_support::SetEnv("HOME", home.c_str());
#endif
    const auto paths = aiagent::ResolveAppPaths("~/.de-alt");
    Expect(paths.data_dir == home + "/.de-alt", "leading ~ is expanded");
}

void TestShareDir() {
    ClearEnv();
    const std::string explicit_share =
            test_support::TempPath("aiagent_paths_share_explicit");
    Expect(aiagent::ResolveShareDir(explicit_share) == explicit_share,
           "explicit share dir wins");
    const std::string env_share = test_support::TempPath("aiagent_paths_share_env");
    test_support::SetEnv("AIAGENT_SHARE_DIR", env_share.c_str());
    Expect(aiagent::ResolveShareDir("") == env_share, "env share dir");
    test_support::UnsetEnv("AIAGENT_SHARE_DIR");
    const std::string exe =
            test_support::TempPath("aiagent_paths_install/bin/aiagent_app");
    const std::string exe_dir = aiagent::platform::ToUtf8(
            aiagent::platform::FromUtf8(exe).parent_path());
    const std::string expected =
            aiagent::platform::ToUtf8((aiagent::platform::FromUtf8(exe_dir) / ".." /
                                       "share" / "de")
                                              .lexically_normal());
    Expect(aiagent::ResolveShareDir("", exe) == expected,
           "exe-relative share dir");
}

void TestConfigFileResolution() {
    ClearEnv();
    namespace fs = std::filesystem;
    const fs::path root = aiagent::platform::FromUtf8(
            test_support::TempPathWithPid("aiagent_paths_"));
    fs::remove_all(root);
    const fs::path cwd = root / "cwd";
    const fs::path data = root / "data";
    const fs::path share = root / "share";

    aiagent::AppPaths paths;
    paths.data_dir = aiagent::platform::ToUtf8(data);
    paths.config_file =
            aiagent::platform::ToUtf8(data / "conf" / "conf.ini");
    paths.runtime_dir = aiagent::platform::ToUtf8(data / "runtime");
    paths.db_file = aiagent::platform::ToUtf8(data / "aiagent_tasks.db");

    const std::string explicit_config =
            aiagent::platform::ToUtf8(root / "explicit" / "conf.ini");

    // 显式路径即使不存在也原样返回。
    Expect(aiagent::ResolveConfigFile(explicit_config, paths, share.string(),
                                      cwd.string()) == explicit_config,
           "explicit config path wins");

    // cwd 下的 conf.ini 优先于 data_dir。
    WriteFile(aiagent::platform::ToUtf8(cwd / "conf" / "conf.ini"));
    Expect(aiagent::ResolveConfigFile("", paths, share.string(), cwd.string()) ==
               aiagent::platform::ToUtf8(cwd / "conf" / "conf.ini"),
           "cwd config preferred");

    // 去掉 cwd 配置后回退到 data_dir。
    fs::remove(cwd / "conf" / "conf.ini");
    WriteFile(paths.config_file);
    Expect(aiagent::ResolveConfigFile("", paths, share.string(), cwd.string()) ==
               paths.config_file,
           "data_dir config fallback");

    // 都没有时返回空。
    fs::remove(aiagent::platform::FromUtf8(paths.config_file));
    Expect(aiagent::ResolveConfigFile("", paths, share.string(), cwd.string())
               .empty(),
           "no config -> empty");

    fs::remove_all(root);
}

}  // namespace

int main() {
    TestExplicitWins();
    TestEnvFallbacks();
    TestHomeExpansion();
    TestShareDir();
    TestConfigFileResolution();

    if (g_failures != 0) {
        std::cerr << g_failures << " app paths test(s) failed\n";
        return 1;
    }
    std::cout << "app paths test passed\n";
    return 0;
}
