#ifndef MYAUDIO_RING_H
#define MYAUDIO_RING_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace myaudio {

class Ring {
public:
    void reset(size_t frames, uint8_t channels) {
        std::lock_guard<std::mutex> lk(mtx_);
        cap_ = frames ? frames : 1;
        ch_ = channels ? channels : 1;
        buf_.assign(cap_ * ch_, 0.0f);
        head_ = 0;
        size_ = 0;
        seam_ = false;
        dropped_ = 0;
    }

    size_t push(const float* in, size_t frames) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!cap_ || !frames)
            return 0;
        if (frames > cap_) {
            in += (frames - cap_) * ch_;
            dropped_ += frames - cap_;
            frames = cap_;
            seam_ = true;
        }
        if (size_ + frames > cap_) {
            size_t toss = size_ + frames - cap_;
            head_ = (head_ + toss) % cap_;
            size_ -= toss;
            dropped_ += toss;
            seam_ = true;
        }
        size_t tail = (head_ + size_) % cap_;
        size_t first = std::min(frames, cap_ - tail);
        std::copy(in, in + first * ch_, buf_.begin() + tail * ch_);
        if (first < frames)
            std::copy(in + first * ch_, in + frames * ch_, buf_.begin());
        size_ += frames;
        return frames;
    }

    size_t read(float* out, size_t frames, bool& seam) {
        std::lock_guard<std::mutex> lk(mtx_);
        seam = seam_;
        seam_ = false;
        size_t n = std::min(frames, size_);
        if (!n)
            return 0;
        size_t first = std::min(n, cap_ - head_);
        std::copy(buf_.begin() + head_ * ch_, buf_.begin() + (head_ + first) * ch_, out);
        if (first < n)
            std::copy(buf_.begin(), buf_.begin() + (n - first) * ch_, out + first * ch_);
        head_ = (head_ + n) % cap_;
        size_ -= n;
        return n;
    }

    size_t skip(size_t frames) {
        std::lock_guard<std::mutex> lk(mtx_);
        size_t n = std::min(frames, size_);
        head_ = (head_ + n) % cap_;
        size_ -= n;
        if (n) {
            seam_ = true;
            dropped_ += n;
        }
        return n;
    }

    size_t depth() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return size_;
    }

    size_t capacity() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return cap_;
    }

    uint64_t dropped() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return dropped_;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        head_ = 0;
        size_ = 0;
        seam_ = true;
    }

private:
    mutable std::mutex mtx_;
    std::vector<float> buf_;
    size_t cap_ = 0;
    uint8_t ch_ = 1;
    size_t head_ = 0;
    size_t size_ = 0;
    bool seam_ = false;
    uint64_t dropped_ = 0;
};

}

#endif
