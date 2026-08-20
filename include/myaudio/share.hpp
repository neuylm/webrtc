#ifndef MYAUDIO_SHARE_H
#define MYAUDIO_SHARE_H

#include <cstddef>
#include <memory>
#include <string>

#include "myaudio/config.hpp"

namespace myaudio {

class ShareReader {
public:
    static std::shared_ptr<ShareReader> open(const std::string& name);
    ~ShareReader();

    ShareReader(const ShareReader&) = delete;
    ShareReader& operator=(const ShareReader&) = delete;

    Format format() const;
    size_t capacity() const;
    size_t pending() const;
    size_t read(float* out, size_t frames);
    bool wait(uint32_t timeoutMs);
    uint64_t dropped() const;
    void close();

private:
    ShareReader();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
