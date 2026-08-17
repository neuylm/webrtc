#include "stun.hpp"

#include "bytes.hpp"

#include <cstring>

namespace myrtm {
namespace {

constexpr uint16_t kBindingRequest = 0x0001;
constexpr uint16_t kBindingSuccess = 0x0101;
constexpr uint16_t kAttrMappedAddress = 0x0001;
constexpr uint16_t kAttrXorMappedAddress = 0x0020;

void fillV4(SockAddr& out, uint16_t port, const uint8_t addr[4]) {
    sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(&out.sa);
    std::memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    std::memcpy(&sin->sin_addr, addr, 4);
    out.len = sizeof(*sin);
}

void fillV6(SockAddr& out, uint16_t port, const uint8_t addr[16]) {
    sockaddr_in6* sin = reinterpret_cast<sockaddr_in6*>(&out.sa);
    std::memset(sin, 0, sizeof(*sin));
    sin->sin6_family = AF_INET6;
    sin->sin6_port = htons(port);
    std::memcpy(&sin->sin6_addr, addr, 16);
    out.len = sizeof(*sin);
}

bool readAddress(Reader& r, bool xored, const uint8_t txid[kStunTxidSize], SockAddr& out) {
    r.u8();
    uint8_t family = r.u8();
    uint16_t port = r.u16();
    if (r.bad())
        return false;

    uint8_t mask[16];
    mask[0] = uint8_t((kStunCookie >> 24) & 0xff);
    mask[1] = uint8_t((kStunCookie >> 16) & 0xff);
    mask[2] = uint8_t((kStunCookie >> 8) & 0xff);
    mask[3] = uint8_t(kStunCookie & 0xff);
    std::memcpy(mask + 4, txid, kStunTxidSize);

    if (xored)
        port = uint16_t(port ^ (kStunCookie >> 16));

    if (family == 0x01) {
        const uint8_t* raw = r.take(4);
        if (!raw)
            return false;
        uint8_t addr[4];
        for (int i = 0; i < 4; ++i)
            addr[i] = uint8_t(raw[i] ^ (xored ? mask[i] : 0));
        fillV4(out, port, addr);
        return true;
    }
    if (family == 0x02) {
        const uint8_t* raw = r.take(16);
        if (!raw)
            return false;
        uint8_t addr[16];
        for (int i = 0; i < 16; ++i)
            addr[i] = uint8_t(raw[i] ^ (xored ? mask[i] : 0));
        fillV6(out, port, addr);
        return true;
    }
    return false;
}

}

bool isStunMessage(const uint8_t* p, size_t n) {
    if (n < kStunHeaderSize)
        return false;
    if (p[0] & 0xc0)
        return false;
    uint32_t cookie = (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) | (uint32_t(p[6]) << 8) | p[7];
    return cookie == kStunCookie;
}

void buildStunRequest(const uint8_t txid[kStunTxidSize], std::vector<uint8_t>& out) {
    Writer w(out);
    w.u16(kBindingRequest);
    w.u16(0);
    w.u32(kStunCookie);
    w.bytes(txid, kStunTxidSize);
}

bool parseStunResponse(const uint8_t* p, size_t n, const uint8_t txid[kStunTxidSize],
                       SockAddr& mapped) {
    if (!isStunMessage(p, n))
        return false;

    Reader r(p, n);
    uint16_t type = r.u16();
    uint16_t length = r.u16();
    r.u32();
    const uint8_t* seen = r.take(kStunTxidSize);
    if (r.bad() || type != kBindingSuccess || !seen)
        return false;
    if (std::memcmp(seen, txid, kStunTxidSize) != 0)
        return false;
    if (length > r.left())
        return false;

    while (r.left() >= 4) {
        uint16_t attr = r.u16();
        uint16_t alen = r.u16();
        if (r.bad() || alen > r.left())
            break;

        size_t padded = (alen + 3u) & ~3u;
        if (attr == kAttrXorMappedAddress || attr == kAttrMappedAddress) {
            Reader body(r.take(alen), alen);
            if (readAddress(body, attr == kAttrXorMappedAddress, txid, mapped))
                return true;
            r.take(padded - alen);
            continue;
        }
        r.take(padded);
    }
    return false;
}

}
