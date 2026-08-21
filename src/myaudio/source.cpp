#include "source.hpp"

#include <algorithm>

namespace myaudio {

Source::Source(const PaceConfig& pace, const Format& out) : cfg_(pace) {
    if (cfg_.fadeMs < 1)
        cfg_.fadeMs = 1;
    if (cfg_.minMs < 5)
        cfg_.minMs = 5;
    if (cfg_.maxMs < cfg_.minMs + 20)
        cfg_.maxMs = cfg_.minMs + 20;
    if (cfg_.targetMs < cfg_.minMs)
        cfg_.targetMs = cfg_.minMs;
    if (cfg_.targetMs > cfg_.maxMs)
        cfg_.targetMs = cfg_.maxMs;
    if (cfg_.maxDriftPpm > 100000)
        cfg_.maxDriftPpm = 100000;
    target_.store(cfg_.targetMs);
    retarget(out);
}

void Source::retarget(const Format& out) {
    out_ = out;
    if (!out_.rate)
        out_.rate = 48000;
    if (!out_.channels)
        out_.channels = 1;

    size_t cap = static_cast<size_t>(out_.rate) * (cfg_.maxMs + 120) / 1000;
    ring_.reset(cap, out_.channels);
    tail_.assign(out_.channels, 0.0f);
    tmp_.assign(static_cast<size_t>(out_.rate) / 20 * out_.channels, 0.0f);
    fadeFrames_ = static_cast<size_t>(out_.rate) * cfg_.fadeMs / 1000;
    if (!fadeFrames_)
        fadeFrames_ = 1;
    fade_ = 0;
    hush_ = 0;
    prime_ = true;
    srcRate_ = 0;
    srcCh_ = 0;
    haveTs_ = false;
    lastSamples_ = 0;
    lastArrival_ = 0;
    jitter_ = 0.0;
    ppm_ = 0.0;
    drift_.store(0);
    peak_.store(0.0f);
}

size_t Source::targetFrames() const {
    size_t n = static_cast<size_t>(out_.rate) * target_.load(std::memory_order_relaxed) / 1000;
    return n ? n : 1;
}

void Source::setGain(float g) {
    if (g < 0.0f)
        g = 0.0f;
    if (g > 8.0f)
        g = 8.0f;
    gain_.store(g, std::memory_order_relaxed);
}

void Source::setMute(bool m) {
    mute_.store(m, std::memory_order_relaxed);
}

bool Source::idle(uint32_t now) const {
    uint32_t seen = touched_.load(std::memory_order_relaxed);
    if (!seen)
        return false;
    return static_cast<int32_t>(now - seen) > static_cast<int32_t>(cfg_.idleTimeoutMs);
}

void Source::pace(size_t added) {
    (void)added;
    double tf = static_cast<double>(targetFrames());
    double err = (static_cast<double>(ring_.depth()) - tf) / tf;
    if (err > 1.0)
        err = 1.0;
    if (err < -1.0)
        err = -1.0;
    double want = err * static_cast<double>(cfg_.maxDriftPpm);
    ppm_ += (want - ppm_) * 0.08;
    int32_t v = static_cast<int32_t>(ppm_);
    drift_.store(v, std::memory_order_relaxed);
    rs_.setDrift(v);
}

bool Source::push(const myrtm::AudioFrame& f, uint32_t now) {
    std::lock_guard<std::mutex> lk(feedMtx_);
    in_.fetch_add(1, std::memory_order_relaxed);
    touched_.store(now, std::memory_order_relaxed);

    if (f.channels == 0 || f.channels > 2 || f.sampleRate < 4000 || f.sampleRate > 192000) {
        bad_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    size_t miss = 0;
    if (haveTs_ && lastSamples_) {
        int32_t adv = static_cast<int32_t>(f.timestamp - lastTs_);
        if (adv <= 0) {
            if (adv > -static_cast<int32_t>(f.sampleRate * 2)) {
                late_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            haveTs_ = false;
            lastSamples_ = 0;
            ring_.clear();
            dec_.reset();
            srcRate_ = 0;
            srcCh_ = 0;
        } else {
            size_t steps = static_cast<size_t>(adv) / lastSamples_;
            if (steps > 1)
                miss = std::min<size_t>(steps - 1, 5);
        }
    }

    pcm_.clear();
    for (size_t i = 0; i < miss; ++i) {
        if (!dec_.conceal(pcm_))
            break;
        hidden_.fetch_add(1, std::memory_order_relaxed);
    }
    size_t hid = pcm_.size();
    if (miss)
        lost_.fetch_add(miss, std::memory_order_relaxed);

    bool ok = dec_.feed(f, pcm_);
    if (!ok)
        bad_.fetch_add(1, std::memory_order_relaxed);
    if (!dec_.channels() || !dec_.rate() || pcm_.empty())
        return ok;

    if (dec_.rate() != srcRate_ || dec_.channels() != srcCh_) {
        if (hid && hid <= pcm_.size())
            pcm_.erase(pcm_.begin(), pcm_.begin() + static_cast<ptrdiff_t>(hid));
        if (pcm_.empty())
            return ok;
        srcRate_ = dec_.rate();
        srcCh_ = dec_.channels();
        rs_.reset(srcRate_, out_.rate, srcCh_);
        rs_.setDrift(static_cast<int32_t>(ppm_));
        ring_.clear();
    }

    wide_.clear();
    size_t made = rs_.process(pcm_.data(), pcm_.size() / srcCh_, wide_);
    if (made) {
        map_.clear();
        remap(wide_.data(), made, srcCh_, out_.channels, map_);
        uint64_t before = ring_.dropped();
        ring_.push(map_.data(), made);
        if (ring_.dropped() != before)
            over_.fetch_add(1, std::memory_order_relaxed);
    }

    if (ok) {
        size_t frames = dec_.lastFrames();
        size_t declared = frames;
        if (dec_.rate() != f.sampleRate && dec_.rate())
            declared = frames * f.sampleRate / dec_.rate();
        if (declared) {
            lastTs_ = f.timestamp;
            lastSamples_ = declared;
            haveTs_ = true;
        }

        uint32_t span = static_cast<uint32_t>(frames * 1000 / dec_.rate());
        if (!span)
            span = 1;
        if (lastArrival_) {
            int32_t gap = static_cast<int32_t>(now - lastArrival_) - static_cast<int32_t>(span);
            if (gap < 0)
                gap = -gap;
            jitter_ += (static_cast<double>(gap) - jitter_) / 16.0;
        }
        lastArrival_ = now;

        uint32_t want = static_cast<uint32_t>(jitter_ * 3.0) + span * 2;
        if (want < cfg_.targetMs)
            want = cfg_.targetMs;
        if (want < cfg_.minMs)
            want = cfg_.minMs;
        if (want > cfg_.maxMs)
            want = cfg_.maxMs;
        uint32_t cur = target_.load(std::memory_order_relaxed);
        uint32_t next = want > cur
                            ? want
                            : static_cast<uint32_t>(cur * 0.995 + want * 0.005 + 0.5);
        target_.store(next, std::memory_order_relaxed);
    }

    pace(made);
    return ok;
}

void Source::render(float* mix, size_t frames) {
    uint8_t ch = out_.channels;
    size_t need = frames * ch;
    if (tmp_.size() < need)
        tmp_.resize(need);

    if (prime_) {
        size_t want = targetFrames();
        if (want < frames)
            want = frames;
        if (ring_.depth() >= want) {
            prime_ = false;
            fade_ = fadeFrames_;
            if (hush_) {
                double left = static_cast<double>(hush_) / fadeFrames_;
                for (uint8_t c = 0; c < ch; ++c)
                    tail_[c] = static_cast<float>(tail_[c] * left);
                hush_ = 0;
            }
        }
    }

    size_t got = 0;
    if (!prime_) {
        bool seam = false;
        got = ring_.read(tmp_.data(), frames, seam);
        if (seam && got) {
            fade_ = fadeFrames_;
            size_t want = targetFrames();
            size_t deep = ring_.depth();
            if (deep > want + frames)
                ring_.skip(deep - want);
        }

        if (fade_ && got) {
            size_t fn = std::min(fade_, got);
            double span = static_cast<double>(fadeFrames_);
            size_t done = fadeFrames_ - fade_;
            for (size_t i = 0; i < fn; ++i) {
                double a = static_cast<double>(done + i + 1) / span;
                for (uint8_t c = 0; c < ch; ++c) {
                    float& v = tmp_[i * ch + c];
                    v = static_cast<float>(tail_[c] * (1.0 - a) + v * a);
                }
            }
            fade_ -= fn;
        }

        if (got) {
            for (uint8_t c = 0; c < ch; ++c)
                tail_[c] = tmp_[(got - 1) * ch + c];
        }

        if (got < frames) {
            dry_.fetch_add(1, std::memory_order_relaxed);
            prime_ = true;
            fade_ = 0;
            hush_ = fadeFrames_;
        }
    }

    if (got < frames) {
        double span = static_cast<double>(fadeFrames_);
        for (size_t i = got; i < frames; ++i) {
            double a = 0.0;
            if (hush_) {
                hush_--;
                a = static_cast<double>(hush_) / span;
            }
            for (uint8_t c = 0; c < ch; ++c)
                tmp_[i * ch + c] = static_cast<float>(tail_[c] * a);
        }
        if (!hush_) {
            for (uint8_t c = 0; c < ch; ++c)
                tail_[c] = 0.0f;
        }
    }

    float g = mute_.load(std::memory_order_relaxed) ? 0.0f : gain_.load(std::memory_order_relaxed);
    float pk = 0.0f;
    for (size_t i = 0; i < need; ++i) {
        float v = tmp_[i] * g;
        mix[i] += v;
        float a = v < 0.0f ? -v : v;
        if (a > pk)
            pk = a;
    }

    float old = peak_.load(std::memory_order_relaxed) * 0.85f;
    peak_.store(pk > old ? pk : old, std::memory_order_relaxed);
    outFrames_.fetch_add(frames, std::memory_order_relaxed);
}

void Source::fill(SourceStats& s) const {
    s.framesIn = in_.load(std::memory_order_relaxed);
    s.framesLate = late_.load(std::memory_order_relaxed);
    s.framesLost = lost_.load(std::memory_order_relaxed);
    s.decodeErrors = bad_.load(std::memory_order_relaxed);
    s.concealed = hidden_.load(std::memory_order_relaxed);
    s.underruns = dry_.load(std::memory_order_relaxed);
    s.overruns = over_.load(std::memory_order_relaxed);
    s.samplesOut = outFrames_.load(std::memory_order_relaxed);
    s.depthMs = out_.rate ? static_cast<uint32_t>(ring_.depth() * 1000 / out_.rate) : 0;
    s.targetMs = target_.load(std::memory_order_relaxed);
    s.driftPpm = drift_.load(std::memory_order_relaxed);
    s.gain = gain_.load(std::memory_order_relaxed);
    s.peak = peak_.load(std::memory_order_relaxed);
    s.muted = mute_.load(std::memory_order_relaxed);
}

}
