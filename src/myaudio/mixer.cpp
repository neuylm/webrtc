#include "mixer.hpp"

#include <algorithm>

namespace myaudio {

void Bus::reset(const Format& fmt, size_t quantum, float ceiling) {
    ch_ = fmt.channels ? fmt.channels : 1;
    look_ = quantum ? quantum : 1;
    ceiling_ = ceiling > 0.05f && ceiling <= 1.0f ? ceiling : 0.97f;
    hold_.assign(look_ * ch_, 0.0f);
    pos_ = 0;
    gain_ = 1.0f;
    rel_ = 3.0f / static_cast<float>(fmt.rate ? fmt.rate : 48000);
    peak_.store(0.0f);
}

void Bus::setMaster(float g) {
    if (g < 0.0f)
        g = 0.0f;
    if (g > 4.0f)
        g = 4.0f;
    master_.store(g, std::memory_order_relaxed);
}

bool Bus::run(float* buf, size_t frames) {
    float m = master_.load(std::memory_order_relaxed);
    size_t total = frames * ch_;
    for (size_t i = 0; i < total; ++i)
        buf[i] *= m;

    bool limited = false;
    if (look_) {
        size_t off = 0;
        while (off < frames) {
            size_t n = std::min(look_, frames - off);
            float p = 0.0f;
            for (size_t i = off * ch_; i < (off + n) * ch_; ++i) {
                float a = buf[i] < 0.0f ? -buf[i] : buf[i];
                if (a > p)
                    p = a;
            }
            float want = 1.0f;
            if (p > ceiling_)
                want = ceiling_ / p;
            float next;
            if (want < gain_) {
                next = want;
            } else {
                next = gain_ + rel_ * static_cast<float>(n);
                if (next > want)
                    next = want;
            }
            for (size_t k = 0; k < n; ++k) {
                float a = gain_ + (next - gain_) * (static_cast<float>(k + 1) / n);
                size_t d = (pos_ + k) % look_;
                for (uint8_t c = 0; c < ch_; ++c) {
                    float held = hold_[d * ch_ + c];
                    hold_[d * ch_ + c] = buf[(off + k) * ch_ + c];
                    buf[(off + k) * ch_ + c] = held * a;
                }
            }
            pos_ = (pos_ + n) % look_;
            gain_ = next;
            if (want < 1.0f)
                limited = true;
            off += n;
        }
    }

    float pk = 0.0f;
    for (size_t i = 0; i < total; ++i) {
        if (buf[i] > 1.0f)
            buf[i] = 1.0f;
        else if (buf[i] < -1.0f)
            buf[i] = -1.0f;
        float a = buf[i] < 0.0f ? -buf[i] : buf[i];
        if (a > pk)
            pk = a;
    }
    float old = peak_.load(std::memory_order_relaxed) * 0.85f;
    peak_.store(pk > old ? pk : old, std::memory_order_relaxed);
    return limited;
}

}
