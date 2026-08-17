#ifndef MYRTM_STUN_H
#define MYRTM_STUN_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "socket.hpp"

namespace myrtm {

constexpr size_t kStunHeaderSize = 20;
constexpr size_t kStunTxidSize = 12;
constexpr uint32_t kStunCookie = 0x2112a442u;

bool isStunMessage(const uint8_t* p, size_t n);

void buildStunRequest(const uint8_t txid[kStunTxidSize], std::vector<uint8_t>& out);

bool parseStunResponse(const uint8_t* p, size_t n, const uint8_t txid[kStunTxidSize],
                       SockAddr& mapped);

}

#endif
