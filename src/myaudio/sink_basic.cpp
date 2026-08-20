#include "sink.hpp"

#include "shm.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace myaudio {
namespace {

class TimedSink : public Sink {
public:
    ~TimedSink() override { TimedSink::close(); }

    bool open(Format& fmt, uint32_t quantumMs, Pull pull, Reformat) override {
        close();
        if (!fmt.rate)
            fmt.rate = 48000;
        if (!fmt.channels)
            fmt.channels = 1;
        if (quantumMs < 1)
            quantumMs = 10;

        fmt_ = fmt;
        quantum_ = static_cast<size_t>(fmt.rate) * quantumMs / 1000;
        if (!quantum_)
            quantum_ = 1;
        if (!begin(fmt_))
            return false;
        pull_ = std::move(pull);
        run_.store(true);
        th_ = std::thread(&TimedSink::loop, this);
        return true;
    }

    void close() override {
        if (run_.exchange(false)) {
            if (th_.joinable())
                th_.join();
        }
        finish();
        pull_ = nullptr;
    }

    uint64_t frames() const override { return frames_.load(std::memory_order_relaxed); }
    uint64_t restarts() const override { return 0; }

protected:
    virtual bool begin(const Format&) { return true; }
    virtual void deliver(const float*, size_t) {}
    virtual void finish() {}

    Format fmt_;
    size_t quantum_ = 0;

private:
    void loop() {
        std::vector<float> buf(quantum_ * fmt_.channels, 0.0f);
        auto tick = std::chrono::nanoseconds(static_cast<int64_t>(quantum_) * 1000000000 /
                                             static_cast<int64_t>(fmt_.rate));
        auto next = std::chrono::steady_clock::now();
        while (run_.load(std::memory_order_relaxed)) {
            std::fill(buf.begin(), buf.end(), 0.0f);
            pull_(buf.data(), quantum_);
            deliver(buf.data(), quantum_);
            frames_.fetch_add(quantum_, std::memory_order_relaxed);
            next += tick;
            auto now = std::chrono::steady_clock::now();
            if (next < now - tick * 5)
                next = now;
            std::this_thread::sleep_until(next);
        }
    }

    Pull pull_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::atomic<uint64_t> frames_{0};
};

class NullSink : public TimedSink {
public:
    ~NullSink() override { close(); }

    std::string device() const override { return "null"; }
};

class ShareSink : public TimedSink {
public:
    ShareSink(const std::string& name, uint32_t ms) : name_(name), ms_(ms) {}
    ~ShareSink() override { close(); }

    std::string device() const override { return out_.ready() ? out_.path() : name_; }
    uint64_t dropped() const override { return out_.dropped(); }

protected:
    bool begin(const Format& fmt) override { return out_.open(name_, fmt, ms_); }

    void deliver(const float* buf, size_t frames) override { out_.put(buf, frames); }

    void finish() override { out_.close(); }

private:
    ShareWriter out_;
    std::string name_;
    uint32_t ms_;
};

}

std::unique_ptr<Sink> makeSink(const SinkConfig& cfg, const LogSink& log) {
    switch (cfg.kind) {
    case SinkKind::Shared:
        return std::unique_ptr<Sink>(new ShareSink(cfg.shareName, cfg.shareMs));
    case SinkKind::Wasapi:
        return makeWasapiSink(cfg, log);
    case SinkKind::Null:
    default:
        return std::unique_ptr<Sink>(new NullSink());
    }
}

}
