#include "aes.hpp"

#include <cstring>
#include <mutex>

namespace myrtm {
namespace {

uint8_t g_sbox[256];
std::once_flag g_sbox_once;

unsigned rotl8(unsigned x, int s) {
    return ((x << s) | (x >> (8 - s))) & 0xff;
}

unsigned xtime(unsigned a) {
    return ((a << 1) ^ (a & 0x80 ? 0x1b : 0)) & 0xff;
}

void buildSbox() {
    unsigned p = 1, q = 1;
    do {
        p ^= (p << 1) ^ (p & 0x80 ? 0x1b : 0);
        p &= 0xff;

        q ^= q << 1;
        q ^= q << 2;
        q ^= q << 4;
        q &= 0xff;
        if (q & 0x80)
            q ^= 0x09;

        unsigned v = q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4);
        g_sbox[p] = uint8_t(v ^ 0x63);
    } while (p != 1);
    g_sbox[0] = 0x63;
}

const uint8_t* sbox() {
    std::call_once(g_sbox_once, buildSbox);
    return g_sbox;
}

void shiftLeft(const uint8_t in[16], uint8_t out[16]) {
    unsigned carry = 0;
    for (int i = 15; i >= 0; --i) {
        unsigned next = in[i] >> 7;
        out[i] = uint8_t((in[i] << 1) | carry);
        carry = next;
    }
}

}

void Aes128::setKey(const uint8_t key[16]) {
    const uint8_t* s = sbox();
    sbox_ = s;
    std::memcpy(rk_, key, 16);
    unsigned rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = {rk_[i - 4], rk_[i - 3], rk_[i - 2], rk_[i - 1]};
        if ((i & 15) == 0) {
            uint8_t first = t[0];
            t[0] = uint8_t(s[t[1]] ^ rcon);
            t[1] = s[t[2]];
            t[2] = s[t[3]];
            t[3] = s[first];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; ++j)
            rk_[i + j] = uint8_t(rk_[i - 16 + j] ^ t[j]);
    }
}

void Aes128::encrypt(const uint8_t in[16], uint8_t out[16]) const {
    const uint8_t* s = sbox_;
    uint8_t st[16];
    for (int i = 0; i < 16; ++i)
        st[i] = uint8_t(in[i] ^ rk_[i]);

    for (int r = 1; r <= 10; ++r) {
        for (int i = 0; i < 16; ++i)
            st[i] = s[st[i]];

        uint8_t t = st[1];
        st[1] = st[5];
        st[5] = st[9];
        st[9] = st[13];
        st[13] = t;
        t = st[2];
        st[2] = st[10];
        st[10] = t;
        t = st[6];
        st[6] = st[14];
        st[14] = t;
        t = st[15];
        st[15] = st[11];
        st[11] = st[7];
        st[7] = st[3];
        st[3] = t;

        if (r != 10) {
            for (int c = 0; c < 16; c += 4) {
                unsigned a0 = st[c], a1 = st[c + 1], a2 = st[c + 2], a3 = st[c + 3];
                unsigned x = a0 ^ a1 ^ a2 ^ a3;
                st[c] = uint8_t(a0 ^ x ^ xtime(a0 ^ a1));
                st[c + 1] = uint8_t(a1 ^ x ^ xtime(a1 ^ a2));
                st[c + 2] = uint8_t(a2 ^ x ^ xtime(a2 ^ a3));
                st[c + 3] = uint8_t(a3 ^ x ^ xtime(a3 ^ a0));
            }
        }

        const uint8_t* k = rk_ + 16 * r;
        for (int i = 0; i < 16; ++i)
            st[i] ^= k[i];
    }
    std::memcpy(out, st, 16);
}

void aesCmac(const uint8_t key[16], const uint8_t* msg, size_t len, uint8_t mac[16]) {
    Aes128 aes;
    aes.setKey(key);

    uint8_t zero[16] = {0};
    uint8_t l[16];
    aes.encrypt(zero, l);

    uint8_t k1[16], k2[16];
    shiftLeft(l, k1);
    if (l[0] & 0x80)
        k1[15] ^= 0x87;
    shiftLeft(k1, k2);
    if (k1[0] & 0x80)
        k2[15] ^= 0x87;

    size_t blocks = (len + 15) / 16;
    bool aligned = len > 0 && (len % 16) == 0;
    if (blocks == 0)
        blocks = 1;

    uint8_t x[16] = {0};
    for (size_t i = 0; i + 1 < blocks; ++i) {
        for (int j = 0; j < 16; ++j)
            x[j] ^= msg[i * 16 + j];
        aes.encrypt(x, x);
    }

    uint8_t tail[16];
    size_t off = (blocks - 1) * 16;
    if (aligned) {
        std::memcpy(tail, msg + off, 16);
        for (int j = 0; j < 16; ++j)
            tail[j] ^= k1[j];
    } else {
        size_t rem = len - off;
        std::memset(tail, 0, 16);
        if (rem)
            std::memcpy(tail, msg + off, rem);
        tail[rem] = 0x80;
        for (int j = 0; j < 16; ++j)
            tail[j] ^= k2[j];
    }

    for (int j = 0; j < 16; ++j)
        x[j] ^= tail[j];
    aes.encrypt(x, mac);
}

void aesCmacPrf(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t len, uint8_t out[16]) {
    uint8_t k[16];
    if (keyLen == 16) {
        std::memcpy(k, key, 16);
    } else {
        uint8_t zero[16] = {0};
        aesCmac(zero, key, keyLen, k);
    }
    aesCmac(k, msg, len, out);
}

}
