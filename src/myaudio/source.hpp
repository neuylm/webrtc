#ifndef MYAUDIO_SOURCE_H
#define MYAUDIO_SOURCE_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "myaudio/config.hpp"
#include "myrtm/codec.hpp"

#include "decode.hpp"
#include "resample.hpp"
#include "ring.hpp"

namespace myaudio {

class Source {
public:
    Source(const PaceConfig& pace, const Format& out);

    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;

    bool push(const myrtm::AudioFrame& f, uint32_t now);
    void render(float* mix, size_t frames);
    void retarget(const Format& out);

    void setGain(float g);
    void setMute(bool m);
    float gain() const { return gain_.load(std::memory_order_relaxed); }
    bool muted() const { return mute_.load(std::memory_order_relaxed); }

    bool idle(uint32_t now) const;
    void fill(SourceStats& s) const;

private:
    void pace(size_t added);
    size_t targetFrames() const;

    PaceConfig cfg_;
    std::mutex feedMtx_;
    Format out_;
    Decoder dec_;
    Resampler rs_;
    Ring ring_;
    std::vector<float> pcm_;
    std::vector<float> wide_;
    std::vector<float> map_;
    std::vector<float> tmp_;
    std::vector<float> tail_;
    size_t fadeFrames_ = 1;
    size_t fade_ = 0;
    size_t hush_ = 0;
    bool prime_ = true;
    uint32_t srcRate_ = 0;
    uint8_t srcCh_ = 0;
    uint32_t lastTs_ = 0;
    bool haveTs_ = false;
    size_t lastSamples_ = 0;
    uint32_t lastArrival_ = 0;
    double jitter_ = 0.0;
    double ppm_ = 0.0;

    std::atomic<uint32_t> target_{0};
    std::atomic<int32_t> drift_{0};
    std::atomic<uint32_t> touched_{0};
    std::atomic<float> gain_{1.0f};
    std::atomic<bool> mute_{false};
    std::atomic<float> peak_{0.0f};
    std::atomic<uint64_t> in_{0};
    std::atomic<uint64_t> late_{0};
    std::atomic<uint64_t> lost_{0};
    std::atomic<uint64_t> bad_{0};
    std::atomic<uint64_t> hidden_{0};
    std::atomic<uint64_t> dry_{0};
    std::atomic<uint64_t> over_{0};
    std::atomic<uint64_t> outFrames_{0};
};

}

#endif
