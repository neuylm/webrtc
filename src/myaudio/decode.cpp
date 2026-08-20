#include "decode.hpp"

#ifdef MYAUDIO_HAS_OPUS
#include <opus/opus.h>
#endif

namespace myaudio {
namespace {

const size_t kMaxOpusFrames = 5760;

#ifdef MYAUDIO_HAS_OPUS
uint32_t opusRate(uint32_t want) {
    static const uint32_t ok[] = {8000, 12000, 16000, 24000, 48000};
    for (uint32_t r : ok) {
        if (want == r)
            return r;
    }
    for (uint32_t r : ok) {
        if (want < r)
            return r;
    }
    return 48000;
}
#endif

}

bool haveOpus() {
#ifdef MYAUDIO_HAS_OPUS
    return true;
#else
    return false;
#endif
}

Decoder::~Decoder() {
    reset();
}

void Decoder::reset() {
#ifdef MYAUDIO_HAS_OPUS
    if (opus_)
        opus_decoder_destroy(static_cast<OpusDecoder*>(opus_));
#endif
    opus_ = nullptr;
    rate_ = 0;
    ch_ = 0;
    last_ = 0;
}

bool Decoder::prepare(uint32_t rate, uint8_t channels, myrtm::AudioCodec codec) {
    if (channels == 0 || channels > 2)
        return false;
    if (rate < 4000 || rate > 192000)
        return false;

    if (codec == myrtm::AudioCodec::Pcm16) {
        if (opus_ || codec_ != codec)
            reset();
        codec_ = codec;
        rate_ = rate;
        ch_ = channels;
        return true;
    }

#ifdef MYAUDIO_HAS_OPUS
    uint32_t want = opusRate(rate);
    if (opus_ && codec_ == codec && rate_ == want && ch_ == channels)
        return true;
    reset();
    int err = 0;
    OpusDecoder* dec = opus_decoder_create(static_cast<opus_int32>(want), channels, &err);
    if (!dec || err != OPUS_OK) {
        if (dec)
            opus_decoder_destroy(dec);
        return false;
    }
    opus_ = dec;
    codec_ = codec;
    rate_ = want;
    ch_ = channels;
    return true;
#else
    (void)codec;
    return false;
#endif
}

bool Decoder::feed(const myrtm::AudioFrame& f, std::vector<float>& pcm) {
    if (!prepare(f.sampleRate, f.channels, f.codec))
        return false;

    if (f.codec == myrtm::AudioCodec::Pcm16) {
        size_t stride = static_cast<size_t>(ch_) * 2;
        if (f.payload.empty() || f.payload.size() % stride != 0)
            return false;
        size_t frames = f.payload.size() / stride;
        const uint8_t* p = f.payload.data();
        pcm.reserve(pcm.size() + frames * ch_);
        for (size_t i = 0; i < frames * ch_; ++i) {
            int16_t v = static_cast<int16_t>(static_cast<uint16_t>(p[i * 2]) |
                                             (static_cast<uint16_t>(p[i * 2 + 1]) << 8));
            pcm.push_back(static_cast<float>(v) / 32768.0f);
        }
        last_ = frames;
        return true;
    }

#ifdef MYAUDIO_HAS_OPUS
    if (!opus_ || f.payload.empty())
        return false;
    scratch_.resize(kMaxOpusFrames * ch_);
    int got = opus_decode_float(static_cast<OpusDecoder*>(opus_), f.payload.data(),
                                static_cast<opus_int32>(f.payload.size()), scratch_.data(),
                                static_cast<int>(kMaxOpusFrames), 0);
    if (got <= 0)
        return false;
    last_ = static_cast<size_t>(got);
    pcm.insert(pcm.end(), scratch_.begin(), scratch_.begin() + static_cast<ptrdiff_t>(last_ * ch_));
    return true;
#else
    return false;
#endif
}

size_t Decoder::conceal(std::vector<float>& pcm) {
#ifdef MYAUDIO_HAS_OPUS
    if (!opus_ || !last_)
        return 0;
    scratch_.resize(kMaxOpusFrames * ch_);
    size_t want = last_ > kMaxOpusFrames ? kMaxOpusFrames : last_;
    int got = opus_decode_float(static_cast<OpusDecoder*>(opus_), nullptr, 0, scratch_.data(),
                                static_cast<int>(want), 0);
    if (got <= 0)
        return 0;
    pcm.insert(pcm.end(), scratch_.begin(),
               scratch_.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(got) * ch_));
    return static_cast<size_t>(got);
#else
    (void)pcm;
    return 0;
#endif
}

}
