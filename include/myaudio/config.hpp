#ifndef MYAUDIO_CONFIG_H
#define MYAUDIO_CONFIG_H

#include <cstdint>
#include <functional>
#include <string>

namespace myaudio {

using LogSink = std::function<void(const std::string&)>;

enum class SinkKind { Null, Wasapi, Shared };

struct Format {
    uint32_t rate = 48000;
    uint8_t channels = 2;
};

struct PaceConfig {
    uint32_t targetMs = 60;
    uint32_t minMs = 20;
    uint32_t maxMs = 200;
    uint32_t fadeMs = 4;
    uint32_t maxDriftPpm = 5000;
    uint32_t idleTimeoutMs = 5000;
};

struct MixConfig {
    Format format;
    uint32_t quantumMs = 10;
    float master = 1.0f;
    float ceiling = 0.97f;
};

struct SinkConfig {
    SinkKind kind = SinkKind::Wasapi;
    std::string deviceId;
    std::string deviceMatch;
    bool preferVirtual = true;
    std::string shareName = "myaudio-mic";
    uint32_t shareMs = 200;
};

struct MicConfig {
    MixConfig mix;
    PaceConfig pace;
    SinkConfig sink;
    std::string micName = "MyMic";
    uint32_t maxSources = 16;
    LogSink log;
};

struct SourceStats {
    uint64_t framesIn = 0;
    uint64_t framesLate = 0;
    uint64_t framesLost = 0;
    uint64_t decodeErrors = 0;
    uint64_t concealed = 0;
    uint64_t underruns = 0;
    uint64_t overruns = 0;
    uint64_t samplesOut = 0;
    uint32_t depthMs = 0;
    uint32_t targetMs = 0;
    int32_t driftPpm = 0;
    float gain = 1.0f;
    float peak = 0.0f;
    bool muted = false;
};

struct MicStats {
    uint64_t quanta = 0;
    uint64_t sinkFrames = 0;
    uint64_t sinkRestarts = 0;
    uint64_t limitedQuanta = 0;
    uint64_t droppedFrames = 0;
    size_t sources = 0;
    float peak = 0.0f;
    float master = 1.0f;
    Format format;
    std::string device;
    bool running = false;
};

}

#endif
