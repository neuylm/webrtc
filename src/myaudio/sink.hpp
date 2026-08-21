#ifndef MYAUDIO_SINK_H
#define MYAUDIO_SINK_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "myaudio/config.hpp"

namespace myaudio {

class Sink {
public:
    using Pull = std::function<void(float*, size_t)>;
    using Reformat = std::function<void(const Format&)>;

    virtual ~Sink() = default;

    virtual bool open(Format& fmt, uint32_t quantumMs, Pull pull, Reformat onFormat) = 0;
    virtual void close() = 0;
    virtual std::string device() const = 0;
    virtual uint64_t frames() const = 0;
    virtual uint64_t restarts() const = 0;
    virtual uint64_t dropped() const { return 0; }
};

std::unique_ptr<Sink> makeSink(const SinkConfig& cfg, const LogSink& log);
std::unique_ptr<Sink> makeWasapiSink(const SinkConfig& cfg, const LogSink& log);

}

#endif
