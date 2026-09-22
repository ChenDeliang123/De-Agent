#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "aiagent/agent/session_meta_store.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

}  // namespace

int main() {
    try {
        // 纯内存模式：不落盘。
        {
            aiagent::agent::SessionMetaStore store;
            Expect(!store.GetTitle("s1").has_value(), "empty store should have no title");
            store.SetTitle("s1", "hello");
            Expect(store.GetTitle("s1").value_or("") == "hello", "title should round-trip");
            store.SetCwd("s1", "/tmp/project");
            Expect(store.GetCwd("s1").value_or("") == "/tmp/project",
                   "cwd should round-trip");
            Expect(store.Size() == 1, "size should count titles");
            store.Remove("s1");
            Expect(!store.GetTitle("s1").has_value(), "removed title should be gone");
            Expect(!store.GetCwd("s1").has_value(), "removed cwd should be gone");
            Expect(store.Size() == 0, "size should drop after remove");
        }

        // 文件持久化：写入后新实例应能读回。
        const auto dir = std::filesystem::temp_directory_path() /
                         ("aiagent_session_meta_" +
                          std::to_string(std::chrono::steady_clock::now()
                                                 .time_since_epoch()
                                                 .count()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        const auto path = (dir / "sessions.json").string();
        {
            aiagent::agent::SessionMetaStore store(path);
            store.SetTitle("s1", "first");
            store.SetTitle("s2", "second");
            store.SetCwd("s1", "/tmp/project-a");
            store.SetCwd("s2", "/tmp/project-b");
        }
        {
            aiagent::agent::SessionMetaStore reloaded(path);
            Expect(reloaded.GetTitle("s1").value_or("") == "first",
                   "title should persist across instances");
            Expect(reloaded.GetTitle("s2").value_or("") == "second",
                   "second title should persist");
            Expect(reloaded.GetCwd("s1").value_or("") == "/tmp/project-a",
                   "cwd should persist across instances");
            Expect(reloaded.GetCwd("s2").value_or("") == "/tmp/project-b",
                   "second cwd should persist across instances");
            Expect(reloaded.Size() == 2, "persisted size should match");
            reloaded.Remove("s1");
        }
        {
            aiagent::agent::SessionMetaStore reloaded(path);
            Expect(!reloaded.GetTitle("s1").has_value(),
                   "removed title should stay removed after reload");
            Expect(!reloaded.GetCwd("s1").has_value(),
                   "removed cwd should stay removed after reload");
            Expect(reloaded.GetTitle("s2").value_or("") == "second",
                   "other titles should survive removal");
            Expect(reloaded.GetCwd("s2").value_or("") == "/tmp/project-b",
                   "other cwd should survive removal");
            reloaded.Clear();
            Expect(reloaded.Size() == 0, "clear should empty the store");
        }
        {
            aiagent::agent::SessionMetaStore reloaded(path);
            Expect(reloaded.Size() == 0, "cleared store should stay empty after reload");
            Expect(!reloaded.GetCwd("s2").has_value(),
                   "cleared cwd should stay empty after reload");
        }

        // 损坏文件不应抛异常，按空处理。
        {
            std::filesystem::create_directories(dir);
            const auto broken_path = (dir / "broken.json").string();
            {
                std::ofstream out(broken_path, std::ios::trunc);
                out << "{ not valid json";
            }
            aiagent::agent::SessionMetaStore broken(broken_path);
            Expect(broken.Size() == 0, "corrupt metadata should fall back to empty");
            broken.SetTitle("s1", "recovered");
            Expect(broken.GetTitle("s1").value_or("") == "recovered",
                   "store should work after recovering from corrupt file");
        }

        std::filesystem::remove_all(dir);
    } catch (const std::exception& e) {
        std::cerr << "session meta store test failed: " << e.what() << '\n';
        return 1;
    }

    if (g_failures != 0) {
        std::cerr << "session meta store test failed with " << g_failures
                  << " failure(s)\n";
        return 1;
    }
    std::cout << "session meta store test passed\n";
    return 0;
}
