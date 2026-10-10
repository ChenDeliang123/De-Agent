// TrustStore 的单元测试：就近继承、读写、持久化。

#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

#include "aiagent/permissions/trust_store.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

using aiagent::permissions::TrustStore;

void TestMemoryStore() {
    TrustStore store;
    Expect(!store.IsTrusted("/a/b/c"), "unset path is untrusted");
    Expect(!store.Resolve("/a/b/c").has_value(), "unset path resolves to nullopt");

    store.Set("/a/b", true);
    Expect(store.IsTrusted("/a/b"), "exact path trusted");
    Expect(store.IsTrusted("/a/b/c"), "child inherits trust");
    Expect(!store.IsTrusted("/a"), "parent is not trusted by child's decision");

    store.Set("/a/b", false);
    Expect(!store.IsTrusted("/a/b/c"), "explicit false overrides inherited true");

    Expect(store.Remove("/a/b"), "remove existing returns true");
    Expect(!store.IsTrusted("/a/b/c"), "after removal, no trust");
    Expect(!store.Remove("/a/b"), "remove missing returns false");
}

void TestPersistence() {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("aiagent_trust_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = dir / "trust.json";

    {
        TrustStore store(file.string());
        store.Set("/tmp/proj", true);
    }
    {
        TrustStore reloaded(file.string());
        Expect(reloaded.IsTrusted("/tmp/proj"), "trust survives reload");
        Expect(reloaded.IsTrusted("/tmp/proj/sub"), "inheritance survives reload");
    }

    std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
    TestMemoryStore();
    TestPersistence();

    if (g_failures != 0) {
        std::cerr << "trust store test failed with " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "trust store test passed\n";
    return 0;
}
