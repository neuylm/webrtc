#include "myaudio/device.hpp"
#include "myaudio/mic.hpp"
#include "myaudio/share.hpp"

#include "decode.hpp"
#include "mixer.hpp"
#include "resample.hpp"
#include "ring.hpp"
#include "shm.hpp"
#include "source.hpp"
#include "wincom.hpp"

#ifdef MYAUDIO_HAS_OPUS
#include <opus/opus.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace myaudio;

namespace {

int g_total = 0;
int g_failed = 0;
const char* g_case = "";

void report(bool ok, const char* what, int line) {
    g_total++;
    if (ok)
        return;
    g_failed++;
    std::printf("  FAIL [%s] line %d: %s\n", g_case, line, what);
}

#define CHECK(x) report((x), #x, __LINE__)

const double kPi = 3.14159265358979323846;

struct Tone {
    double phase = 0.0;
    double step = 0.0;

    void set(double freq, uint32_t rate) { step = 2.0 * kPi * freq / rate; }

    float next(double amp) {
        float v = static_cast<float>(std::sin(phase) * amp);
        phase += step;
        if (phase > 2.0 * kPi)
            phase -= 2.0 * kPi;
        return v;
    }
};

myrtm::AudioFrame pcmFrame(uint32_t rate, uint8_t ch, uint32_t ts, size_t frames, Tone& tone,
                           double amp) {
    myrtm::AudioFrame f;
    f.codec = myrtm::AudioCodec::Pcm16;
    f.channels = ch;
    f.sampleRate = rate;
    f.timestamp = ts;
    f.payload.resize(frames * ch * 2);
    for (size_t i = 0; i < frames; ++i) {
        float v = tone.next(amp);
        int16_t s = static_cast<int16_t>(v * 32767.0f);
        for (uint8_t c = 0; c < ch; ++c) {
            size_t at = (i * ch + c) * 2;
            f.payload[at] = static_cast<uint8_t>(s & 0xff);
            f.payload[at + 1] = static_cast<uint8_t>((s >> 8) & 0xff);
        }
    }
    return f;
}

double rms(const std::vector<float>& v, size_t from = 0) {
    if (v.size() <= from)
        return 0.0;
    double acc = 0.0;
    for (size_t i = from; i < v.size(); ++i)
        acc += static_cast<double>(v[i]) * v[i];
    return std::sqrt(acc / (v.size() - from));
}

double peakOf(const std::vector<float>& v) {
    double p = 0.0;
    for (float s : v) {
        double a = s < 0 ? -s : s;
        if (a > p)
            p = a;
    }
    return p;
}

double biggestStep(const std::vector<float>& v, size_t from) {
    double worst = 0.0;
    for (size_t i = from + 1; i < v.size(); ++i) {
        double d = v[i] - v[i - 1];
        if (d < 0)
            d = -d;
        if (d > worst)
            worst = d;
    }
    return worst;
}

size_t crossings(const std::vector<float>& v) {
    size_t n = 0;
    for (size_t i = 1; i < v.size(); ++i) {
        if ((v[i - 1] < 0.0f && v[i] >= 0.0f) || (v[i - 1] >= 0.0f && v[i] < 0.0f))
            n++;
    }
    return n;
}

void caseResampleIdentity() {
    g_case = "resample-identity";
    Resampler rs;
    rs.reset(48000, 48000, 1);

    Tone tone;
    tone.set(1000.0, 48000);
    std::vector<float> in(48000);
    for (float& v : in)
        v = tone.next(0.5);

    std::vector<float> out;
    size_t made = rs.process(in.data(), in.size(), out);
    CHECK(made == out.size());
    CHECK(made > 47950 && made <= 48000);
    CHECK(peakOf(out) > 0.49 && peakOf(out) < 0.51);
    CHECK(crossings(out) >= 1996 && crossings(out) <= 2004);

    Resampler dc;
    dc.reset(48000, 48000, 1);
    std::vector<float> flat(4800, 0.25f);
    std::vector<float> flatOut;
    dc.process(flat.data(), flat.size(), flatOut);
    CHECK(flatOut.size() > 4000);
    double err = 0.0;
    for (size_t i = 1000; i < flatOut.size(); ++i)
        err = std::max(err, std::fabs(flatOut[i] - 0.25));
    CHECK(err < 0.001);
}

void caseResampleRates() {
    g_case = "resample-rates";
    Resampler down;
    down.reset(48000, 44100, 1);
    Tone tone;
    tone.set(1000.0, 48000);
    std::vector<float> in(48000);
    for (float& v : in)
        v = tone.next(0.5);
    std::vector<float> out;
    down.process(in.data(), in.size(), out);
    CHECK(out.size() > 44000 && out.size() < 44120);
    CHECK(peakOf(out) > 0.48 && peakOf(out) < 0.52);
    CHECK(crossings(out) >= 1996 && crossings(out) <= 2004);

    Resampler up;
    up.reset(16000, 48000, 1);
    Tone t2;
    t2.set(1000.0, 16000);
    std::vector<float> in2(16000);
    for (float& v : in2)
        v = t2.next(0.5);
    std::vector<float> out2;
    up.process(in2.data(), in2.size(), out2);
    CHECK(out2.size() > 47800 && out2.size() <= 48000);
    CHECK(peakOf(out2) > 0.48 && peakOf(out2) < 0.52);
    CHECK(crossings(out2) >= 1996 && crossings(out2) <= 2004);

    Resampler stereo;
    stereo.reset(48000, 24000, 2);
    std::vector<float> in3(4800 * 2, 0.1f);
    std::vector<float> out3;
    size_t frames = stereo.process(in3.data(), 4800, out3);
    CHECK(out3.size() == frames * 2);
    CHECK(frames > 2350 && frames <= 2400);
}

void caseResampleAlias() {
    g_case = "resample-alias";
    Resampler rs;
    rs.reset(48000, 16000, 1);
    Tone tone;
    tone.set(15000.0, 48000);
    std::vector<float> in(48000);
    for (float& v : in)
        v = tone.next(0.5);
    std::vector<float> out;
    rs.process(in.data(), in.size(), out);
    CHECK(out.size() > 15900 && out.size() <= 16000);
    CHECK(rms(out, 2000) < 0.02);
}

void caseResampleDrift() {
    g_case = "resample-drift";
    std::vector<float> in(48000, 0.0f);
    Tone tone;
    tone.set(440.0, 48000);
    for (float& v : in)
        v = tone.next(0.4);

    Resampler flat;
    flat.reset(48000, 48000, 1);
    std::vector<float> a;
    flat.process(in.data(), in.size(), a);

    Resampler fast;
    fast.reset(48000, 48000, 1);
    fast.setDrift(10000);
    CHECK(fast.drift() == 10000);
    std::vector<float> b;
    fast.process(in.data(), in.size(), b);
    CHECK(b.size() < a.size());
    double ratio = static_cast<double>(b.size()) / a.size();
    CHECK(ratio > 0.985 && ratio < 0.995);

    Resampler slow;
    slow.reset(48000, 48000, 1);
    slow.setDrift(-10000);
    std::vector<float> c;
    slow.process(in.data(), in.size(), c);
    CHECK(c.size() > a.size());
    CHECK(peakOf(c) > 0.39 && peakOf(c) < 0.41);
}

double toneSnr(const std::vector<float>& y, double freq, uint32_t rate, size_t from, size_t to) {
    double w = 2.0 * kPi * freq / rate;
    double ss = 0, cc = 0, sc = 0, sy = 0, cy = 0;
    for (size_t i = from; i < to; ++i) {
        double s = std::sin(w * i);
        double c = std::cos(w * i);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        sy += s * y[i];
        cy += c * y[i];
    }
    double det = ss * cc - sc * sc;
    if (det == 0.0)
        return 0.0;
    double a = (sy * cc - cy * sc) / det;
    double b = (cy * ss - sy * sc) / det;
    double sig = 0, res = 0;
    for (size_t i = from; i < to; ++i) {
        double fit = a * std::sin(w * i) + b * std::cos(w * i);
        sig += fit * fit;
        double d = y[i] - fit;
        res += d * d;
    }
    if (res <= 0.0)
        return 200.0;
    return 10.0 * std::log10(sig / res);
}

void caseResampleSnr() {
    g_case = "resample-snr";
    struct Leg {
        uint32_t in;
        uint32_t out;
        double freq;
    };
    const Leg legs[] = {{48000, 44100, 1000.0}, {44100, 48000, 997.0}, {16000, 48000, 1000.0},
                        {48000, 24000, 800.0}};
    for (const Leg& leg : legs) {
        Resampler rs;
        rs.reset(leg.in, leg.out, 1);
        Tone tone;
        tone.set(leg.freq, leg.in);
        std::vector<float> in(leg.in);
        for (float& v : in)
            v = tone.next(0.5);
        std::vector<float> out;
        rs.process(in.data(), in.size(), out);
        CHECK(out.size() > leg.out - 200);
        double snr = toneSnr(out, leg.freq, leg.out, 2000, out.size() - 200);
        std::printf("  %u->%u snr %.1f dB\n", leg.in, leg.out, snr);
        CHECK(snr > 70.0);
    }
}

void caseRemap() {
    g_case = "remap";
    std::vector<float> mono = {0.1f, 0.2f, 0.3f};
    std::vector<float> out;
    remap(mono.data(), 3, 1, 2, out);
    CHECK(out.size() == 6);
    CHECK(out[0] == 0.1f && out[1] == 0.1f && out[4] == 0.3f);

    std::vector<float> stereo = {0.2f, 0.4f, -0.2f, 0.2f};
    std::vector<float> down;
    remap(stereo.data(), 2, 2, 1, down);
    CHECK(down.size() == 2);
    CHECK(std::fabs(down[0] - 0.3f) < 1e-6);
    CHECK(std::fabs(down[1] - 0.0f) < 1e-6);

    std::vector<float> same;
    remap(stereo.data(), 2, 2, 2, same);
    CHECK(same.size() == 4 && same[3] == 0.2f);
}

void caseRing() {
    g_case = "ring";
    Ring r;
    r.reset(8, 1);
    CHECK(r.capacity() == 8);
    CHECK(r.depth() == 0);

    std::vector<float> in = {1, 2, 3, 4, 5};
    CHECK(r.push(in.data(), 5) == 5);
    CHECK(r.depth() == 5);

    float out[8] = {0};
    bool seam = true;
    CHECK(r.read(out, 3, seam) == 3);
    CHECK(!seam);
    CHECK(out[0] == 1 && out[2] == 3);
    CHECK(r.depth() == 2);

    std::vector<float> more = {6, 7, 8, 9, 10, 11};
    r.push(more.data(), 6);
    CHECK(r.depth() == 8);
    CHECK(r.dropped() == 0);

    std::vector<float> spill = {12, 13};
    r.push(spill.data(), 2);
    CHECK(r.depth() == 8);
    CHECK(r.dropped() == 2);
    CHECK(r.read(out, 8, seam) == 8);
    CHECK(seam);
    CHECK(out[0] == 6 && out[7] == 13);

    r.reset(4, 2);
    std::vector<float> pair = {1, 2, 3, 4, 5, 6};
    r.push(pair.data(), 3);
    CHECK(r.depth() == 3);
    float st[8] = {0};
    CHECK(r.read(st, 3, seam) == 3);
    CHECK(st[0] == 1 && st[1] == 2 && st[5] == 6);

    r.reset(4, 1);
    std::vector<float> big = {1, 2, 3, 4, 5, 6};
    r.push(big.data(), 6);
    CHECK(r.depth() == 4);
    CHECK(r.read(out, 4, seam) == 4);
    CHECK(out[0] == 3 && out[3] == 6);
}

void caseDecodePcm() {
    g_case = "decode-pcm";
    Decoder dec;
    Tone tone;
    tone.set(1000.0, 48000);
    myrtm::AudioFrame f = pcmFrame(48000, 1, 0, 480, tone, 0.5);

    std::vector<float> pcm;
    CHECK(dec.feed(f, pcm));
    CHECK(pcm.size() == 480);
    CHECK(dec.rate() == 48000);
    CHECK(dec.channels() == 1);
    CHECK(dec.lastFrames() == 480);
    CHECK(peakOf(pcm) > 0.45 && peakOf(pcm) <= 0.51);

    myrtm::AudioFrame bad = f;
    bad.payload.pop_back();
    pcm.clear();
    CHECK(!dec.feed(bad, pcm));

    myrtm::AudioFrame zero = f;
    zero.channels = 0;
    CHECK(!dec.feed(zero, pcm));

    myrtm::AudioFrame empty = f;
    empty.payload.clear();
    CHECK(!dec.feed(empty, pcm));

    Tone t2;
    t2.set(500.0, 44100);
    myrtm::AudioFrame st = pcmFrame(44100, 2, 0, 441, t2, 0.25);
    pcm.clear();
    CHECK(dec.feed(st, pcm));
    CHECK(pcm.size() == 882);
    CHECK(dec.channels() == 2);
    CHECK(dec.rate() == 44100);
    CHECK(dec.conceal(pcm) == 0);
}

void caseDecodeOpus() {
    g_case = "decode-opus";
#ifdef MYAUDIO_HAS_OPUS
    CHECK(opusAvailable());
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
    CHECK(enc != nullptr && err == OPUS_OK);
    if (!enc)
        return;

    Tone tone;
    tone.set(440.0, 48000);
    std::vector<float> pcm(960);
    for (float& v : pcm)
        v = tone.next(0.5);

    std::vector<uint8_t> packet(4000);
    int n = opus_encode_float(enc, pcm.data(), 960, packet.data(),
                              static_cast<opus_int32>(packet.size()));
    CHECK(n > 0);
    packet.resize(n > 0 ? static_cast<size_t>(n) : 0);

    myrtm::AudioFrame f;
    f.codec = myrtm::AudioCodec::Opus;
    f.channels = 1;
    f.sampleRate = 48000;
    f.timestamp = 0;
    f.payload = packet;

    Decoder dec;
    std::vector<float> out;
    CHECK(dec.feed(f, out));
    CHECK(out.size() == 960);
    CHECK(dec.lastFrames() == 960);
    CHECK(rms(out) > 0.05);

    size_t before = out.size();
    CHECK(dec.conceal(out) == 960);
    CHECK(out.size() == before + 960);

    myrtm::AudioFrame junk = f;
    junk.payload.assign(20, 0xff);
    std::vector<float> trash;
    CHECK(!dec.feed(junk, trash));

    opus_encoder_destroy(enc);
#else
    CHECK(!opusAvailable());
    Decoder dec;
    myrtm::AudioFrame f;
    f.codec = myrtm::AudioCodec::Opus;
    f.channels = 1;
    f.sampleRate = 48000;
    f.payload.assign(40, 0x3c);
    std::vector<float> out;
    CHECK(!dec.feed(f, out));
#endif
}

void caseSourceSteady() {
    g_case = "source-steady";
    PaceConfig pace;
    Format fmt;
    fmt.rate = 48000;
    fmt.channels = 1;
    Source src(pace, fmt);

    Tone tone;
    tone.set(1000.0, 48000);
    std::vector<float> tape;
    std::vector<float> quantum(480);

    uint32_t now = 0;
    uint32_t ts = 0;
    for (int i = 0; i < 100; ++i) {
        myrtm::AudioFrame f = pcmFrame(48000, 1, ts, 960, tone, 0.5);
        CHECK(src.push(f, now) || i < 0);
        ts += 960;
        now += 20;
        for (int q = 0; q < 2; ++q) {
            std::fill(quantum.begin(), quantum.end(), 0.0f);
            src.render(quantum.data(), quantum.size());
            tape.insert(tape.end(), quantum.begin(), quantum.end());
        }
    }

    SourceStats st;
    src.fill(st);
    CHECK(st.framesIn == 100);
    CHECK(st.decodeErrors == 0);
    CHECK(st.framesLost == 0);
    CHECK(st.underruns <= 1);
    CHECK(st.depthMs > 20 && st.depthMs < 160);
    CHECK(tape.size() == 96000);
    CHECK(peakOf(tape) > 0.45);
    CHECK(biggestStep(tape, 10000) < 0.15);
    CHECK(st.peak > 0.1f);
}

void caseSourceGap() {
    g_case = "source-gap";
    PaceConfig pace;
    Format fmt;
    fmt.rate = 48000;
    fmt.channels = 1;
    Source src(pace, fmt);

    Tone tone;
    tone.set(700.0, 48000);
    std::vector<float> tape;
    std::vector<float> quantum(480);

    uint32_t now = 0;
    uint32_t ts = 0;
    for (int i = 0; i < 120; ++i) {
        bool skip = i >= 60 && i < 63;
        myrtm::AudioFrame f = pcmFrame(48000, 1, ts, 960, tone, 0.5);
        ts += 960;
        now += 20;
        if (!skip)
            src.push(f, now);
        for (int q = 0; q < 2; ++q) {
            std::fill(quantum.begin(), quantum.end(), 0.0f);
            src.render(quantum.data(), quantum.size());
            tape.insert(tape.end(), quantum.begin(), quantum.end());
        }
    }

    SourceStats st;
    src.fill(st);
    CHECK(st.framesIn == 117);
    CHECK(st.framesLost == 3);
    CHECK(biggestStep(tape, 5000) < 0.15);

    Source late(pace, fmt);
    Tone t2;
    t2.set(700.0, 48000);
    myrtm::AudioFrame a = pcmFrame(48000, 1, 1920, 960, t2, 0.5);
    myrtm::AudioFrame b = pcmFrame(48000, 1, 960, 960, t2, 0.5);
    late.push(a, 0);
    late.push(b, 20);
    SourceStats ls;
    late.fill(ls);
    CHECK(ls.framesLate == 1);
}

void casePaceDrift() {
    g_case = "pace-drift";
    PaceConfig pace;
    pace.targetMs = 60;
    Format fmt;
    fmt.rate = 48000;
    fmt.channels = 1;
    Source src(pace, fmt);

    Tone tone;
    tone.set(300.0, 48000);
    std::vector<float> quantum(957);
    uint32_t now = 0;
    uint32_t ts = 0;
    for (int i = 0; i < 400; ++i) {
        myrtm::AudioFrame f = pcmFrame(48000, 1, ts, 960, tone, 0.4);
        src.push(f, now);
        ts += 960;
        now += 20;
        std::fill(quantum.begin(), quantum.end(), 0.0f);
        src.render(quantum.data(), quantum.size());
    }

    SourceStats st;
    src.fill(st);
    CHECK(st.driftPpm > 500);
    CHECK(st.overruns == 0);
    CHECK(st.underruns <= 1);
    CHECK(st.depthMs < 200);

    Source dry(pace, fmt);
    Tone t2;
    t2.set(300.0, 48000);
    std::vector<float> wide(963);
    now = 0;
    ts = 0;
    for (int i = 0; i < 400; ++i) {
        myrtm::AudioFrame f = pcmFrame(48000, 1, ts, 960, t2, 0.4);
        dry.push(f, now);
        ts += 960;
        now += 20;
        std::fill(wide.begin(), wide.end(), 0.0f);
        dry.render(wide.data(), wide.size());
    }
    SourceStats ds;
    dry.fill(ds);
    CHECK(ds.driftPpm < -500);
    CHECK(ds.underruns <= 2);
}

void caseSourceControls() {
    g_case = "source-controls";
    PaceConfig pace;
    Format fmt;
    fmt.rate = 48000;
    fmt.channels = 2;
    Source src(pace, fmt);

    Tone tone;
    tone.set(1000.0, 48000);
    uint32_t ts = 0;
    uint32_t now = 0;
    std::vector<float> quantum(960);

    for (int i = 0; i < 20; ++i) {
        myrtm::AudioFrame f = pcmFrame(48000, 1, ts, 960, tone, 0.5);
        src.push(f, now);
        ts += 960;
        now += 20;
    }
    for (int i = 0; i < 5; ++i) {
        std::fill(quantum.begin(), quantum.end(), 0.0f);
        src.render(quantum.data(), 480);
    }
    std::fill(quantum.begin(), quantum.end(), 0.0f);
    src.render(quantum.data(), 480);
    double loud = peakOf(quantum);
    CHECK(loud > 0.4);
    CHECK(quantum[0] == quantum[1]);

    src.setGain(0.25f);
    std::fill(quantum.begin(), quantum.end(), 0.0f);
    src.render(quantum.data(), 480);
    double quiet = peakOf(quantum);
    CHECK(quiet > 0.08 && quiet < 0.16);

    src.setMute(true);
    CHECK(src.muted());
    std::fill(quantum.begin(), quantum.end(), 0.0f);
    src.render(quantum.data(), 480);
    CHECK(peakOf(quantum) == 0.0);

    src.setMute(false);
    src.setGain(-5.0f);
    CHECK(src.gain() == 0.0f);
    src.setGain(99.0f);
    CHECK(src.gain() == 8.0f);
}

void caseBus() {
    g_case = "bus";
    Format fmt;
    fmt.rate = 48000;
    fmt.channels = 1;
    Bus bus;
    bus.reset(fmt, 480, 0.97f);
    CHECK(bus.master() == 1.0f);

    std::vector<float> block(480, 0.5f);
    CHECK(!bus.run(block.data(), block.size()));
    CHECK(peakOf(block) == 0.0);

    std::vector<float> second(480, 0.5f);
    CHECK(!bus.run(second.data(), second.size()));
    CHECK(std::fabs(peakOf(second) - 0.5) < 1e-5);

    bus.setMaster(0.5f);
    std::vector<float> third(480, 0.5f);
    bus.run(third.data(), third.size());
    CHECK(std::fabs(peakOf(third) - 0.5) < 1e-5);
    std::vector<float> fourth(480, 0.5f);
    bus.run(fourth.data(), fourth.size());
    CHECK(std::fabs(peakOf(fourth) - 0.25) < 1e-5);

    Bus hot;
    hot.reset(fmt, 480, 0.9f);
    bool limited = false;
    std::vector<float> loud(480, 0.0f);
    Tone tone;
    tone.set(200.0, 48000);
    for (int i = 0; i < 20; ++i) {
        for (float& v : loud)
            v = tone.next(2.5);
        if (hot.run(loud.data(), loud.size()))
            limited = true;
        CHECK(peakOf(loud) <= 0.9001);
    }
    CHECK(limited);
    CHECK(hot.reduction() < 0.5f);

    bus.setMaster(9.0f);
    CHECK(bus.master() == 4.0f);
    bus.setMaster(-1.0f);
    CHECK(bus.master() == 0.0f);
}

void caseShare() {
    g_case = "share";
    Format fmt;
    fmt.rate = 48000;
    fmt.channels = 2;

    ShareWriter w;
    CHECK(w.open("myaudio-test-ring", fmt, 100));
    if (!w.ready())
        return;
    CHECK(w.capacity() == 4800);

    std::shared_ptr<ShareReader> r = ShareReader::open("myaudio-test-ring");
    CHECK(r != nullptr);
    if (!r)
        return;
    CHECK(r->format().rate == 48000);
    CHECK(r->format().channels == 2);
    CHECK(r->capacity() == 4800);
    CHECK(r->pending() == 0);

    std::vector<float> block(480 * 2);
    for (size_t i = 0; i < block.size(); ++i)
        block[i] = static_cast<float>(i) / 1000.0f;
    w.put(block.data(), 480);
    CHECK(r->pending() == 480);
    CHECK(r->wait(200));

    std::vector<float> got(480 * 2, 0.0f);
    CHECK(r->read(got.data(), 480) == 480);
    CHECK(std::memcmp(got.data(), block.data(), block.size() * sizeof(float)) == 0);
    CHECK(r->pending() == 0);
    CHECK(r->read(got.data(), 480) == 0);

    for (int i = 0; i < 20; ++i)
        w.put(block.data(), 480);
    CHECK(r->pending() == 4800);
    CHECK(r->read(got.data(), 480) == 480);
    CHECK(r->dropped() > 0);

    ShareWriter dup;
    CHECK(!dup.open("myaudio-test-ring", fmt, 100));

    r->close();
    w.close();
    CHECK(ShareReader::open("myaudio-test-ring-missing") == nullptr);
}

void caseRegistry() {
    g_case = "registry";
    MicRecord rec;
    rec.name = "myaudio-test-mic";
    rec.share = "Local\\myaudio-test-ring";
    rec.sink = "shared";
    rec.device = "unit test";
    rec.format.rate = 44100;
    rec.format.channels = 1;

    CHECK(registerMic(rec));

    MicRecord back;
    CHECK(queryMic(rec.name, back));
    CHECK(back.share == rec.share);
    CHECK(back.sink == "shared");
    CHECK(back.device == "unit test");
    CHECK(back.format.rate == 44100);
    CHECK(back.format.channels == 1);
    CHECK(back.pid != 0);

    bool listed = false;
    for (const MicRecord& m : registeredMics()) {
        if (m.name == rec.name)
            listed = true;
    }
    CHECK(listed);

    CHECK(unregisterMic(rec.name));
    MicRecord gone;
    CHECK(!queryMic(rec.name, gone));

    MicRecord orphan = rec;
    orphan.name = "myaudio-test-orphan";
    orphan.pid = 0xfffffff0;
    CHECK(registerMic(orphan));
    MicRecord dead;
    CHECK(!queryMic(orphan.name, dead));
    CHECK(!queryMic(orphan.name, dead));

    orphan.pid = 0xfffffff0;
    CHECK(registerMic(orphan));
    bool seen = false;
    for (const MicRecord& m : registeredMics()) {
        if (m.name == orphan.name)
            seen = true;
    }
    CHECK(!seen);
    CHECK(!queryMic(orphan.name, dead));
}

void caseDevices() {
    g_case = "devices";
    std::vector<DeviceInfo> outs = devices(false);
    for (const DeviceInfo& d : outs) {
        CHECK(!d.id.empty());
        CHECK(!d.capture);
    }
    std::vector<DeviceInfo> ins = devices(true);
    for (const DeviceInfo& d : ins) {
        CHECK(!d.id.empty());
        CHECK(d.capture);
    }
#ifdef _WIN32
    CHECK(looksLikeCable("CABLE Input (VB-Audio Virtual Cable)"));
    CHECK(looksLikeCable("MyMic Virtual Microphone"));
    CHECK(!looksLikeCable("Speakers (Realtek High Definition Audio)"));
#endif

    DeviceInfo def;
    if (defaultDevice(false, def)) {
        CHECK(!def.id.empty());
        CHECK(def.isDefault);
        CHECK(def.format.rate >= 8000);
    }
}

void caseSourceStall() {
    g_case = "source-stall";
    PaceConfig pace;
    Format fmt;
    fmt.rate = 48000;
    fmt.channels = 1;
    Source src(pace, fmt);

    Tone tone;
    tone.set(880.0, 48000);
    std::vector<float> tape;
    std::vector<float> q(480);
    uint32_t ts = 0;
    uint32_t now = 0;

    for (int i = 0; i < 40; ++i) {
        src.push(pcmFrame(48000, 1, ts, 960, tone, 0.5), now);
        ts += 960;
        now += 20;
        for (int k = 0; k < 2; ++k) {
            std::fill(q.begin(), q.end(), 0.0f);
            src.render(q.data(), q.size());
            tape.insert(tape.end(), q.begin(), q.end());
        }
    }
    size_t before = tape.size();

    for (int i = 0; i < 30; ++i) {
        now += 20;
        for (int k = 0; k < 2; ++k) {
            std::fill(q.begin(), q.end(), 0.0f);
            src.render(q.data(), q.size());
            tape.insert(tape.end(), q.begin(), q.end());
        }
    }
    SourceStats mid;
    src.fill(mid);
    CHECK(mid.underruns > 0);
    CHECK(peakOf(std::vector<float>(tape.end() - 4800, tape.end())) == 0.0);

    ts += 30 * 960;
    for (int i = 0; i < 40; ++i) {
        src.push(pcmFrame(48000, 1, ts, 960, tone, 0.5), now);
        ts += 960;
        now += 20;
        for (int k = 0; k < 2; ++k) {
            std::fill(q.begin(), q.end(), 0.0f);
            src.render(q.data(), q.size());
            tape.insert(tape.end(), q.begin(), q.end());
        }
    }

    std::vector<float> tail(tape.end() - 20000, tape.end());
    CHECK(peakOf(tail) > 0.4);
    CHECK(biggestStep(tape, before) < 0.15);
    CHECK(biggestStep(tape, 2000) < 0.15);
}

void caseSourceSwitch() {
    g_case = "source-switch";
    PaceConfig pace;
    Format fmt;
    fmt.rate = 48000;
    fmt.channels = 1;
    Source src(pace, fmt);

    Tone a;
    a.set(500.0, 48000);
    std::vector<float> tape;
    std::vector<float> q(480);
    uint32_t ts = 0;
    uint32_t now = 0;

    for (int i = 0; i < 30; ++i) {
        src.push(pcmFrame(48000, 1, ts, 960, a, 0.5), now);
        ts += 960;
        now += 20;
        for (int k = 0; k < 2; ++k) {
            std::fill(q.begin(), q.end(), 0.0f);
            src.render(q.data(), q.size());
            tape.insert(tape.end(), q.begin(), q.end());
        }
    }
    size_t seam = tape.size();

    Tone b;
    b.set(500.0, 44100);
    for (int i = 0; i < 40; ++i) {
        src.push(pcmFrame(44100, 2, ts, 882, b, 0.5), now);
        ts += 882;
        now += 20;
        for (int k = 0; k < 2; ++k) {
            std::fill(q.begin(), q.end(), 0.0f);
            src.render(q.data(), q.size());
            tape.insert(tape.end(), q.begin(), q.end());
        }
    }

    SourceStats st;
    src.fill(st);
    CHECK(st.decodeErrors == 0);
    CHECK(st.framesIn == 70);
    std::vector<float> tail(tape.end() - 10000, tape.end());
    CHECK(peakOf(tail) > 0.4);
    CHECK(biggestStep(tape, seam + 4800) < 0.15);
}

void caseSourceRestart() {
    g_case = "source-restart";
    PaceConfig pace;
    Format fmt;
    fmt.rate = 48000;
    fmt.channels = 1;
    Source src(pace, fmt);

    Tone tone;
    tone.set(750.0, 48000);
    std::vector<float> q(480);
    uint32_t ts = 500000;
    uint32_t now = 0;

    for (int i = 0; i < 30; ++i) {
        src.push(pcmFrame(48000, 1, ts, 960, tone, 0.5), now);
        ts += 960;
        now += 20;
        src.render(q.data(), q.size());
    }
    SourceStats first;
    src.fill(first);
    CHECK(first.framesIn == 30);
    CHECK(first.framesLate == 0);

    uint32_t fresh = 0;
    std::vector<float> tape;
    for (int i = 0; i < 30; ++i) {
        src.push(pcmFrame(48000, 1, fresh, 960, tone, 0.5), now);
        fresh += 960;
        now += 20;
        for (int k = 0; k < 2; ++k) {
            std::fill(q.begin(), q.end(), 0.0f);
            src.render(q.data(), q.size());
            tape.insert(tape.end(), q.begin(), q.end());
        }
    }

    SourceStats after;
    src.fill(after);
    CHECK(after.framesIn == 60);
    CHECK(after.framesLate <= 1);
    CHECK(after.depthMs > 10);
    std::vector<float> tail(tape.end() - 10000, tape.end());
    CHECK(peakOf(tail) > 0.4);
    CHECK(biggestStep(tape, 5000) < 0.15);

    Source jitterish(pace, fmt);
    Tone t2;
    t2.set(750.0, 48000);
    jitterish.push(pcmFrame(48000, 1, 96000, 960, t2, 0.5), 0);
    jitterish.push(pcmFrame(48000, 1, 95040, 960, t2, 0.5), 20);
    SourceStats js;
    jitterish.fill(js);
    CHECK(js.framesLate == 1);
}

void caseShareReuse() {
    g_case = "share-reuse";
    Format fmt;
    fmt.rate = 48000;
    fmt.channels = 1;

    ShareWriter first;
    CHECK(first.open("myaudio-test-reuse", fmt, 20));
    if (!first.ready())
        return;
    CHECK(first.capacity() == 960);

    std::shared_ptr<ShareReader> hold = ShareReader::open("myaudio-test-reuse");
    CHECK(hold != nullptr);
    first.close();

    ShareWriter again;
    CHECK(again.open("myaudio-test-reuse", fmt, 2000));
    if (again.ready()) {
        CHECK(again.capacity() >= 256);
        CHECK(again.capacity() < 96000);
        std::vector<float> block(again.capacity(), 0.25f);
        again.put(block.data(), block.size());
        CHECK(again.written() == again.capacity());
        CHECK(again.dropped() == 0);
    }
    again.close();
    if (hold)
        hold->close();
}

void caseSourceRetarget() {
    g_case = "source-retarget";
    PaceConfig pace;
    Format fmt;
    fmt.rate = 48000;
    fmt.channels = 1;
    Source src(pace, fmt);

    Tone tone;
    tone.set(600.0, 48000);
    std::vector<float> q(480);
    uint32_t ts = 0;
    uint32_t now = 0;
    for (int i = 0; i < 20; ++i) {
        src.push(pcmFrame(48000, 1, ts, 960, tone, 0.5), now);
        ts += 960;
        now += 20;
        std::fill(q.begin(), q.end(), 0.0f);
        src.render(q.data(), 480);
    }

    Format other;
    other.rate = 44100;
    other.channels = 2;
    src.retarget(other);

    SourceStats mid;
    src.fill(mid);
    CHECK(mid.depthMs == 0);
    CHECK(mid.driftPpm == 0);

    std::vector<float> tape;
    std::vector<float> wide(882);
    for (int i = 0; i < 40; ++i) {
        src.push(pcmFrame(48000, 1, ts, 960, tone, 0.5), now);
        ts += 960;
        now += 20;
        for (int k = 0; k < 2; ++k) {
            std::fill(wide.begin(), wide.end(), 0.0f);
            src.render(wide.data(), 441);
            tape.insert(tape.end(), wide.begin(), wide.end());
        }
    }

    SourceStats st;
    src.fill(st);
    CHECK(st.decodeErrors == 0);
    CHECK(st.underruns <= 2);
    CHECK(st.depthMs > 10);
    std::vector<float> tail(tape.end() - 10000, tape.end());
    CHECK(peakOf(tail) > 0.4);
    CHECK(tape[20000] == tape[20001]);
    CHECK(biggestStep(tape, 10000) < 0.15);
}

void caseMicNull() {
    g_case = "mic-null";
    MicConfig cfg;
    cfg.sink.kind = SinkKind::Null;
    cfg.mix.format.rate = 48000;
    cfg.mix.format.channels = 2;
    cfg.micName.clear();
    cfg.maxSources = 2;

    std::shared_ptr<VirtualMic> mic = VirtualMic::start(cfg);
    CHECK(mic != nullptr);
    if (!mic)
        return;
    CHECK(mic->format().rate == 48000);
    CHECK(mic->device() == "null");

    Tone a;
    a.set(440.0, 48000);
    Tone b;
    b.set(660.0, 48000);
    uint32_t ts = 0;
    auto begun = std::chrono::steady_clock::now();
    for (int i = 0; i < 25; ++i) {
        mic->feed(1, pcmFrame(48000, 1, ts, 960, a, 0.4));
        mic->feed(2, pcmFrame(48000, 1, ts, 960, b, 0.4));
        ts += 960;
        std::this_thread::sleep_until(begun + std::chrono::milliseconds(20 * (i + 1)));
    }

    MicStats st = mic->stats();
    CHECK(st.running);
    CHECK(st.sources == 2);
    CHECK(st.quanta > 10);
    CHECK(st.peak > 0.1f);
    CHECK(st.sinkFrames > 10000);

    SourceStats one;
    CHECK(mic->sourceStats(1, one));
    CHECK(one.framesIn == 25);
    CHECK(one.decodeErrors == 0);
    CHECK(!mic->sourceStats(99, one));

    CHECK(mic->feed(3, pcmFrame(48000, 1, ts, 960, a, 0.4)) == false);
    CHECK(mic->sources().size() == 2);

    mic->setMaster(0.5f);
    CHECK(mic->master() == 0.5f);
    mic->setMute(1, true);
    mic->setGain(2, 0.5f);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(mic->sourceStats(1, one));
    CHECK(one.muted);

    mic->drop(1);
    CHECK(mic->sources().size() == 1);
    mic->stop();
    CHECK(!mic->stats().running);
}

void caseMicShared() {
    g_case = "mic-shared";
    MicConfig cfg;
    cfg.sink.kind = SinkKind::Shared;
    cfg.sink.shareName = "myaudio-test-mic-ring";
    cfg.sink.shareMs = 200;
    cfg.mix.format.rate = 48000;
    cfg.mix.format.channels = 1;
    cfg.micName = "myaudio-test-vmic";

    std::shared_ptr<VirtualMic> mic = VirtualMic::start(cfg);
    CHECK(mic != nullptr);
    if (!mic)
        return;

    MicRecord rec;
    CHECK(queryMic("myaudio-test-vmic", rec));
    CHECK(rec.sink == "shared");
    CHECK(rec.format.rate == 48000);
    CHECK(!rec.share.empty());

    std::shared_ptr<ShareReader> reader = ShareReader::open(cfg.sink.shareName);
    CHECK(reader != nullptr);
    if (!reader) {
        mic->stop();
        return;
    }
    CHECK(reader->format().rate == 48000);
    CHECK(reader->format().channels == 1);

    Tone tone;
    tone.set(500.0, 48000);
    uint32_t ts = 0;
    std::vector<float> heard;
    std::vector<float> chunk(4800);
    auto begun = std::chrono::steady_clock::now();
    for (int i = 0; i < 40; ++i) {
        mic->feed(11, pcmFrame(48000, 1, ts, 960, tone, 0.5));
        ts += 960;
        std::this_thread::sleep_until(begun + std::chrono::milliseconds(20 * (i + 1)));
        size_t got = reader->read(chunk.data(), chunk.size());
        heard.insert(heard.end(), chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(got));
    }

    CHECK(heard.size() > 20000);
    CHECK(peakOf(heard) > 0.3);
    CHECK(biggestStep(heard, heard.size() / 2) < 0.15);

    MicStats st = mic->stats();
    CHECK(st.sinkFrames > 20000);
    CHECK(st.sinkRestarts == 0);

    mic->stop();
    MicRecord gone;
    CHECK(!queryMic("myaudio-test-vmic", gone));
}

void caseFadeSeam() {
    g_case = "fade-seam";
    PaceConfig pace;
    pace.targetMs = 20;
    pace.minMs = 20;
    Format fmt;
    fmt.rate = 48000;
    fmt.channels = 1;
    Source src(pace, fmt);

    Tone tone;
    tone.set(700.0, 48000);
    uint32_t ts = 0;
    for (int i = 0; i < 3; ++i) {
        src.push(pcmFrame(48000, 1, ts, 960, tone, 0.8), i * 20u);
        ts += 960;
    }

    std::vector<float> tape;
    std::vector<float> q(1000);
    for (int i = 0; i < 6; ++i) {
        std::fill(q.begin(), q.end(), 0.0f);
        src.render(q.data(), q.size());
        tape.insert(tape.end(), q.begin(), q.end());
    }

    SourceStats st;
    src.fill(st);
    CHECK(st.underruns >= 1);
    CHECK(peakOf(tape) > 0.7);
    CHECK(biggestStep(tape, 0) < 0.15);
    CHECK(tape[tape.size() - 1] == 0.0f);
}

void caseSourceFlood() {
    g_case = "source-flood";
    PaceConfig pace;
    Format fmt;
    fmt.rate = 48000;
    fmt.channels = 1;
    Source src(pace, fmt);

    Tone tone;
    tone.set(600.0, 48000);
    uint32_t ts = 0;
    uint32_t now = 0;
    for (int i = 0; i < 60; ++i) {
        src.push(pcmFrame(48000, 1, ts, 960, tone, 0.5), now);
        ts += 960;
        now += 20;
    }

    SourceStats st;
    src.fill(st);
    CHECK(st.overruns > 0);
    CHECK(st.depthMs <= 330);
    CHECK(st.driftPpm > 1000);

    std::vector<float> tape;
    std::vector<float> q(480);
    for (int i = 0; i < 40; ++i) {
        std::fill(q.begin(), q.end(), 0.0f);
        src.render(q.data(), q.size());
        tape.insert(tape.end(), q.begin(), q.end());
    }
    CHECK(peakOf(tape) > 0.4);
    CHECK(biggestStep(tape, 480) < 0.15);
}

void caseSourceResync() {
    g_case = "source-resync";
    PaceConfig pace;
    Format fmt;
    fmt.rate = 48000;
    fmt.channels = 1;
    Source src(pace, fmt);

    Tone tone;
    tone.set(600.0, 48000);
    uint32_t ts = 0;
    uint32_t now = 0;
    for (int i = 0; i < 60; ++i) {
        src.push(pcmFrame(48000, 1, ts, 960, tone, 0.5), now);
        ts += 960;
        now += 20;
    }

    SourceStats st;
    src.fill(st);
    CHECK(st.depthMs > 250);

    std::vector<float> tape;
    std::vector<float> q(480);
    std::fill(q.begin(), q.end(), 0.0f);
    src.render(q.data(), q.size());
    tape.insert(tape.end(), q.begin(), q.end());

    src.fill(st);
    CHECK(st.depthMs >= 50 && st.depthMs <= 70);

    for (int i = 0; i < 40; ++i) {
        if (i % 2 == 0) {
            src.push(pcmFrame(48000, 1, ts, 960, tone, 0.5), now);
            ts += 960;
            now += 20;
        }
        std::fill(q.begin(), q.end(), 0.0f);
        src.render(q.data(), q.size());
        tape.insert(tape.end(), q.begin(), q.end());
    }

    src.fill(st);
    CHECK(st.underruns == 0);
    CHECK(st.depthMs >= 40 && st.depthMs <= 90);
    CHECK(peakOf(tape) > 0.4);
    CHECK(biggestStep(tape, 400) < 0.15);
}

void caseMicThreads() {
    g_case = "mic-threads";
    MicConfig cfg;
    cfg.sink.kind = SinkKind::Null;
    cfg.micName.clear();
    cfg.maxSources = 4;
    cfg.mix.format.rate = 48000;
    cfg.mix.format.channels = 2;

    std::shared_ptr<VirtualMic> mic = VirtualMic::start(cfg);
    CHECK(mic != nullptr);
    if (!mic)
        return;

    std::atomic<bool> go{true};
    std::vector<std::thread> crew;
    for (int k = 0; k < 3; ++k) {
        crew.emplace_back([&mic, &go, k]() {
            Tone t;
            t.set(300.0 + 100.0 * k, 48000);
            uint32_t ts = 0;
            auto begun = std::chrono::steady_clock::now();
            for (int i = 0; i < 30 && go.load(); ++i) {
                mic->feed(static_cast<VirtualMic::SourceId>(k + 1),
                          pcmFrame(48000, 1, ts, 960, t, 0.3));
                ts += 960;
                std::this_thread::sleep_until(begun + std::chrono::milliseconds(20 * (i + 1)));
            }
        });
    }
    crew.emplace_back([&mic, &go]() {
        while (go.load()) {
            MicStats st = mic->stats();
            (void)st.quanta;
            for (VirtualMic::SourceId id : mic->sources()) {
                SourceStats one;
                mic->sourceStats(id, one);
                mic->setGain(id, 0.8f);
                mic->setMute(id, false);
            }
            mic->setMaster(0.9f);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(650));
    go.store(false);
    for (std::thread& t : crew)
        t.join();

    MicStats st = mic->stats();
    CHECK(st.sources == 3);
    CHECK(st.quanta > 10);
    CHECK(st.master == 0.9f);
    for (VirtualMic::SourceId id : mic->sources()) {
        SourceStats one;
        CHECK(mic->sourceStats(id, one));
        CHECK(one.decodeErrors == 0);
        CHECK(one.framesIn > 10);
    }
    mic->stop();
}

void caseMicWasapi() {
    g_case = "mic-wasapi";
    std::vector<DeviceInfo> outs = devices(false);
    if (outs.empty()) {
        std::printf("  skip [mic-wasapi] no render endpoint\n");
        return;
    }

    MicConfig cfg;
    cfg.sink.kind = SinkKind::Wasapi;
    cfg.sink.preferVirtual = true;
    cfg.micName = "myaudio-test-wasapi";
    cfg.mix.format.rate = 44100;
    cfg.mix.format.channels = 1;

    std::shared_ptr<VirtualMic> mic = VirtualMic::start(cfg);
    CHECK(mic != nullptr);
    if (!mic)
        return;

    Format fmt = mic->format();
    CHECK(fmt.rate >= 8000 && fmt.rate <= 192000);
    CHECK(fmt.channels >= 1);
    CHECK(!mic->device().empty());

    for (const DeviceInfo& d : outs) {
        if (d.name != mic->device())
            continue;
        CHECK(fmt.rate == d.format.rate);
        CHECK(fmt.channels == d.format.channels);
    }

    Tone tone;
    tone.set(400.0, 48000);
    uint32_t ts = 0;
    auto begun = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) {
        mic->feed(21, pcmFrame(48000, 1, ts, 960, tone, 0.05));
        ts += 960;
        std::this_thread::sleep_until(begun + std::chrono::milliseconds(20 * (i + 1)));
    }

    MicStats st = mic->stats();
    CHECK(st.running);
    CHECK(st.sinkFrames > 5000);
    CHECK(st.quanta > 5);
    SourceStats one;
    CHECK(mic->sourceStats(21, one));
    CHECK(one.underruns <= 3);
    CHECK(one.decodeErrors == 0);
    mic->stop();
}

}

int main() {
    caseResampleIdentity();
    caseResampleRates();
    caseResampleAlias();
    caseResampleDrift();
    caseResampleSnr();
    caseRemap();
    caseRing();
    caseDecodePcm();
    caseDecodeOpus();
    caseSourceSteady();
    caseSourceGap();
    casePaceDrift();
    caseSourceControls();
    caseBus();
    caseShare();
    caseShareReuse();
    caseRegistry();
    caseDevices();
    caseFadeSeam();
    caseSourceFlood();
    caseSourceResync();
    caseSourceStall();
    caseSourceSwitch();
    caseSourceRestart();
    caseSourceRetarget();
    caseMicNull();
    caseMicThreads();
    caseMicShared();
    caseMicWasapi();

    std::printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed ? 1 : 0;
}
