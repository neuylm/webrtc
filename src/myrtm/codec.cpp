#include "myrtm/codec.hpp"

#include "bytes.hpp"

namespace myrtm {
namespace {

const uint32_t kRateTable[] = {8000, 12000, 16000, 24000, 32000, 44100, 48000, 96000};

int rateIndex(uint32_t rate) {
    for (int i = 0; i < 8; ++i) {
        if (kRateTable[i] == rate)
            return i;
    }
    return 15;
}

uint16_t clampAbs(int32_t v) {
    if (v < 0)
        return 0;
    if (v > 65535)
        return 65535;
    return static_cast<uint16_t>(v);
}

int16_t clampRel(int32_t v) {
    if (v < -32768)
        return -32768;
    if (v > 32767)
        return 32767;
    return static_cast<int16_t>(v);
}

}

void encodeInputBatch(const InputEvent* ev, size_t count, std::vector<uint8_t>& out) {
    if (count > kMaxBatch)
        count = kMaxBatch;

    Writer w(out);
    uint32_t base = count ? ev[0].timeMs : 0;
    w.u32(base);
    w.u8(static_cast<uint8_t>(count));

    for (size_t i = 0; i < count; ++i) {
        const InputEvent& e = ev[i];
        uint32_t delta = e.timeMs > base ? e.timeMs - base : 0;
        w.u16(delta > 0xffff ? 0xffff : static_cast<uint16_t>(delta));
        w.u8(static_cast<uint8_t>(e.kind));

        switch (e.kind) {
        case InputKind::KeyDown:
        case InputKind::KeyUp:
            w.u16(e.code);
            w.u8(e.flags);
            break;
        case InputKind::MouseMove:
            w.u8(e.flags);
            if (e.flags & kFlagAbsolute) {
                w.u16(clampAbs(e.x));
                w.u16(clampAbs(e.y));
            } else {
                w.i16(clampRel(e.x));
                w.i16(clampRel(e.y));
            }
            break;
        case InputKind::MouseDown:
        case InputKind::MouseUp:
            w.u8(static_cast<uint8_t>(e.code));
            break;
        case InputKind::Wheel:
            w.i16(e.wheel);
            w.u8(e.flags);
            break;
        }
    }
}

bool decodeInputBatch(const uint8_t* p, size_t n, std::vector<InputEvent>& out) {
    Reader r(p, n);
    uint32_t base = r.u32();
    uint8_t count = r.u8();
    if (r.bad())
        return false;

    out.clear();
    out.reserve(count);

    for (uint8_t i = 0; i < count; ++i) {
        InputEvent e;
        uint16_t delta = r.u16();
        uint8_t kind = r.u8();
        if (r.bad())
            return false;
        if (kind < static_cast<uint8_t>(InputKind::KeyDown) ||
            kind > static_cast<uint8_t>(InputKind::Wheel))
            return false;

        e.kind = static_cast<InputKind>(kind);
        e.timeMs = base + delta;

        switch (e.kind) {
        case InputKind::KeyDown:
        case InputKind::KeyUp:
            e.code = r.u16();
            e.flags = r.u8();
            break;
        case InputKind::MouseMove:
            e.flags = r.u8();
            if (e.flags & kFlagAbsolute) {
                e.x = r.u16();
                e.y = r.u16();
            } else {
                e.x = r.i16();
                e.y = r.i16();
            }
            break;
        case InputKind::MouseDown:
        case InputKind::MouseUp:
            e.code = r.u8();
            break;
        case InputKind::Wheel:
            e.wheel = r.i16();
            e.flags = r.u8();
            break;
        }
        if (r.bad())
            return false;
        out.push_back(e);
    }
    return true;
}

void encodeAudioFrame(const AudioFrame& f, std::vector<uint8_t>& out) {
    Writer w(out);
    int idx = rateIndex(f.sampleRate);
    uint8_t channels = f.channels ? f.channels : 1;
    if (channels > 15)
        channels = 15;

    w.u8(static_cast<uint8_t>(f.codec));
    w.u8(static_cast<uint8_t>((idx << 4) | channels));
    w.u32(f.timestamp);
    if (idx == 15)
        w.u32(f.sampleRate);
    if (!f.payload.empty())
        w.bytes(f.payload.data(), f.payload.size());
}

bool decodeAudioFrame(const uint8_t* p, size_t n, AudioFrame& out) {
    Reader r(p, n);
    uint8_t codec = r.u8();
    uint8_t format = r.u8();
    out.timestamp = r.u32();
    if (r.bad())
        return false;
    if (codec > static_cast<uint8_t>(AudioCodec::Opus))
        return false;

    int idx = (format >> 4) & 0x0f;
    out.codec = static_cast<AudioCodec>(codec);
    out.channels = format & 0x0f;
    if (out.channels == 0)
        return false;

    if (idx == 15) {
        out.sampleRate = r.u32();
        if (r.bad())
            return false;
    } else if (idx < 8) {
        out.sampleRate = kRateTable[idx];
    } else {
        return false;
    }

    size_t rest = r.left();
    out.payload.clear();
    if (rest) {
        const uint8_t* body = r.take(rest);
        out.payload.assign(body, body + rest);
    }
    return true;
}

}
