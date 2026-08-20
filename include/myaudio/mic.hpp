#ifndef MYAUDIO_MIC_H
#define MYAUDIO_MIC_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "myaudio/config.hpp"
#include "myrtm/codec.hpp"

namespace myaudio {

class VirtualMic {
public:
    using SourceId = uint32_t;

    static std::shared_ptr<VirtualMic> start(const MicConfig& cfg);
    ~VirtualMic();

    VirtualMic(const VirtualMic&) = delete;
    VirtualMic& operator=(const VirtualMic&) = delete;

    bool feed(SourceId src, const myrtm::AudioFrame& frame);
    void drop(SourceId src);

    void setGain(SourceId src, float gain);
    void setMute(SourceId src, bool mute);
    void setMaster(float gain);
    float master() const;

    std::vector<SourceId> sources() const;
    bool sourceStats(SourceId src, SourceStats& out) const;
    MicStats stats() const;
    Format format() const;
    std::string device() const;
    void stop();

private:
    VirtualMic();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool opusAvailable();

}

#endif
