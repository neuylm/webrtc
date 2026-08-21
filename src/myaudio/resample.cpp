#include "resample.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace myaudio {
namespace {

const int kHalf = 16;
const int kOver = 512;
const double kBeta = 9.0;

double bessel0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int i = 1; i < 24; ++i) {
        term *= (x / (2.0 * i)) * (x / (2.0 * i));
        sum += term;
        if (term < sum * 1e-12)
            break;
    }
    return sum;
}

double sinc(double x) {
    if (x < 1e-9 && x > -1e-9)
        return 1.0;
    double a = 3.14159265358979323846 * x;
    return std::sin(a) / a;
}

}

void Resampler::buildWindow() {
    win_.resize(static_cast<size_t>(kHalf) * kOver + 2);
    double norm = bessel0(kBeta);
    for (size_t i = 0; i < win_.size(); ++i) {
        double x = static_cast<double>(i) / kOver;
        double t = x / kHalf;
        if (t >= 1.0) {
            win_[i] = 0.0f;
            continue;
        }
        double w = bessel0(kBeta * std::sqrt(1.0 - t * t)) / norm;
        win_[i] = static_cast<float>(sinc(x) * w);
    }
}

void Resampler::reset(uint32_t inRate, uint32_t outRate, uint8_t channels) {
    if (win_.empty())
        buildWindow();

    inRate_ = inRate ? inRate : 48000;
    outRate_ = outRate ? outRate : 48000;
    ch_ = channels ? channels : 1;
    base_ = static_cast<double>(inRate_) / static_cast<double>(outRate_);
    ppm_ = 0;
    step_ = base_;
    cut_ = std::min(1.0, 1.0 / base_) * 0.95;
    span_ = kHalf / cut_;

    size_t pad = static_cast<size_t>(std::ceil(span_)) + 1;
    hist_.assign(pad * ch_, 0.0f);
    pos_ = static_cast<double>(pad);
    co_.assign(2 * pad + 4, 0.0f);
}

void Resampler::setDrift(int32_t ppm) {
    if (ppm > 200000)
        ppm = 200000;
    if (ppm < -200000)
        ppm = -200000;
    ppm_ = ppm;
    step_ = base_ * (1.0 + static_cast<double>(ppm) * 1e-6);
}

float Resampler::tap(double x) const {
    double a = x < 0 ? -x : x;
    a *= kOver;
    size_t i = static_cast<size_t>(a);
    if (i + 1 >= win_.size())
        return 0.0f;
    double f = a - static_cast<double>(i);
    return static_cast<float>(win_[i] * (1.0 - f) + win_[i + 1] * f);
}

size_t Resampler::process(const float* in, size_t inFrames, std::vector<float>& out) {
    if (!ch_)
        return 0;
    if (inFrames)
        hist_.insert(hist_.end(), in, in + inFrames * ch_);

    size_t frames = hist_.size() / ch_;
    size_t made = 0;

    while (pos_ + span_ < static_cast<double>(frames)) {
        ptrdiff_t first = static_cast<ptrdiff_t>(std::ceil(pos_ - span_));
        if (first < 0)
            first = 0;
        ptrdiff_t last = static_cast<ptrdiff_t>(std::floor(pos_ + span_));
        if (last >= static_cast<ptrdiff_t>(frames))
            last = static_cast<ptrdiff_t>(frames) - 1;
        if (last < first)
            break;
        size_t taps = static_cast<size_t>(last - first + 1);
        if (co_.size() < taps)
            co_.resize(taps + 8);

        double sum = 0.0;
        for (size_t k = 0; k < taps; ++k) {
            float c = tap((static_cast<double>(first + static_cast<ptrdiff_t>(k)) - pos_) * cut_);
            co_[k] = c;
            sum += c;
        }
        double scale = sum > 1e-6 || sum < -1e-6 ? 1.0 / sum : 0.0;

        for (uint8_t c = 0; c < ch_; ++c) {
            double acc = 0.0;
            const float* p = hist_.data() + static_cast<size_t>(first) * ch_ + c;
            for (size_t k = 0; k < taps; ++k) {
                acc += static_cast<double>(*p) * co_[k];
                p += ch_;
            }
            out.push_back(static_cast<float>(acc * scale));
        }
        pos_ += step_;
        made++;
    }

    double keep = std::floor(pos_ - span_) - 1.0;
    if (keep > 0.0) {
        size_t drop = static_cast<size_t>(keep);
        if (drop > frames)
            drop = frames;
        hist_.erase(hist_.begin(), hist_.begin() + static_cast<ptrdiff_t>(drop * ch_));
        pos_ -= static_cast<double>(drop);
    }
    return made;
}

void remap(const float* in, size_t frames, uint8_t inCh, uint8_t outCh, std::vector<float>& out) {
    if (!inCh || !outCh)
        return;
    if (inCh == outCh) {
        out.insert(out.end(), in, in + frames * inCh);
        return;
    }
    if (inCh == 1) {
        for (size_t i = 0; i < frames; ++i) {
            for (uint8_t c = 0; c < outCh; ++c)
                out.push_back(in[i]);
        }
        return;
    }
    if (outCh == 1) {
        for (size_t i = 0; i < frames; ++i) {
            double acc = 0.0;
            for (uint8_t c = 0; c < inCh; ++c)
                acc += in[i * inCh + c];
            out.push_back(static_cast<float>(acc / inCh));
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i) {
        for (uint8_t c = 0; c < outCh; ++c)
            out.push_back(in[i * inCh + (c < inCh ? c : inCh - 1)]);
    }
}

}
