// AppPaths 用户级数据目录解析的单元测试。

#include <cstdlib>
#include <iostream>
#include <string>

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
}

void TestExplicitWins() {
    ClearEnv();
    setenv("DE_HOME", "/tmp/de_home", 1);
    const auto paths = aiagent::ResolveAppPaths("/tmp/explicit");
    Expect(paths.data_dir == "/tmp/explicit", "explicit data_dir wins");
    Expect(paths.permissions_file == "/tmp/explicit/permissions.json",
           "permissions file derives from data_dir");
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
    }
}

void TestHomeExpansion() {
    ClearEnv();
    setenv("HOME", "/home/tester", 1);
    const auto paths = aiagent::ResolveAppPaths("~/.de-alt");
    Expect(paths.data_dir == "/home/tester/.de-alt", "leading ~ is expanded");
}

}  // namespace

int main() {
    TestExplicitWins();
    TestEnvFallbacks();
    TestHomeExpansion();

    if (g_failures != 0) {
        std::cerr << "app paths test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "app paths test passed\n";
    return 0;
}
