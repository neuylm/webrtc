#include "wire.hpp"

#include <cstring>

namespace myrtm {

void writeHeader(std::vector<uint8_t>& out, const Header& h) {
    Writer w(out);
    w.u8(kMagic0);
    w.u8(kMagic1);
    w.u8(kVersion);
    w.u8(static_cast<uint8_t>(h.type));
    w.u32(h.session);
    w.u64(h.counter);
}

bool readHeader(const uint8_t* p, size_t n, Header& h) {
    if (n < kHeaderSize + kTagSize)
        return false;
    if (p[0] != kMagic0 || p[1] != kMagic1 || p[2] != kVersion)
        return false;

    uint8_t t = p[3];
    if (t < static_cast<uint8_t>(PacketType::Hello) || t > static_cast<uint8_t>(PacketType::Punch))
        return false;

    Reader r(p + 4, kHeaderSize - 4);
    h.type = static_cast<PacketType>(t);
    h.session = r.u32();
    h.counter = r.u64();
    return !r.bad();
}

void writeDataHeader(std::vector<uint8_t>& out, const DataHeader& h) {
    Writer w(out);
    w.u8(h.channel);
    w.u8(h.frag);
    w.u32(h.seq);
    w.u32(h.sendTs);
}

bool readDataHeader(Reader& r, DataHeader& h) {
    h.channel = r.u8();
    h.frag = r.u8();
    h.seq = r.u32();
    h.sendTs = r.u32();
    return !r.bad() && h.channel < kChanCount;
}

void writeAck(std::vector<uint8_t>& out, const AckBody& a) {
    Writer w(out);
    w.u8(a.channel);
    w.u32(a.una);
    w.u32(a.bitmap);
    w.u32(a.echoTs);
    w.u16(a.delay);
}

bool readAck(Reader& r, AckBody& a) {
    a.channel = r.u8();
    a.una = r.u32();
    a.bitmap = r.u32();
    a.echoTs = r.u32();
    a.delay = r.u16();
    return !r.bad() && a.channel < kChanCount;
}

void writeHello(std::vector<uint8_t>& out, const HelloBody& b) {
    Writer w(out);
    w.u64(b.stamp);
    w.bytes(b.nonce, kNonceSize);
}

bool readHello(Reader& r, HelloBody& b) {
    b.stamp = r.u64();
    const uint8_t* p = r.take(kNonceSize);
    if (!p)
        return false;
    std::memcpy(b.nonce, p, kNonceSize);
    return !r.bad();
}

void writeWelcome(std::vector<uint8_t>& out, const WelcomeBody& b) {
    Writer w(out);
    w.u32(b.session);
    w.u64(b.stamp);
    w.bytes(b.nonce, kNonceSize);
    w.bytes(b.echo, kNonceSize);
}

bool readWelcome(Reader& r, WelcomeBody& b) {
    b.session = r.u32();
    b.stamp = r.u64();
    const uint8_t* p = r.take(kNonceSize);
    const uint8_t* e = r.take(kNonceSize);
    if (!p || !e)
        return false;
    std::memcpy(b.nonce, p, kNonceSize);
    std::memcpy(b.echo, e, kNonceSize);
    return !r.bad();
}

}
