#include "frame.hpp"

#include <cstring>
#include <string>

namespace mysignal {
namespace {

bool knownOpcode(uint8_t op) {
    return op == kOpContinuation || op == kOpText || op == kOpBinary || op == kOpClose ||
           op == kOpPing || op == kOpPong;
}

void putLength(size_t n, std::vector<uint8_t>& out) {
    if (n < 126) {
        out.push_back(uint8_t(n));
    } else if (n <= 0xffff) {
        out.push_back(126);
        out.push_back(uint8_t(n >> 8));
        out.push_back(uint8_t(n));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; --i)
            out.push_back(uint8_t((uint64_t(n) >> (i * 8)) & 0xff));
    }
}

}

FrameState readFrame(const uint8_t* p, size_t n, size_t limit, bool expectMask, Frame& out,
                     size_t& consumed) {
    if (n < 2)
        return FrameState::NeedMore;

    bool fin = (p[0] & 0x80) != 0;
    if (p[0] & 0x70)
        return FrameState::Bad;

    uint8_t opcode = uint8_t(p[0] & 0x0f);
    if (!knownOpcode(opcode))
        return FrameState::Bad;

    bool masked = (p[1] & 0x80) != 0;
    if (masked != expectMask)
        return FrameState::Bad;

    uint64_t length = uint64_t(p[1] & 0x7f);
    size_t cursor = 2;

    if (length == 126) {
        if (n < cursor + 2)
            return FrameState::NeedMore;
        length = (uint64_t(p[cursor]) << 8) | p[cursor + 1];
        cursor += 2;
    } else if (length == 127) {
        if (n < cursor + 8)
            return FrameState::NeedMore;
        length = 0;
        for (int i = 0; i < 8; ++i)
            length = (length << 8) | p[cursor + i];
        cursor += 8;
        if (length & 0x8000000000000000ull)
            return FrameState::Bad;
    }

    if (opcode >= kOpClose && (!fin || length > 125))
        return FrameState::Bad;
    if (length > limit)
        return FrameState::Bad;

    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (n < cursor + 4)
            return FrameState::NeedMore;
        std::memcpy(mask, p + cursor, 4);
        cursor += 4;
    }

    if (n < cursor + size_t(length))
        return FrameState::NeedMore;

    out.fin = fin;
    out.opcode = opcode;
    out.payload.resize(size_t(length));
    for (size_t i = 0; i < size_t(length); ++i)
        out.payload[i] = uint8_t(p[cursor + i] ^ mask[i & 3]);

    consumed = cursor + size_t(length);
    return FrameState::Ok;
}

void writeFrame(uint8_t opcode, const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
    out.push_back(uint8_t(0x80 | opcode));
    putLength(n, out);
    out.insert(out.end(), p, p + n);
}

void writeText(const std::string& text, std::vector<uint8_t>& out) {
    writeFrame(kOpText, reinterpret_cast<const uint8_t*>(text.data()), text.size(), out);
}

void writeClose(uint16_t code, std::vector<uint8_t>& out) {
    uint8_t body[2] = {uint8_t(code >> 8), uint8_t(code & 0xff)};
    writeFrame(kOpClose, body, sizeof(body), out);
}

}
