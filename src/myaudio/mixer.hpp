#ifndef MYAUDIO_MIXER_H
#define MYAUDIO_MIXER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "myaudio/config.hpp"

namespace myaudio {

class Bus {
public:
    void reset(const Format& fmt, size_t quantum, float ceiling);
    void setMaster(float g);
    float master() const { return master_.load(std::memory_order_relaxed); }
    float peak() const { return peak_.load(std::memory_order_relaxed); }
    float reduction() const { return gain_; }

    bool run(float* buf, size_t frames);

private:
    std::vector<float> hold_;
    size_t look_ = 0;
    size_t pos_ = 0;
    float ceiling_ = 0.97f;
    float gain_ = 1.0f;
    float rel_ = 0.0f;
    uint8_t ch_ = 2;
    std::atomic<float> master_{1.0f};
    std::atomic<float> peak_{0.0f};
};

}

#endif
