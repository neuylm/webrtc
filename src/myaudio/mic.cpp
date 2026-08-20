#include "myaudio/mic.hpp"

#include "myaudio/device.hpp"

#include "decode.hpp"
#include "mixer.hpp"
#include "sink.hpp"
#include "source.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace myaudio {
namespace {

uint32_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

const char* sinkName(SinkKind kind) {
    switch (kind) {
    case SinkKind::Wasapi:
        return "wasapi";
    case SinkKind::Shared:
        return "shared";
    default:
        return "null";
    }
}

}

struct VirtualMic::Impl {
    MicConfig cfg;
    Format fmt;
    std::unique_ptr<Sink> sink;
    Bus bus;
    mutable std::shared_mutex mtx;
    std::map<SourceId, std::unique_ptr<Source>> live;
    std::atomic<bool> ready{false};
    std::atomic<uint64_t> quanta{0};
    std::atomic<uint64_t> limited{0};
    uint32_t swept = 0;
    bool listed = false;

    void say(const std::string& text) const {
        if (cfg.log)
            cfg.log(text);
    }

    size_t lookahead(const Format& f) const {
        size_t n = static_cast<size_t>(f.rate) * cfg.mix.quantumMs / 1000;
        return n ? n : 1;
    }

    void pull(float* out, size_t frames) {
        if (!ready.load(std::memory_order_acquire))
            return;
        uint8_t ch = fmt.channels ? fmt.channels : 1;
        std::fill(out, out + frames * ch, 0.0f);
        {
            std::shared_lock<std::shared_mutex> lk(mtx);
            for (auto& kv : live)
                kv.second->render(out, frames);
        }
        if (bus.run(out, frames))
            limited.fetch_add(1, std::memory_order_relaxed);
        quanta.fetch_add(1, std::memory_order_relaxed);
    }

    void reformat(const Format& f) {
        std::unique_lock<std::shared_mutex> lk(mtx);
        fmt = f;
        float keep = bus.master();
        bus.reset(f, lookahead(f), cfg.mix.ceiling);
        bus.setMaster(keep);
        for (auto& kv : live)
            kv.second->retarget(f);
        lk.unlock();
        record();
        say("mix format now " + std::to_string(f.rate) + " Hz / " +
            std::to_string(static_cast<int>(f.channels)) + " ch");
    }

    void record() {
        if (cfg.micName.empty())
            return;
        MicRecord rec;
        rec.name = cfg.micName;
        rec.sink = sinkName(cfg.sink.kind);
        rec.device = sink ? sink->device() : std::string();
        rec.share = cfg.sink.kind == SinkKind::Shared ? rec.device : std::string();
        {
            std::shared_lock<std::shared_mutex> lk(mtx);
            rec.format = fmt;
        }
        listed = registerMic(rec) || listed;
    }

    void sweep(uint32_t now) {
        if (now - swept < 1000)
            return;
        swept = now;
        std::vector<SourceId> gone;
        {
            std::shared_lock<std::shared_mutex> lk(mtx);
            for (auto& kv : live) {
                if (kv.second->idle(now))
                    gone.push_back(kv.first);
            }
        }
        if (gone.empty())
            return;
        std::unique_lock<std::shared_mutex> lk(mtx);
        for (SourceId id : gone) {
            auto it = live.find(id);
            if (it != live.end() && it->second->idle(now))
                live.erase(it);
        }
    }
};

VirtualMic::VirtualMic() : impl_(new Impl()) {}

VirtualMic::~VirtualMic() {
    stop();
}

std::shared_ptr<VirtualMic> VirtualMic::start(const MicConfig& cfg) {
    std::shared_ptr<VirtualMic> self(new VirtualMic());
    Impl& s = *self->impl_;
    s.cfg = cfg;
    if (!s.cfg.mix.quantumMs)
        s.cfg.mix.quantumMs = 10;
    if (!s.cfg.maxSources)
        s.cfg.maxSources = 1;
    s.fmt = cfg.mix.format;
    if (!s.fmt.rate)
        s.fmt.rate = 48000;
    if (!s.fmt.channels)
        s.fmt.channels = 2;

    s.sink = makeSink(s.cfg.sink, s.cfg.log);
    if (!s.sink) {
        s.say("no sink for kind " + std::string(sinkName(s.cfg.sink.kind)));
        return nullptr;
    }

    Format got = s.fmt;
    Impl* raw = &s;
    if (!s.sink->open(got, s.cfg.mix.quantumMs,
                      [raw](float* p, size_t n) { raw->pull(p, n); },
                      [raw](const Format& f) { raw->reformat(f); })) {
        s.say("sink open failed");
        return nullptr;
    }

    if (!got.rate)
        got.rate = s.fmt.rate;
    if (!got.channels)
        got.channels = s.fmt.channels;
    s.fmt = got;
    s.bus.reset(got, s.lookahead(got), s.cfg.mix.ceiling);
    s.bus.setMaster(s.cfg.mix.master);
    s.ready.store(true, std::memory_order_release);
    s.record();
    s.say("virtual mic on " + s.sink->device() + " at " + std::to_string(got.rate) + " Hz / " +
          std::to_string(static_cast<int>(got.channels)) + " ch");
    return self;
}

void VirtualMic::stop() {
    Impl& s = *impl_;
    s.ready.store(false, std::memory_order_release);
    if (s.sink)
        s.sink->close();
    if (s.listed) {
        unregisterMic(s.cfg.micName);
        s.listed = false;
    }
    std::unique_lock<std::shared_mutex> lk(s.mtx);
    s.live.clear();
}

bool VirtualMic::feed(SourceId src, const myrtm::AudioFrame& frame) {
    Impl& s = *impl_;
    uint32_t now = nowMs();
    s.sweep(now);

    {
        std::shared_lock<std::shared_mutex> lk(s.mtx);
        auto it = s.live.find(src);
        if (it != s.live.end())
            return it->second->push(frame, now);
    }

    std::unique_lock<std::shared_mutex> lk(s.mtx);
    auto it = s.live.find(src);
    if (it == s.live.end()) {
        if (s.live.size() >= s.cfg.maxSources)
            return false;
        it = s.live.emplace(src, std::unique_ptr<Source>(new Source(s.cfg.pace, s.fmt))).first;
    }
    return it->second->push(frame, now);
}

void VirtualMic::drop(SourceId src) {
    std::unique_lock<std::shared_mutex> lk(impl_->mtx);
    impl_->live.erase(src);
}

void VirtualMic::setGain(SourceId src, float gain) {
    std::shared_lock<std::shared_mutex> lk(impl_->mtx);
    auto it = impl_->live.find(src);
    if (it != impl_->live.end())
        it->second->setGain(gain);
}

void VirtualMic::setMute(SourceId src, bool mute) {
    std::shared_lock<std::shared_mutex> lk(impl_->mtx);
    auto it = impl_->live.find(src);
    if (it != impl_->live.end())
        it->second->setMute(mute);
}

void VirtualMic::setMaster(float gain) {
    impl_->bus.setMaster(gain);
}

float VirtualMic::master() const {
    return impl_->bus.master();
}

std::vector<VirtualMic::SourceId> VirtualMic::sources() const {
    std::vector<SourceId> out;
    std::shared_lock<std::shared_mutex> lk(impl_->mtx);
    out.reserve(impl_->live.size());
    for (const auto& kv : impl_->live)
        out.push_back(kv.first);
    return out;
}

bool VirtualMic::sourceStats(SourceId src, SourceStats& out) const {
    std::shared_lock<std::shared_mutex> lk(impl_->mtx);
    auto it = impl_->live.find(src);
    if (it == impl_->live.end())
        return false;
    it->second->fill(out);
    return true;
}

MicStats VirtualMic::stats() const {
    const Impl& s = *impl_;
    MicStats out;
    out.quanta = s.quanta.load(std::memory_order_relaxed);
    out.limitedQuanta = s.limited.load(std::memory_order_relaxed);
    out.master = s.bus.master();
    out.peak = s.bus.peak();
    out.running = s.ready.load(std::memory_order_acquire);
    if (s.sink) {
        out.sinkFrames = s.sink->frames();
        out.sinkRestarts = s.sink->restarts();
        out.droppedFrames = s.sink->dropped();
        out.device = s.sink->device();
    }
    std::shared_lock<std::shared_mutex> lk(s.mtx);
    out.sources = s.live.size();
    out.format = s.fmt;
    return out;
}

Format VirtualMic::format() const {
    std::shared_lock<std::shared_mutex> lk(impl_->mtx);
    return impl_->fmt;
}

std::string VirtualMic::device() const {
    return impl_->sink ? impl_->sink->device() : std::string();
}

bool opusAvailable() {
    return haveOpus();
}

}
