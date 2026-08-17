#ifndef MYRTM_AES_H
#define MYRTM_AES_H

#include <cstddef>
#include <cstdint>

namespace myrtm {

class Aes128 {
public:
    void setKey(const uint8_t key[16]);
    void encrypt(const uint8_t in[16], uint8_t out[16]) const;

private:
    const uint8_t* sbox_ = nullptr;
    uint8_t rk_[176] = {0};
};

void aesCmac(const uint8_t key[16], const uint8_t* msg, size_t len, uint8_t mac[16]);

void aesCmacPrf(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t len, uint8_t out[16]);

}

#endif
