#include "session.hpp"

#include "util.hpp"

#include <algorithm>
#include <cstring>

namespace myrtm {
namespace {

constexpr uint32_t kHelloRetryMs = 250;
constexpr uint64_t kClockSkewMs = 30000;

size_t payloadLimit(uint16_t mtu) {
    size_t room = mtu > kPacketOverhead + 64 ? mtu - kPacketOverhead : 64;
    return room;
}

}

bool helloStampFresh(uint64_t stamp) {
    uint64_t now = unixMs();
    uint64_t gap = now > stamp ? now - stamp : stamp - now;
    return gap <= kClockSkewMs;
}

Session::Session(const Config& cfg, const AeadKey& hs, bool server, const SockAddr& peer, Sink sink)
    : cfg_(cfg), hs_(hs), server_(server), peer_(peer), sink_(std::move(sink)),
      sendCtl_(kChanControl, payloadLimit(cfg.mtu), cfg.controlRateBps, 0, true),
      sendAudio_(kChanAudio, payloadLimit(cfg.mtu), cfg.audioBitrate * 5 / 4,
                 cfg.audioDeadlineMs, false),
      recvCtl_(kChanControl, true, 0),
      recvAudio_(kChanAudio, false, cfg.audioJitterMs) {
    randomBytes(localNonce_, kNonceSize);
}

void Session::emit(PacketType type, const AeadKey& key, const uint8_t* payload, size_t len) {
    Header h;
    h.type = type;
    h.session = id_;
    if (type == PacketType::Hello || type == PacketType::Welcome)
        h.counter = randomU64() | 1;
    else
        h.counter = ++txCounter_;

    pkt_.clear();
    pkt_.reserve(kHeaderSize + len + kTagSize);
    writeHeader(pkt_, h);
    key.seal(h.counter, pkt_, kHeaderSize, payload, len);
    sink_(pkt_.data(), pkt_.size(), peer_);
}

void Session::emitData(const DataHeader& h, const uint8_t* p, size_t n) {
    body_.clear();
    body_.reserve(kDataHeaderSize + n);
    writeDataHeader(body_, h);
    body_.insert(body_.end(), p, p + n);
    emit(PacketType::Data, keys_.tx, body_.data(), body_.size());
}

void Session::startClient(uint32_t now) {
    startedAt_ = now;
    lastRecv_ = now;
    sendHello(now);
}

void Session::sendHello(uint32_t now) {
    HelloBody hb;
    hb.stamp = unixMs();
    std::memcpy(hb.nonce, localNonce_, kNonceSize);

    std::vector<uint8_t> payload;
    writeHello(payload, hb);
    emit(PacketType::Hello, hs_, payload.data(), payload.size());
    lastHello_ = now;
}

void Session::acceptClient(uint32_t id, const uint8_t nonce[kNonceSize], uint32_t now) {
    id_ = id;
    std::memcpy(remoteNonce_, nonce, kNonceSize);
    deriveSessionKeys(cfg_.psk, remoteNonce_, localNonce_, true, keys_);
    established_ = true;
    startedAt_ = now;
    lastRecv_ = now;
    lastPing_ = now;
    sendWelcome();
}

void Session::sendWelcome() {
    WelcomeBody wb;
    wb.session = id_;
    wb.stamp = unixMs();
    std::memcpy(wb.nonce, localNonce_, kNonceSize);
    std::memcpy(wb.echo, remoteNonce_, kNonceSize);

    std::vector<uint8_t> payload;
    writeWelcome(payload, wb);
    emit(PacketType::Welcome, hs_, payload.data(), payload.size());
}

void Session::sendPing(uint32_t now) {
    uint8_t ts[4] = {static_cast<uint8_t>(now >> 24), static_cast<uint8_t>(now >> 16),
                     static_cast<uint8_t>(now >> 8), static_cast<uint8_t>(now)};
    emit(PacketType::Ping, keys_.tx, ts, sizeof(ts));
    lastPing_ = now;
}

void Session::sendAcks(uint32_t now) {
    RecvChannel* chans[kChanCount] = {&recvCtl_, &recvAudio_};
    for (int i = 0; i < kChanCount; ++i) {
        if (!chans[i]->ackPending())
            continue;
        AckBody a = chans[i]->takeAck(now);
        std::vector<uint8_t> payload;
        payload.reserve(kAckBodySize);
        writeAck(payload, a);
        emit(PacketType::Ack, keys_.tx, payload.data(), payload.size());
    }
}

void Session::flushChannels(uint32_t now) {
    SendChannel::Emit emit = [this](const DataHeader& h, const uint8_t* p, size_t n) {
        emitData(h, p, n);
    };
    sendCtl_.flush(now, emit);
    sendAudio_.flush(now, emit);
}

void Session::drainRecv(uint32_t now, Deliveries& out) {
    RecvChannel* chans[kChanCount] = {&recvCtl_, &recvAudio_};
    for (int i = 0; i < kChanCount; ++i) {
        uint8_t ch = static_cast<uint8_t>(i);
        chans[i]->poll(now, [this, ch, &out](const uint8_t* p, size_t n) {
            Delivered d;
            d.peer = id_;
            d.channel = ch;
            d.data.assign(p, p + n);
            out.messages.push_back(std::move(d));
        });
    }
}

bool Session::sendMessage(uint8_t channel, const uint8_t* data, size_t len, uint32_t now) {
    if (!established_ || dead_)
        return false;
    SendChannel& c = channel == kChanAudio ? sendAudio_ : sendCtl_;
    if (!c.push(data, len, now))
        return false;
    flushChannels(now);
    return true;
}

void Session::shutdown() {
    if (established_ && !dead_)
        emit(PacketType::Bye, keys_.tx, nullptr, 0);
    dead_ = true;
}

void Session::onDatagram(const uint8_t* pkt, size_t len, const Header& h, const SockAddr& from,
                         uint32_t now, Deliveries& out) {
    if (dead_)
        return;

    const uint8_t* body = pkt + kHeaderSize;
    size_t bodyLen = len - kHeaderSize;

    if (!established_) {
        if (server_ || h.type != PacketType::Welcome)
            return;
        if (!hs_.open(h.counter, pkt, kHeaderSize, body, bodyLen, plain_)) {
            authFailures_++;
            return;
        }
        Reader r(plain_.data(), plain_.size());
        WelcomeBody wb;
        if (!readWelcome(r, wb) || wb.session == 0)
            return;
        if (!helloStampFresh(wb.stamp))
            return;
        if (std::memcmp(wb.echo, localNonce_, kNonceSize) != 0)
            return;

        id_ = wb.session;
        std::memcpy(remoteNonce_, wb.nonce, kNonceSize);
        deriveSessionKeys(cfg_.psk, localNonce_, remoteNonce_, false, keys_);
        established_ = true;
        lastRecv_ = now;
        lastPing_ = now;
        peer_ = from;
        out.events.emplace_back(id_, PeerEvent::Connected);
        return;
    }

    if (h.type == PacketType::Hello || h.type == PacketType::Welcome)
        return;

    if (!keys_.rx.open(h.counter, pkt, kHeaderSize, body, bodyLen, plain_)) {
        authFailures_++;
        return;
    }
    if (!replay_.accept(h.counter)) {
        replayDrops_++;
        return;
    }

    lastRecv_ = now;
    if (server_ && !peer_.sameAs(from))
        peer_ = from;

    Reader r(plain_.data(), plain_.size());
    switch (h.type) {
    case PacketType::Data: {
        DataHeader dh;
        if (!readDataHeader(r, dh))
            return;
        size_t rest = r.left();
        const uint8_t* payload = rest ? r.take(rest) : plain_.data();
        RecvChannel& c = dh.channel == kChanAudio ? recvAudio_ : recvCtl_;
        c.onData(dh, payload, rest, now);
        break;
    }
    case PacketType::Ack: {
        AckBody a;
        if (!readAck(r, a))
            return;
        SendChannel& c = a.channel == kChanAudio ? sendAudio_ : sendCtl_;
        c.onAck(a, now);
        break;
    }
    case PacketType::Ping: {
        uint32_t ts = r.u32();
        if (r.bad())
            return;
        uint8_t echo[4] = {static_cast<uint8_t>(ts >> 24), static_cast<uint8_t>(ts >> 16),
                           static_cast<uint8_t>(ts >> 8), static_cast<uint8_t>(ts)};
        emit(PacketType::Pong, keys_.tx, echo, sizeof(echo));
        break;
    }
    case PacketType::Pong: {
        uint32_t echo = r.u32();
        if (r.bad())
            return;
        int32_t sample = seqDiff(now, echo);
        if (sample >= 0 && sample < 60000) {
            linkRtt_ = linkRtt_ ? (linkRtt_ * 3 + static_cast<uint32_t>(sample)) / 4
                                : static_cast<uint32_t>(sample);
            uint32_t hold = std::max(cfg_.audioJitterMs, linkRtt_ * 2);
            if (cfg_.audioDeadlineMs)
                hold = std::min(hold, std::max(cfg_.audioDeadlineMs, cfg_.audioJitterMs));
            recvAudio_.setHold(hold);
        }
        break;
    }
    case PacketType::Bye:
        dead_ = true;
        out.events.emplace_back(id_, PeerEvent::Closed);
        return;
    default:
        return;
    }

    drainRecv(now, out);
    return;
}

void Session::update(uint32_t now, Deliveries& out) {
    if (dead_)
        return;

    if (!established_) {
        if (server_)
            return;
        if (seqDiff(now, startedAt_) > static_cast<int32_t>(cfg_.handshakeTimeoutMs)) {
            dead_ = true;
            out.events.emplace_back(id_, PeerEvent::TimedOut);
            return;
        }
        if (seqDiff(now, lastHello_) >= static_cast<int32_t>(kHelloRetryMs))
            sendHello(now);
        return;
    }

    if (seqDiff(now, lastRecv_) > static_cast<int32_t>(cfg_.idleTimeoutMs)) {
        dead_ = true;
        out.events.emplace_back(id_, PeerEvent::TimedOut);
        return;
    }

    drainRecv(now, out);
    sendAcks(now);
    flushChannels(now);

    if (seqDiff(now, lastPing_) >= static_cast<int32_t>(cfg_.pingIntervalMs))
        sendPing(now);
}

void Session::fillStats(Stats& s) const {
    sendCtl_.fillStats(s.control);
    recvCtl_.fillStats(s.control);
    sendAudio_.fillStats(s.audio);
    recvAudio_.fillStats(s.audio);
    s.linkRttMs = linkRtt_;
    s.authFailures = authFailures_;
    s.replayDrops = replayDrops_;
}

}
