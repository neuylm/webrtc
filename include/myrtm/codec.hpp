#ifndef MYRTM_CODEC_H
#define MYRTM_CODEC_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace myrtm {

constexpr size_t kMaxBatch = 255;

enum class InputKind : uint8_t {
    KeyDown = 1,
    KeyUp = 2,
    MouseMove = 3,
    MouseDown = 4,
    MouseUp = 5,
    Wheel = 6
};

constexpr uint8_t kFlagAbsolute = 0x01;
constexpr uint8_t kFlagHorizontal = 0x02;
constexpr uint8_t kFlagExtended = 0x04;

struct InputEvent {
    InputKind kind = InputKind::KeyDown;
    uint8_t flags = 0;
    uint16_t code = 0;
    int32_t x = 0;
    int32_t y = 0;
    int16_t wheel = 0;
    uint32_t timeMs = 0;
};

void encodeInputBatch(const InputEvent* ev, size_t count, std::vector<uint8_t>& out);
bool decodeInputBatch(const uint8_t* p, size_t n, std::vector<InputEvent>& out);

enum class AudioCodec : uint8_t { Pcm16 = 0, Opus = 1 };

struct AudioFrame {
    AudioCodec codec = AudioCodec::Opus;
    uint8_t channels = 1;
    uint32_t sampleRate = 48000;
    uint32_t timestamp = 0;
    std::vector<uint8_t> payload;
};

void encodeAudioFrame(const AudioFrame& f, std::vector<uint8_t>& out);
bool decodeAudioFrame(const uint8_t* p, size_t n, AudioFrame& out);

}

#endif
