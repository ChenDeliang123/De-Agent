#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "aiagent/util/sha256.hpp"

#include "test_support.hpp"

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
        // NIST 标准向量。
        Expect(aiagent::util::Sha256Hex("") ==
                       "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
               "empty string vector");
        Expect(aiagent::util::Sha256Hex("abc") ==
                       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               "abc vector");
        Expect(aiagent::util::Sha256Hex(
                       "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
               "long vector");

        // 分段更新与一次性更新必须一致（实现内部有缓冲边界）。
        std::string chunked;
        for (int i = 0; i < 1000; ++i) {
            chunked += "0123456789";
        }
        Expect(aiagent::util::Sha256Hex(chunked) ==
                       aiagent::util::Sha256Hex(chunked.data(), chunked.size()),
               "span and pointer overloads should match");

        // 文件哈希。
        const std::string path =
                test_support::TempPathWithPid("aiagent_sha256_", ".txt");
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "abc";
        }
        std::string error;
        Expect(aiagent::util::Sha256HexFile(path, &error) ==
                       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               "file hash should match content hash: " + error);
        Expect(aiagent::util::Sha256HexFile(path + ".missing", &error).empty() &&
                       !error.empty(),
               "missing file should report an error");
        std::filesystem::remove(path);
    } catch (const std::exception& e) {
        std::cerr << "sha256 test failed: " << e.what() << '\n';
        return 1;
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " sha256 test(s) failed\n";
        return 1;
    }
    return 0;
}
