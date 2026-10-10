// PermissionStore 的单元测试：内存与文件两种模式、就近继承、full 不继承、
// 原子持久化与损坏文件容错。

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "aiagent/permissions/permission_store.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

std::filesystem::path TempDir() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("aiagent_perm_store_" + std::to_string(::getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

using aiagent::permissions::PermissionMode;
using aiagent::permissions::PermissionStore;

void TestMemoryStore() {
    PermissionStore store;  // path 为空 -> 纯内存
    const std::string proj = "/tmp/aiagent_perm_mem/proj";
    const std::string sub = proj + "/sub";

    Expect(!store.Resolve(proj).has_value(), "unset path resolves to nullopt");
    Expect(store.Resolve("").has_value() == false, "empty path resolves to nullopt");

    store.Set(proj, PermissionMode::kWorkspace);
    const auto exact = store.Resolve(proj);
    Expect(exact.has_value() && exact->mode == PermissionMode::kWorkspace,
           "exact match returns workspace");
    Expect(exact.has_value() && !exact->inherited, "exact match is not inherited");

    const auto inherited = store.Resolve(sub);
    Expect(inherited.has_value() && inherited->mode == PermissionMode::kWorkspace,
           "child inherits workspace");
    Expect(inherited.has_value() && inherited->inherited, "child is inherited");

    // full 不继承：子目录不应命中 full。
    store.Set(proj, PermissionMode::kFull);
    Expect(!store.Resolve(sub).has_value(), "full is not inherited by children");
    const auto full_exact = store.Resolve(proj);
    Expect(full_exact.has_value() && full_exact->mode == PermissionMode::kFull,
           "exact full still resolves");

    // full 祖先被跳过，继续向上找非 full 祖先。
    store.Set("/tmp/aiagent_perm_mem", PermissionMode::kConfirm);
    const auto skipped = store.Resolve(sub);
    Expect(skipped.has_value() && skipped->mode == PermissionMode::kConfirm,
           "full ancestor is skipped, upper confirm is used");
    Expect(skipped.has_value() && skipped->inherited, "upper match is inherited");

    Expect(store.Size() == 2, "size counts explicit entries");
    Expect(store.Remove(proj), "remove existing entry returns true");
    Expect(!store.Remove(proj), "remove missing entry returns false");
    // 删除 proj 后，回退到祖先 /tmp/aiagent_perm_mem 的 confirm。
    const auto after_remove = store.Resolve(proj);
    Expect(after_remove.has_value() && after_remove->mode == PermissionMode::kConfirm &&
                   after_remove->inherited,
           "after removal, proj inherits ancestor confirm");

    store.Clear();
    Expect(store.Size() == 0, "clear removes all entries");
}

void TestFilePersistence() {
    const auto dir = TempDir();
    const auto file = dir / "permissions.json";
    const std::string proj = (dir / "proj").string();
    std::filesystem::create_directories(proj);

    {
        PermissionStore store(file.string());
        store.Set(proj, PermissionMode::kWorkspace);
    }
    Expect(std::filesystem::exists(file), "permissions file is written");

    {
        PermissionStore reloaded(file.string());
        const auto entry = reloaded.Resolve(proj);
        Expect(entry.has_value() && entry->mode == PermissionMode::kWorkspace,
               "mode survives reload");
    }

    // 损坏文件：应容错为空配置，不抛异常。
    {
        std::ofstream output(file, std::ios::trunc);
        output << "{ this is not json";
    }
    {
        PermissionStore corrupted(file.string());
        Expect(!corrupted.Resolve(proj).has_value(),
               "corrupt file is treated as empty");
    }

    std::filesystem::remove_all(dir);
}

void TestCanonicalization() {
    const auto dir = TempDir();
    const auto proj = dir / "proj";
    std::filesystem::create_directories(proj);

    PermissionStore store;
    store.Set(proj.string(), PermissionMode::kConfirm);

    // 末尾斜杠 / ./ 规范化后应命中同一路径。
    Expect(store.Resolve(proj.string() + "/").has_value(),
           "trailing slash canonicalizes to same path");
    Expect(store.Resolve((proj / "." / "sub").string()).has_value(),
           "dot segment canonicalizes and inherits");

    std::filesystem::remove_all(dir);
}

void TestSymlinkCanonicalization() {
    const auto dir = TempDir();
    const auto real = dir / "real";
    std::filesystem::create_directories(real);
    const auto link = dir / "link";
    std::error_code ec;
    std::filesystem::create_directory_symlink(real, link, ec);
    if (ec) {
        std::filesystem::remove_all(dir);
        return;  // 平台不支持符号链接则跳过
    }
    PermissionStore store;
    store.Set(link.string(), PermissionMode::kWorkspace);
    // 通过真实路径应能命中（canonical 后同一路径）。
    Expect(store.Resolve(real.string()).has_value(),
           "symlink canonicalizes to the real path");
    std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
    TestMemoryStore();
    TestFilePersistence();
    TestCanonicalization();
    TestSymlinkCanonicalization();

    if (g_failures != 0) {
        std::cerr << "permission store test failed with " << g_failures
                  << " failure(s)\n";
        return 1;
    }
    std::cout << "permission store test passed\n";
    return 0;
}
