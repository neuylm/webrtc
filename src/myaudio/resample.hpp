#ifndef MYAUDIO_RESAMPLE_H
#define MYAUDIO_RESAMPLE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace myaudio {

class Resampler {
public:
    void reset(uint32_t inRate, uint32_t outRate, uint8_t channels);
    void setDrift(int32_t ppm);
    size_t process(const float* in, size_t inFrames, std::vector<float>& out);

    int32_t drift() const { return ppm_; }

private:
    float tap(double x) const;
    void buildWindow();

    std::vector<float> win_;
    std::vector<float> co_;
    std::vector<float> hist_;
    double pos_ = 0.0;
    double base_ = 1.0;
    double step_ = 1.0;
    double cut_ = 1.0;
    double span_ = 0.0;
    int32_t ppm_ = 0;
    uint32_t inRate_ = 0;
    uint32_t outRate_ = 0;
    uint8_t ch_ = 0;
};

void remap(const float* in, size_t frames, uint8_t inCh, uint8_t outCh, std::vector<float>& out);

}

#endif
