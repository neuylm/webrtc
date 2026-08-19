#ifndef MYRTM_WIRE_H
#define MYRTM_WIRE_H

#include <cstdint>
#include <vector>

#include "bytes.hpp"
#include "crypto.hpp"

namespace myrtm {

constexpr uint8_t kMagic0 = 0x52;
constexpr uint8_t kMagic1 = 0x4d;
constexpr uint8_t kVersion = 1;

constexpr size_t kHeaderSize = 16;
constexpr size_t kDataHeaderSize = 10;
constexpr size_t kAckBodySize = 15;
constexpr size_t kHelloBodySize = 24;
constexpr size_t kMaxDatagram = 2048;

constexpr uint8_t kMsgInput = 1;
constexpr uint8_t kMsgRaw = 2;

constexpr uint8_t kChanControl = 0;
constexpr uint8_t kChanAudio = 1;
constexpr uint8_t kChanCount = 2;

enum class PacketType : uint8_t {
    Hello = 1,
    Welcome = 2,
    Data = 3,
    Ack = 4,
    Ping = 5,
    Pong = 6,
    Bye = 7,
    Punch = 8
};

struct Header {
    PacketType type = PacketType::Data;
    uint32_t session = 0;
    uint64_t counter = 0;
};

struct DataHeader {
    uint8_t channel = 0;
    uint8_t frag = 0;
    uint32_t seq = 0;
    uint32_t sendTs = 0;
};

struct AckBody {
    uint8_t channel = 0;
    uint32_t una = 0;
    uint32_t bitmap = 0;
    uint32_t echoTs = 0;
    uint16_t delay = 0;
};

struct HelloBody {
    uint64_t stamp = 0;
    uint8_t nonce[kNonceSize] = {0};
};

struct WelcomeBody {
    uint32_t session = 0;
    uint64_t stamp = 0;
    uint8_t nonce[kNonceSize] = {0};
    uint8_t echo[kNonceSize] = {0};
};

void writeHeader(std::vector<uint8_t>& out, const Header& h);
bool readHeader(const uint8_t* p, size_t n, Header& h);

void writeDataHeader(std::vector<uint8_t>& out, const DataHeader& h);
bool readDataHeader(Reader& r, DataHeader& h);

void writeAck(std::vector<uint8_t>& out, const AckBody& a);
bool readAck(Reader& r, AckBody& a);

void writeHello(std::vector<uint8_t>& out, const HelloBody& b);
bool readHello(Reader& r, HelloBody& b);

void writeWelcome(std::vector<uint8_t>& out, const WelcomeBody& b);
bool readWelcome(Reader& r, WelcomeBody& b);

}

#endif
