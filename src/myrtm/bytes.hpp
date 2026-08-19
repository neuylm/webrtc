#ifndef MYRTM_BYTES_H
#define MYRTM_BYTES_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace myrtm {

class Writer {
public:
    explicit Writer(std::vector<uint8_t>& out) : out_(out) {}

    void u8(uint8_t v) { out_.push_back(v); }

    void u16(uint16_t v) {
        out_.push_back(static_cast<uint8_t>(v >> 8));
        out_.push_back(static_cast<uint8_t>(v));
    }

    void u32(uint32_t v) {
        out_.push_back(uint8_t(v >> 24));
        out_.push_back(uint8_t(v >> 16));
        out_.push_back(uint8_t(v >> 8));
        out_.push_back(uint8_t(v));
    }

    void u64(uint64_t v) {
        u32(uint32_t(v >> 32));
        u32(uint32_t(v));
    }

    void i16(int16_t v) { u16(uint16_t(v)); }

    void bytes(const void* p, size_t n) {
        const uint8_t* s = static_cast<const uint8_t*>(p);
        out_.insert(out_.end(), s, s + n);
    }

private:
    std::vector<uint8_t>& out_;
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}

    bool bad() const { return bad_; }
    size_t left() const { return bad_ ? 0 : static_cast<size_t>(end_ - p_); }

    uint8_t u8() {
        if (left() < 1) {
            bad_ = true;
            return 0;
        }
        return *p_++;
    }

    uint16_t u16() {
        if (left() < 2) {
            bad_ = true;
            return 0;
        }
        uint16_t v = uint16_t((p_[0] << 8) | p_[1]);
        p_ += 2;
        return v;
    }

    uint32_t u32() {
        if (left() < 4) {
            bad_ = true;
            return 0;
        }
        uint32_t v = (uint32_t(p_[0]) << 24) | (uint32_t(p_[1]) << 16) | (uint32_t(p_[2]) << 8) |
                     p_[3];
        p_ += 4;
        return v;
    }

    uint64_t u64() {
        uint64_t hi = u32();
        uint64_t lo = u32();
        return (hi << 32) | lo;
    }

    int16_t i16() { return int16_t(u16()); }

    const uint8_t* take(size_t n) {
        if (left() < n) {
            bad_ = true;
            return nullptr;
        }
        const uint8_t* r = p_;
        p_ += n;
        return r;
    }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    bool bad_ = false;
};

}

#endif
