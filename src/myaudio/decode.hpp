#ifndef MYAUDIO_DECODE_H
#define MYAUDIO_DECODE_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "myrtm/codec.hpp"

namespace myaudio {

class Decoder {
public:
    Decoder() = default;
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool feed(const myrtm::AudioFrame& f, std::vector<float>& pcm);
    size_t conceal(std::vector<float>& pcm);
    void reset();

    uint32_t rate() const { return rate_; }
    uint8_t channels() const { return ch_; }
    size_t lastFrames() const { return last_; }

private:
    bool prepare(uint32_t rate, uint8_t channels, myrtm::AudioCodec codec);

    void* opus_ = nullptr;
    myrtm::AudioCodec codec_ = myrtm::AudioCodec::Pcm16;
    uint32_t rate_ = 0;
    uint8_t ch_ = 0;
    size_t last_ = 0;
    std::vector<float> scratch_;
};

bool haveOpus();

}

#endif
