#include "crypto.hpp"

#include "aes.hpp"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace myrtm {
namespace {

uint64_t load64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    return v;
}

void store64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = uint8_t(v);
        v >>= 8;
    }
}

void gmul(uint64_t x[2], const uint64_t h[2]) {
    uint64_t zh = 0, zl = 0;
    uint64_t vh = h[0], vl = h[1];
    for (int i = 0; i < 128; ++i) {
        uint64_t bit = (i < 64) ? (x[0] >> (63 - i)) : (x[1] >> (127 - i));
        if (bit & 1) {
            zh ^= vh;
            zl ^= vl;
        }
        uint64_t lsb = vl & 1;
        vl = (vl >> 1) | (vh << 63);
        vh >>= 1;
        if (lsb)
            vh ^= 0xe100000000000000ULL;
    }
    x[0] = zh;
    x[1] = zl;
}

struct GHash {
    uint64_t h[2];
    uint64_t y[2];

    void init(const uint8_t hk[16]) {
        h[0] = load64(hk);
        h[1] = load64(hk + 8);
        y[0] = 0;
        y[1] = 0;
    }

    void update(const uint8_t* p, size_t n) {
        uint8_t pad[16];
        while (n) {
            size_t take = n < 16 ? n : 16;
            const uint8_t* src = p;
            if (take < 16) {
                std::memset(pad, 0, 16);
                std::memcpy(pad, p, take);
                src = pad;
            }
            y[0] ^= load64(src);
            y[1] ^= load64(src + 8);
            gmul(y, h);
            p += take;
            n -= take;
        }
    }

    void finish(uint8_t out[16]) {
        store64(out, y[0]);
        store64(out + 8, y[1]);
    }
};

void ctrCrypt(const Aes128& aes, const uint8_t iv[12], uint32_t start, const uint8_t* in,
              size_t len, uint8_t* out) {
    uint8_t block[16], ks[16];
    std::memcpy(block, iv, 12);
    uint32_t c = start;
    size_t off = 0;
    while (off < len) {
        block[12] = uint8_t(c >> 24);
        block[13] = uint8_t(c >> 16);
        block[14] = uint8_t(c >> 8);
        block[15] = uint8_t(c);
        aes.encrypt(block, ks);
        size_t n = (len - off) < 16 ? (len - off) : 16;
        for (size_t i = 0; i < n; ++i)
            out[off + i] = uint8_t(in[off + i] ^ ks[i]);
        off += n;
        ++c;
    }
}

void authTag(const Aes128& aes, const uint8_t iv[12], const uint8_t hk[16], const uint8_t* aad,
             size_t aadLen, const uint8_t* cipher, size_t len, uint8_t tag[16]) {
    GHash g;
    g.init(hk);
    if (aadLen)
        g.update(aad, aadLen);
    if (len)
        g.update(cipher, len);
    uint8_t lens[16];
    store64(lens, static_cast<uint64_t>(aadLen) * 8);
    store64(lens + 8, static_cast<uint64_t>(len) * 8);
    g.update(lens, 16);
    uint8_t s[16];
    g.finish(s);
    ctrCrypt(aes, iv, 1, s, 16, tag);
}

void kdf(const uint8_t key[16], const char* label, const uint8_t* seed, size_t seedLen,
         uint8_t* out, size_t outLen) {
    size_t labelLen = std::strlen(label);
    std::vector<uint8_t> msg;
    msg.reserve(labelLen + seedLen + 4);
    msg.push_back(0);
    msg.insert(msg.end(), reinterpret_cast<const uint8_t*>(label),
               reinterpret_cast<const uint8_t*>(label) + labelLen);
    msg.push_back(0);
    if (seedLen)
        msg.insert(msg.end(), seed, seed + seedLen);
    uint16_t bits = static_cast<uint16_t>(outLen * 8);
    msg.push_back(static_cast<uint8_t>(bits >> 8));
    msg.push_back(static_cast<uint8_t>(bits));

    uint8_t block[16];
    size_t done = 0;
    uint8_t counter = 1;
    while (done < outLen) {
        msg[0] = counter++;
        aesCmac(key, msg.data(), msg.size(), block);
        size_t n = (outLen - done) < 16 ? (outLen - done) : 16;
        std::memcpy(out + done, block, n);
        done += n;
    }
}

void rootKey(const std::string& psk, uint8_t out[16]) {
    static const char kSalt[] = "myrtm root v1";
    aesCmacPrf(reinterpret_cast<const uint8_t*>(psk.data()), psk.size(),
               reinterpret_cast<const uint8_t*>(kSalt), sizeof(kSalt) - 1, out);
}

}

void randomBytes(void* out, size_t n) {
#ifdef _WIN32
    NTSTATUS st = BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(n),
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(st))
        std::abort();
#else
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        std::abort();
    uint8_t* p = static_cast<uint8_t*>(out);
    while (n) {
        ssize_t got = ::read(fd, p, n);
        if (got <= 0) {
            ::close(fd);
            std::abort();
        }
        p += got;
        n -= static_cast<size_t>(got);
    }
    ::close(fd);
#endif
}

uint32_t randomU32() {
    uint32_t v = 0;
    randomBytes(&v, sizeof(v));
    return v;
}

uint64_t randomU64() {
    uint64_t v = 0;
    randomBytes(&v, sizeof(v));
    return v;
}

AeadKey::AeadKey() {
    uint8_t key[16] = {0};
    uint8_t salt[4] = {0};
    set(key, salt);
}

void AeadKey::set(const uint8_t key[16], const uint8_t salt[4]) {
    aes_.setKey(key);
    std::memcpy(salt_, salt, 4);
    uint8_t zero[16] = {0};
    aes_.encrypt(zero, hk_);
}

void AeadKey::makeIv(uint64_t counter, uint8_t out[12]) const {
    std::memcpy(out, salt_, 4);
    store64(out + 4, counter);
}

void AeadKey::seal(uint64_t counter, std::vector<uint8_t>& packet, size_t aadLen,
                   const uint8_t* plain, size_t plainLen) const {
    uint8_t iv[12];
    makeIv(counter, iv);

    size_t base = packet.size();
    packet.resize(base + plainLen + kTagSize);
    uint8_t* cipher = packet.data() + base;
    if (plainLen)
        ctrCrypt(aes_, iv, 2, plain, plainLen, cipher);
    authTag(aes_, iv, hk_, packet.data(), aadLen, cipher, plainLen, cipher + plainLen);
}

bool AeadKey::open(uint64_t counter, const uint8_t* aad, size_t aadLen, const uint8_t* cipher,
                   size_t cipherLen, std::vector<uint8_t>& out) const {
    if (cipherLen < kTagSize)
        return false;

    uint8_t iv[12];
    makeIv(counter, iv);

    size_t len = cipherLen - kTagSize;
    uint8_t tag[16];
    authTag(aes_, iv, hk_, aad, aadLen, cipher, len, tag);

    unsigned diff = 0;
    for (size_t i = 0; i < kTagSize; ++i)
        diff |= tag[i] ^ cipher[len + i];
    if (diff)
        return false;

    out.resize(len);
    if (len)
        ctrCrypt(aes_, iv, 2, cipher, len, out.data());
    return true;
}

AeadKey handshakeKey(const std::string& psk) {
    uint8_t root[16];
    rootKey(psk, root);
    uint8_t material[20];
    kdf(root, "handshake", nullptr, 0, material, sizeof(material));

    AeadKey k;
    k.set(material, material + 16);
    return k;
}

void deriveSessionKeys(const std::string& psk, const uint8_t clientNonce[kNonceSize],
                       const uint8_t serverNonce[kNonceSize], bool server, SessionKeys& out) {
    uint8_t root[16];
    rootKey(psk, root);

    uint8_t seed[kNonceSize * 2];
    std::memcpy(seed, clientNonce, kNonceSize);
    std::memcpy(seed + kNonceSize, serverNonce, kNonceSize);

    uint8_t material[40];
    kdf(root, "session", seed, sizeof(seed), material, sizeof(material));

    const uint8_t* up = material;
    const uint8_t* down = material + 20;
    if (server) {
        out.tx.set(down, down + 16);
        out.rx.set(up, up + 16);
    } else {
        out.tx.set(up, up + 16);
        out.rx.set(down, down + 16);
    }
}

bool ReplayWindow::accept(uint64_t counter) {
    if (counter == 0)
        return false;

    if (counter > top_) {
        uint64_t shift = counter - top_;
        seen_ = shift >= 64 ? 0 : (seen_ << shift);
        seen_ |= 1;
        top_ = counter;
        return true;
    }

    uint64_t back = top_ - counter;
    if (back >= 64)
        return false;
    uint64_t bit = 1ULL << back;
    if (seen_ & bit)
        return false;
    seen_ |= bit;
    return true;
}

}
