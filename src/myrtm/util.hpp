#ifndef MYRTM_UTIL_H
#define MYRTM_UTIL_H

#include <chrono>
#include <cstdint>

namespace myrtm {

inline uint32_t nowMs() {
    static const std::chrono::steady_clock::time_point base = std::chrono::steady_clock::now();
    auto d = std::chrono::steady_clock::now() - base;
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
}

inline uint64_t unixMs() {
    auto d = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
}

inline int32_t seqDiff(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b); }

inline bool seqLess(uint32_t a, uint32_t b) { return seqDiff(a, b) < 0; }

}

#endif
