#ifndef MYRTM_CRYPTO_H
#define MYRTM_CRYPTO_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "aes.hpp"

namespace myrtm {

constexpr size_t kTagSize = 16;
constexpr size_t kNonceSize = 16;

class AeadKey {
public:
    AeadKey();

    void set(const uint8_t key[16], const uint8_t salt[4]);

    void seal(uint64_t counter, std::vector<uint8_t>& packet, size_t aadLen, const uint8_t* plain,
              size_t plainLen) const;
    bool open(uint64_t counter, const uint8_t* aad, size_t aadLen, const uint8_t* cipher,
              size_t cipherLen, std::vector<uint8_t>& out) const;

private:
    void makeIv(uint64_t counter, uint8_t out[12]) const;

    Aes128 aes_;
    uint8_t hk_[16] = {0};
    uint8_t salt_[4] = {0};
};

struct SessionKeys {
    AeadKey tx;
    AeadKey rx;
};

void randomBytes(void* out, size_t n);
uint32_t randomU32();
uint64_t randomU64();

AeadKey handshakeKey(const std::string& psk);

void deriveSessionKeys(const std::string& psk, const uint8_t clientNonce[kNonceSize],
                       const uint8_t serverNonce[kNonceSize], bool server, SessionKeys& out);

class ReplayWindow {
public:
    bool accept(uint64_t counter);

private:
    uint64_t top_ = 0;
    uint64_t seen_ = 0;
};

}

#endif
