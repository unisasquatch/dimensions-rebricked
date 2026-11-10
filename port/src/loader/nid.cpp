#include "loader/nid.h"

#include <array>
#include <cstdint>
#include <vector>

namespace loader {
namespace {

constexpr std::array<uint8_t, 16> kNidSuffix = {
    0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
    0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};

uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

std::array<uint8_t, 20> Sha1(const std::vector<uint8_t>& input) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<uint8_t> msg = input;
    const uint64_t bit_len = uint64_t(input.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(uint8_t(bit_len >> (i * 8)));

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = &msg[chunk + i * 4];
            w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            const uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    std::array<uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = uint8_t(h[i] >> (24 - j * 8));
    return out;
}

}  // namespace

std::string ComputeNid(std::string_view name) {
    std::vector<uint8_t> input(name.begin(), name.end());
    input.insert(input.end(), kNidSuffix.begin(), kNidSuffix.end());
    const auto digest = Sha1(input);

    // The NID is the first 8 digest bytes read as a little-endian u64,
    // base64-encoded big-endian into 11 characters.
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | digest[i];

    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string nid(11, '\0');
    nid[10] = kAlphabet[(v & 0xF) << 2];
    v >>= 4;
    for (int i = 9; i >= 0; --i) {
        nid[i] = kAlphabet[v & 0x3F];
        v >>= 6;
    }
    return nid;
}

}  // namespace loader
