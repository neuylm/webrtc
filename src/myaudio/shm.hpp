#ifndef MYAUDIO_SHM_H
#define MYAUDIO_SHM_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "myaudio/config.hpp"

namespace myaudio {

const uint32_t kShareMagic = 0x4d594155;
const uint32_t kShareVersion = 1;

struct ShareHead {
    uint32_t magic;
    uint32_t version;
    uint32_t rate;
    uint32_t channels;
    uint32_t frames;
    uint32_t stride;
    uint32_t pid;
    uint32_t spare;
    std::atomic<uint64_t> write;
    std::atomic<uint64_t> read;
    std::atomic<uint64_t> dropped;
    std::atomic<uint32_t> live;
    uint32_t pad;
};

static_assert(sizeof(ShareHead) == 64, "share header layout must stay fixed");

class ShareWriter {
public:
    ~ShareWriter();

    bool open(const std::string& name, const Format& fmt, uint32_t ms);
    void put(const float* in, size_t frames);
    void close();

    bool ready() const { return head_ != nullptr; }
    const std::string& path() const { return path_; }
    uint64_t written() const;
    uint64_t dropped() const;
    size_t capacity() const;

private:
    void* map_ = nullptr;
    void* file_ = nullptr;
    void* event_ = nullptr;
    ShareHead* head_ = nullptr;
    float* data_ = nullptr;
    size_t cap_ = 0;
    uint8_t ch_ = 0;
    std::string path_;
};

}

#endif
