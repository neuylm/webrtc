#include "sha1.hpp"

#include <cstring>

namespace mysignal {
namespace {

uint32_t rol(uint32_t v, int bits) {
    return (v << bits) | (v >> (32 - bits));
}

void crunch(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];

    for (int i = 0; i < 80; ++i) {
        uint32_t f;
        uint32_t k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }

        uint32_t tmp = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = tmp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

}

void sha1(const uint8_t* data, size_t len, uint8_t out[kSha1Size]) {
    uint32_t state[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};

    size_t whole = len / 64;
    for (size_t i = 0; i < whole; ++i)
        crunch(state, data + i * 64);

    uint8_t tail[128];
    size_t rest = len - whole * 64;
    std::memcpy(tail, data + whole * 64, rest);
    tail[rest] = 0x80;

    size_t tailLen = rest + 1 <= 56 ? 64 : 128;
    std::memset(tail + rest + 1, 0, tailLen - rest - 1);

    uint64_t bits = uint64_t(len) * 8;
    for (int i = 0; i < 8; ++i)
        tail[tailLen - 1 - i] = uint8_t((bits >> (i * 8)) & 0xff);

    for (size_t off = 0; off < tailLen; off += 64)
        crunch(state, tail + off);

    for (int i = 0; i < 5; ++i) {
        out[i * 4] = uint8_t(state[i] >> 24);
        out[i * 4 + 1] = uint8_t(state[i] >> 16);
        out[i * 4 + 2] = uint8_t(state[i] >> 8);
        out[i * 4 + 3] = uint8_t(state[i]);
    }
}

}
