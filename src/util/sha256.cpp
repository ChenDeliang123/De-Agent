#include "aiagent/util/sha256.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace aiagent::util {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
        0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
        0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
        0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

inline std::uint32_t RotateRight(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

class Sha256 {
public:
    Sha256()
            : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                     0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

    void Update(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        total_bytes_ += size;
        while (size > 0) {
            const std::size_t take = std::min(size, sizeof(buffer_) - buffer_size_);
            std::memcpy(buffer_ + buffer_size_, bytes, take);
            buffer_size_ += take;
            bytes += take;
            size -= take;
            if (buffer_size_ == sizeof(buffer_)) {
                Transform(buffer_);
                buffer_size_ = 0;
            }
        }
    }

    std::string FinalHex() {
        const std::uint64_t bit_length = total_bytes_ * 8U;
        const unsigned char padding = 0x80U;
        Update(&padding, 1);
        const unsigned char zero = 0;
        while (buffer_size_ != 56) {
            Update(&zero, 1);
        }
        unsigned char length_bytes[8];
        for (int i = 0; i < 8; ++i) {
            length_bytes[7 - i] = static_cast<unsigned char>((bit_length >> (i * 8)) & 0xffU);
        }
        Update(length_bytes, sizeof(length_bytes));

        static constexpr char kHex[] = "0123456789abcdef";
        std::string output;
        output.reserve(64);
        for (const std::uint32_t word : state_) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                output.push_back(kHex[(word >> shift) & 0x0fU]);
            }
        }
        return output;
    }

private:
    void Transform(const unsigned char block[64]) {
        std::uint32_t words[64];
        for (int i = 0; i < 16; ++i) {
            words[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                       (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                       (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                       static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = RotateRight(words[i - 15], 7) ^
                                     RotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const std::uint32_t s1 = RotateRight(words[i - 2], 17) ^
                                     RotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + words[i];
            const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_;
    unsigned char buffer_[64] = {};
    std::size_t buffer_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

}  // namespace

std::string Sha256Hex(const void* data, std::size_t size) {
    Sha256 hash;
    if (size > 0) {
        hash.Update(data, size);
    }
    return hash.FinalHex();
}

std::string Sha256Hex(std::string_view data) {
    return Sha256Hex(data.data(), data.size());
}

std::string Sha256HexFile(const std::filesystem::path& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open file for hashing";
        }
        return {};
    }
    Sha256 hash;
    std::vector<char> buffer(64 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = input.gcount();
        if (read > 0) {
            hash.Update(buffer.data(), static_cast<std::size_t>(read));
        }
    }
    if (input.bad()) {
        if (error != nullptr) {
            *error = "read error while hashing";
        }
        return {};
    }
    return hash.FinalHex();
}

}  // namespace aiagent::util
