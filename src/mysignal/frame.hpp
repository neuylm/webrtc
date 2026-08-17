#ifndef MYSIGNAL_FRAME_H
#define MYSIGNAL_FRAME_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mysignal {

const uint8_t kOpContinuation = 0x0;
const uint8_t kOpText = 0x1;
const uint8_t kOpBinary = 0x2;
const uint8_t kOpClose = 0x8;
const uint8_t kOpPing = 0x9;
const uint8_t kOpPong = 0xa;

struct Frame {
    bool fin = false;
    uint8_t opcode = 0;
    std::vector<uint8_t> payload;
};

enum class FrameState { NeedMore, Ok, Bad };

FrameState readFrame(const uint8_t* p, size_t n, size_t limit, bool expectMask, Frame& out,
                     size_t& consumed);

void writeFrame(uint8_t opcode, const uint8_t* p, size_t n, std::vector<uint8_t>& out);
void writeText(const std::string& text, std::vector<uint8_t>& out);
void writeClose(uint16_t code, std::vector<uint8_t>& out);

}

#endif
