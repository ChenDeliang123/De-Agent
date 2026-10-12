// AppPaths 用户级数据目录与安装路径解析的单元测试。

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "aiagent/app_paths.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

void ClearEnv() {
    unsetenv("DE_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("HOME");
    unsetenv("AIAGENT_SHARE_DIR");
}

void WriteFile(const std::string& path) {
    std::filesystem::create_directories(
            std::filesystem::path(path).parent_path());
    std::ofstream out(path);
    out << "[deepseek]\n";
}

void TestExplicitWins() {
    ClearEnv();
    setenv("DE_HOME", "/tmp/de_home", 1);
    const auto paths = aiagent::ResolveAppPaths("/tmp/explicit");
    Expect(paths.data_dir == "/tmp/explicit", "explicit data_dir wins");
    Expect(paths.permissions_file == "/tmp/explicit/permissions.json",
           "permissions file derives from data_dir");
    Expect(paths.bash_policy_file == "/tmp/explicit/bash_policy.rules",
           "bash policy file derives from data_dir");
    Expect(paths.config_file == "/tmp/explicit/conf/conf.ini",
           "config file derives from data_dir");
    Expect(paths.runtime_dir == "/tmp/explicit/runtime",
           "runtime dir derives from data_dir");
    Expect(paths.db_file == "/tmp/explicit/aiagent_tasks.db",
           "db file derives from data_dir");
}

void TestEnvFallbacks() {
    ClearEnv();
    {
        setenv("DE_HOME", "/tmp/de_home", 1);
        setenv("XDG_DATA_HOME", "/tmp/xdg", 1);
        const auto paths = aiagent::ResolveAppPaths("");
        Expect(paths.data_dir == "/tmp/de_home", "DE_HOME beats XDG");
    }
    ClearEnv();
    {
        setenv("XDG_DATA_HOME", "/tmp/xdg", 1);
        const auto paths = aiagent::ResolveAppPaths("");
        Expect(paths.data_dir == "/tmp/xdg/de", "XDG_DATA_HOME -> /de");
    }
    ClearEnv();
    {
        setenv("HOME", "/home/tester", 1);
        const auto paths = aiagent::ResolveAppPaths("");
        Expect(paths.data_dir == "/home/tester/.de", "HOME -> ~/.de");
    }
    ClearEnv();
    {
        const auto paths = aiagent::ResolveAppPaths("");
        Expect(paths.data_dir.empty(), "no env -> empty data_dir");
        Expect(paths.permissions_file.empty(), "empty data_dir -> empty file");
        Expect(paths.db_file.empty(), "empty data_dir -> empty db file");
    }
}

void TestHomeExpansion() {
    ClearEnv();
    setenv("HOME", "/home/tester", 1);
    const auto paths = aiagent::ResolveAppPaths("~/.de-alt");
    Expect(paths.data_dir == "/home/tester/.de-alt", "leading ~ is expanded");
}

void TestShareDir() {
    ClearEnv();
    Expect(aiagent::ResolveShareDir("/opt/de/share") == "/opt/de/share",
           "explicit share dir wins");
    setenv("AIAGENT_SHARE_DIR", "/env/share", 1);
    Expect(aiagent::ResolveShareDir("") == "/env/share", "env share dir");
    unsetenv("AIAGENT_SHARE_DIR");
    Expect(aiagent::ResolveShareDir("", "/opt/de/bin/aiagent_app") ==
               "/opt/de/share/de",
           "exe-relative share dir");
}

void TestConfigFileResolution() {
    ClearEnv();
    namespace fs = std::filesystem;
    const fs::path root =
            fs::temp_directory_path() /
            ("aiagent_paths_" + std::to_string(static_cast<long>(::getpid())));
    fs::remove_all(root);
    const fs::path cwd = root / "cwd";
    const fs::path data = root / "data";
    const fs::path share = root / "share";

    aiagent::AppPaths paths;
    paths.data_dir = data.string();
    paths.config_file = (data / "conf" / "conf.ini").string();
    paths.runtime_dir = (data / "runtime").string();
    paths.db_file = (data / "aiagent_tasks.db").string();

    // 显式路径即使不存在也原样返回。
    Expect(aiagent::ResolveConfigFile("/explicit/conf.ini", paths, share.string(),
                                      cwd.string()) == "/explicit/conf.ini",
           "explicit config path wins");

    // cwd 下的 conf.ini 优先于 data_dir。
    WriteFile((cwd / "conf" / "conf.ini").string());
    Expect(aiagent::ResolveConfigFile("", paths, share.string(), cwd.string()) ==
               (cwd / "conf" / "conf.ini").string(),
           "cwd config preferred");

    // 去掉 cwd 配置后回退到 data_dir。
    fs::remove(cwd / "conf" / "conf.ini");
    WriteFile(paths.config_file);
    Expect(aiagent::ResolveConfigFile("", paths, share.string(), cwd.string()) ==
               paths.config_file,
           "data_dir config fallback");

    // 都没有时返回空。
    fs::remove(paths.config_file);
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
        std::cerr << "app paths test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "app paths test passed\n";
    return 0;
}
