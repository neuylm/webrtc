#include "myrtm/endpoint.hpp"

#include "aes.hpp"
#include "channel.hpp"
#include "crypto.hpp"
#include "socket.hpp"
#include "stun.hpp"
#include "util.hpp"
#include "wire.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace myrtm;

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

std::vector<uint8_t> fromHex(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            return c - 'A' + 10;
        };
        out.push_back(static_cast<uint8_t>((nib(s[i]) << 4) | nib(s[i + 1])));
    }
    return out;
}

std::string toHex(const uint8_t* p, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(digits[p[i] >> 4]);
        s.push_back(digits[p[i] & 0x0f]);
    }
    return s;
}

bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
    for (int spent = 0; spent < timeoutMs; spent += 10) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

uint32_t percentile(std::vector<uint32_t> v, int pct) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    size_t idx = (v.size() - 1) * static_cast<size_t>(pct) / 100;
    return v[idx];
}

class Relay {
public:
    Relay(uint16_t hostPort, int lossPct, int jitterMs, unsigned seed)
        : lossPct_(lossPct), jitterMs_(jitterMs), rng_(seed) {
        ok_ = sock_.bindTo("127.0.0.1", 0) && resolveAddr("127.0.0.1", hostPort, host_);
        if (ok_) {
            running_.store(true);
            worker_ = std::thread([this] { run(); });
        }
    }

    ~Relay() { stop(); }

    bool ok() const { return ok_; }
    uint16_t port() const { return sock_.localPort(); }

    void stop() {
        if (running_.exchange(false) && worker_.joinable())
            worker_.join();
    }

private:
    struct Pending {
        uint32_t due = 0;
        SockAddr to;
        std::vector<uint8_t> data;
    };

    void run() {
        std::vector<uint8_t> buf(2048);
        std::uniform_int_distribution<int> roll(0, 99);
        while (running_.load()) {
            SockAddr from;
            int n = sock_.recvFrom(buf.data(), buf.size(), from, 2);
            uint32_t now = nowMs();

            if (n > 0) {
                bool fromHost = from.sameAs(host_);
                if (!fromHost)
                    viewer_ = from;
                SockAddr to = fromHost ? viewer_ : host_;
                if (to.valid() && roll(rng_) >= lossPct_) {
                    Pending p;
                    p.due = now + (jitterMs_ > 0
                                       ? static_cast<uint32_t>(rng_() % static_cast<unsigned>(jitterMs_))
                                       : 0);
                    p.to = to;
                    p.data.assign(buf.begin(), buf.begin() + n);
                    queue_.push_back(std::move(p));
                }
            }

            for (auto it = queue_.begin(); it != queue_.end();) {
                if (seqDiff(now, it->due) >= 0) {
                    sock_.sendTo(it->data.data(), it->data.size(), it->to);
                    it = queue_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    UdpSocket sock_;
    SockAddr host_;
    SockAddr viewer_;
    int lossPct_;
    int jitterMs_;
    std::mt19937 rng_;
    std::vector<Pending> queue_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    bool ok_ = false;
};

struct Sink {
    std::mutex mtx;
    std::vector<InputEvent> inputs;
    std::vector<AudioFrame> audio;
    std::vector<std::string> raw;
    std::atomic<int> connects{0};
    std::atomic<int> closes{0};

    void attach(const std::shared_ptr<Endpoint>& ep) {
        ep->onPeer([this](Endpoint::PeerId, PeerEvent e) {
            if (e == PeerEvent::Connected) {
                connects++;
            } else {
                closes++;
            }
        });
        ep->onInput([this](Endpoint::PeerId, const InputEvent& ev) {
            std::lock_guard<std::mutex> lk(mtx);
            inputs.push_back(ev);
        });
        ep->onAudio([this](Endpoint::PeerId, const AudioFrame& f) {
            std::lock_guard<std::mutex> lk(mtx);
            audio.push_back(f);
        });
        ep->onControl([this](Endpoint::PeerId, const uint8_t* p, size_t n) {
            std::lock_guard<std::mutex> lk(mtx);
            raw.emplace_back(reinterpret_cast<const char*>(p), n);
        });
    }

    size_t inputCount() {
        std::lock_guard<std::mutex> lk(mtx);
        return inputs.size();
    }

    size_t audioCount() {
        std::lock_guard<std::mutex> lk(mtx);
        return audio.size();
    }
};

const char* kKey = "rtm-test-key";

bool startHost(Config cfg, Sink* sink, std::shared_ptr<Endpoint>& host) {
    cfg.bindAddress = "127.0.0.1";
    if (cfg.psk.empty())
        cfg.psk = kKey;
    host = Endpoint::listen(cfg);
    if (!host)
        return false;
    if (sink)
        sink->attach(host);
    return true;
}

bool dial(Config cfg, uint16_t port, std::shared_ptr<Endpoint>& viewer) {
    cfg.bindAddress = "127.0.0.1";
    if (cfg.psk.empty())
        cfg.psk = kKey;
    viewer = Endpoint::connect(cfg, "127.0.0.1", port);
    return viewer && viewer->waitConnected(4000);
}

bool sniffHello(UdpSocket& sock, const AeadKey& hs, SockAddr& from, HelloBody& hb) {
    std::vector<uint8_t> buf(kMaxDatagram);
    for (int i = 0; i < 100; ++i) {
        int n = sock.recvFrom(buf.data(), buf.size(), from, 20);
        if (n <= 0)
            continue;
        Header h;
        if (!readHeader(buf.data(), static_cast<size_t>(n), h) || h.type != PacketType::Hello)
            continue;
        std::vector<uint8_t> plain;
        if (!hs.open(h.counter, buf.data(), kHeaderSize, buf.data() + kHeaderSize,
                     static_cast<size_t>(n) - kHeaderSize, plain))
            continue;
        Reader r(plain.data(), plain.size());
        if (readHello(r, hb))
            return true;
    }
    return false;
}

void replyWelcome(UdpSocket& sock, const AeadKey& hs, const SockAddr& to, uint32_t session,
                  const uint8_t echo[kNonceSize], uint64_t stamp) {
    WelcomeBody wb;
    wb.session = session;
    wb.stamp = stamp;
    randomBytes(wb.nonce, kNonceSize);
    std::memcpy(wb.echo, echo, kNonceSize);

    std::vector<uint8_t> body;
    writeWelcome(body, wb);

    Header h;
    h.type = PacketType::Welcome;
    h.session = session;
    h.counter = randomU64() | 1;

    std::vector<uint8_t> pkt;
    writeHeader(pkt, h);
    hs.seal(h.counter, pkt, kHeaderSize, body.data(), body.size());
    sock.sendTo(pkt.data(), pkt.size(), to);
}

void buildBindingSuccess(const uint8_t* txid, const SockAddr& mapped,
                         std::vector<uint8_t>& out) {
    const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(&mapped.sa);
    uint16_t port = ntohs(sin->sin_port);
    uint8_t addr[4];
    std::memcpy(addr, &sin->sin_addr, 4);

    Writer w(out);
    w.u16(0x0101);
    w.u16(12);
    w.u32(kStunCookie);
    w.bytes(txid, kStunTxidSize);
    w.u16(0x0020);
    w.u16(8);
    w.u8(0);
    w.u8(0x01);
    w.u16(uint16_t(port ^ (kStunCookie >> 16)));
    w.u8(uint8_t(addr[0] ^ 0x21));
    w.u8(uint8_t(addr[1] ^ 0x12));
    w.u8(uint8_t(addr[2] ^ 0xa4));
    w.u8(uint8_t(addr[3] ^ 0x42));
}

class StunServer {
public:
    StunServer() {
        ok_ = sock_.bindTo("127.0.0.1", 0);
        if (!ok_)
            return;
        running_.store(true);
        worker_ = std::thread([this] { run(); });
    }

    ~StunServer() { stop(); }

    bool ok() const { return ok_; }
    uint16_t port() const { return sock_.localPort(); }

    void stop() {
        if (running_.exchange(false) && worker_.joinable())
            worker_.join();
    }

private:
    void run() {
        std::vector<uint8_t> buf(kMaxDatagram);
        while (running_.load()) {
            SockAddr from;
            int n = sock_.recvFrom(buf.data(), buf.size(), from, 20);
            if (n <= 0 || !isStunMessage(buf.data(), static_cast<size_t>(n)))
                continue;
            std::vector<uint8_t> reply;
            buildBindingSuccess(buf.data() + 8, from, reply);
            sock_.sendTo(reply.data(), reply.size(), from);
        }
    }

    UdpSocket sock_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    bool ok_ = false;
};

class Doorman {
public:
    Doorman(const SockAddr& host) : host_(host) {
        ok_ = sock_.bindTo("127.0.0.1", 0);
        if (!ok_)
            return;
        running_.store(true);
        worker_ = std::thread([this] { run(); });
    }

    ~Doorman() { stop(); }

    bool ok() const { return ok_; }
    uint16_t port() const { return sock_.localPort(); }
    int blocked() const { return blocked_.load(); }

    void stop() {
        if (running_.exchange(false) && worker_.joinable())
            worker_.join();
    }

private:
    void run() {
        std::vector<uint8_t> buf(kMaxDatagram);
        while (running_.load()) {
            SockAddr from;
            int n = sock_.recvFrom(buf.data(), buf.size(), from, 5);
            if (n <= 0)
                continue;

            if (from.sameAs(host_)) {
                open_ = true;
                if (outside_.valid())
                    sock_.sendTo(buf.data(), static_cast<size_t>(n), outside_);
                continue;
            }

            outside_ = from;
            if (!open_) {
                blocked_++;
                continue;
            }
            sock_.sendTo(buf.data(), static_cast<size_t>(n), host_);
        }
    }

    UdpSocket sock_;
    SockAddr host_;
    SockAddr outside_;
    bool open_ = false;
    std::atomic<int> blocked_{0};
    std::atomic<bool> running_{false};
    std::thread worker_;
    bool ok_ = false;
};

void testAes() {
    g_case = "aes";
    auto key = fromHex("000102030405060708090a0b0c0d0e0f");
    auto plain = fromHex("00112233445566778899aabbccddeeff");
    Aes128 aes;
    aes.setKey(key.data());
    uint8_t out[16];
    aes.encrypt(plain.data(), out);
    CHECK(toHex(out, 16) == "69c4e0d86a7b0430d8cdb78070b4c55a");
}

void testCmac() {
    g_case = "cmac";
    auto key = fromHex("2b7e151628aed2a6abf7158809cf4f3c");
    uint8_t mac[16];

    aesCmac(key.data(), nullptr, 0, mac);
    CHECK(toHex(mac, 16) == "bb1d6929e95937287fa37d129b756746");

    auto m16 = fromHex("6bc1bee22e409f96e93d7e117393172a");
    aesCmac(key.data(), m16.data(), m16.size(), mac);
    CHECK(toHex(mac, 16) == "070a16b46b4d4144f79bdd9dd04a287c");

    auto m40 = fromHex("6bc1bee22e409f96e93d7e117393172a"
                       "ae2d8a571e03ac9c9eb76fac45af8e51"
                       "30c81c46a35ce411");
    aesCmac(key.data(), m40.data(), m40.size(), mac);
    CHECK(toHex(mac, 16) == "dfa66747de9ae63030ca32611497c827");
}

void testGcm() {
    g_case = "gcm";
    AeadKey k;

    std::vector<uint8_t> packet;
    k.seal(0, packet, 0, nullptr, 0);
    CHECK(toHex(packet.data(), packet.size()) == "58e2fccefa7e3061367f1d57a4e7455a");

    std::vector<uint8_t> zeros(16, 0);
    packet.clear();
    k.seal(0, packet, 0, zeros.data(), zeros.size());
    CHECK(toHex(packet.data(), packet.size()) ==
          "0388dace60b6a392f328c2b971b2fe78ab6e47d42cec13bdf53a67b21257bddf");

    std::vector<uint8_t> plain;
    CHECK(k.open(0, nullptr, 0, packet.data(), packet.size(), plain));
    CHECK(plain == zeros);

    CHECK(!k.open(1, nullptr, 0, packet.data(), packet.size(), plain));

    packet[2] ^= 0x40;
    CHECK(!k.open(0, nullptr, 0, packet.data(), packet.size(), plain));
}

void testAeadWithHeader() {
    g_case = "aead-header";
    AeadKey k = handshakeKey("a shared secret");

    Header h;
    h.type = PacketType::Data;
    h.session = 0xdeadbeef;
    h.counter = 42;

    std::string text = "keyboard and mouse";
    std::vector<uint8_t> packet;
    writeHeader(packet, h);
    k.seal(h.counter, packet, kHeaderSize,
             reinterpret_cast<const uint8_t*>(text.data()), text.size());
    CHECK(packet.size() == kHeaderSize + text.size() + kTagSize);

    Header back;
    CHECK(readHeader(packet.data(), packet.size(), back));
    CHECK(back.session == h.session && back.counter == h.counter);

    std::vector<uint8_t> plain;
    CHECK(k.open(back.counter, packet.data(), kHeaderSize, packet.data() + kHeaderSize,
                   packet.size() - kHeaderSize, plain));
    CHECK(std::string(plain.begin(), plain.end()) == text);

    packet[5] ^= 0x08;
    CHECK(!k.open(back.counter, packet.data(), kHeaderSize, packet.data() + kHeaderSize,
                    packet.size() - kHeaderSize, plain));
}

std::string fingerprint(const AeadKey& k) {
    static const uint8_t probe[8] = {'m', 'y', 'r', 't', 'm', 'k', 'e', 'y'};
    std::vector<uint8_t> packet;
    k.seal(7, packet, 0, probe, sizeof(probe));
    return toHex(packet.data(), packet.size());
}

void testKeys() {
    g_case = "keys";
    uint8_t cn[kNonceSize], sn[kNonceSize];
    randomBytes(cn, sizeof(cn));
    randomBytes(sn, sizeof(sn));

    SessionKeys client, server;
    deriveSessionKeys("hunter2", cn, sn, false, client);
    deriveSessionKeys("hunter2", cn, sn, true, server);
    CHECK(fingerprint(client.tx) == fingerprint(server.rx));
    CHECK(fingerprint(client.rx) == fingerprint(server.tx));
    CHECK(fingerprint(client.tx) != fingerprint(client.rx));

    SessionKeys other;
    deriveSessionKeys("hunter3", cn, sn, false, other);
    CHECK(fingerprint(client.tx) != fingerprint(other.tx));

    uint8_t cn2[kNonceSize];
    randomBytes(cn2, sizeof(cn2));
    SessionKeys reroll;
    deriveSessionKeys("hunter2", cn2, sn, false, reroll);
    CHECK(fingerprint(client.tx) != fingerprint(reroll.tx));

    CHECK(fingerprint(handshakeKey("hunter2")) == fingerprint(handshakeKey("hunter2")));
    CHECK(fingerprint(handshakeKey("hunter2")) != fingerprint(handshakeKey("hunter3")));
}

void testReplay() {
    g_case = "replay";
    ReplayWindow w;
    CHECK(!w.accept(0));
    CHECK(w.accept(1));
    CHECK(!w.accept(1));
    CHECK(w.accept(5));
    CHECK(w.accept(4));
    CHECK(!w.accept(4));
    CHECK(w.accept(200));
    CHECK(!w.accept(100));
    CHECK(w.accept(199));
    CHECK(!w.accept(199));
    CHECK(w.accept(137));
    CHECK(!w.accept(136));
}

void testInputCodec() {
    g_case = "input-codec";
    std::vector<InputEvent> src;

    InputEvent e;
    e.kind = InputKind::MouseMove;
    e.flags = kFlagAbsolute;
    e.x = 30000;
    e.y = 41000;
    e.timeMs = 100000;
    src.push_back(e);

    e = InputEvent();
    e.kind = InputKind::MouseMove;
    e.x = -700;
    e.y = 250;
    e.timeMs = 100005;
    src.push_back(e);

    e = InputEvent();
    e.kind = InputKind::KeyDown;
    e.code = 0x1b;
    e.flags = kFlagExtended;
    e.timeMs = 100010;
    src.push_back(e);

    e = InputEvent();
    e.kind = InputKind::MouseDown;
    e.code = 2;
    e.timeMs = 100012;
    src.push_back(e);

    e = InputEvent();
    e.kind = InputKind::Wheel;
    e.wheel = -240;
    e.flags = kFlagHorizontal;
    e.timeMs = 100020;
    src.push_back(e);

    std::vector<uint8_t> buf;
    encodeInputBatch(src.data(), src.size(), buf);

    std::vector<InputEvent> back;
    CHECK(decodeInputBatch(buf.data(), buf.size(), back));
    CHECK(back.size() == src.size());
    if (back.size() == src.size()) {
        for (size_t i = 0; i < src.size(); ++i) {
            CHECK(back[i].kind == src[i].kind);
            CHECK(back[i].flags == src[i].flags);
            CHECK(back[i].code == src[i].code);
            CHECK(back[i].x == src[i].x);
            CHECK(back[i].y == src[i].y);
            CHECK(back[i].wheel == src[i].wheel);
            CHECK(back[i].timeMs == src[i].timeMs);
        }
    }

    CHECK(!decodeInputBatch(buf.data(), buf.size() / 2, back));
}

void testAudioCodec() {
    g_case = "audio-codec";
    AudioFrame f;
    f.codec = AudioCodec::Opus;
    f.channels = 2;
    f.sampleRate = 48000;
    f.timestamp = 123456;
    f.payload = {1, 2, 3, 4, 5, 6, 7};

    std::vector<uint8_t> buf;
    encodeAudioFrame(f, buf);
    CHECK(buf.size() == 6 + f.payload.size());

    AudioFrame back;
    CHECK(decodeAudioFrame(buf.data(), buf.size(), back));
    CHECK(back.codec == f.codec);
    CHECK(back.channels == f.channels);
    CHECK(back.sampleRate == f.sampleRate);
    CHECK(back.timestamp == f.timestamp);
    CHECK(back.payload == f.payload);

    f.sampleRate = 22050;
    buf.clear();
    encodeAudioFrame(f, buf);
    CHECK(decodeAudioFrame(buf.data(), buf.size(), back));
    CHECK(back.sampleRate == 22050);

    CHECK(!decodeAudioFrame(buf.data(), 3, back));
}

void testHeaderParse() {
    g_case = "header";
    Header h;
    h.type = PacketType::Ack;
    h.session = 0x11223344;
    h.counter = 0x8899aabbccddeeffULL;

    std::vector<uint8_t> buf;
    writeHeader(buf, h);
    CHECK(buf.size() == kHeaderSize);
    buf.resize(kHeaderSize + kTagSize);

    Header back;
    CHECK(readHeader(buf.data(), buf.size(), back));
    CHECK(back.type == h.type);
    CHECK(back.session == h.session);
    CHECK(back.counter == h.counter);

    buf[0] = 0x00;
    CHECK(!readHeader(buf.data(), buf.size(), back));
    buf[0] = kMagic0;
    buf[3] = 99;
    CHECK(!readHeader(buf.data(), buf.size(), back));
    CHECK(!readHeader(buf.data(), kHeaderSize, back));
}

void testAckRoundTrip() {
    g_case = "ack";
    AckBody a;
    a.channel = kChanAudio;
    a.una = 4096;
    a.bitmap = 0xa5a5a5a5;
    a.echoTs = 777;
    a.delay = 12;

    std::vector<uint8_t> buf;
    writeAck(buf, a);
    CHECK(buf.size() == kAckBodySize);

    Reader r(buf.data(), buf.size());
    AckBody back;
    CHECK(readAck(r, back));
    CHECK(back.channel == a.channel);
    CHECK(back.una == a.una);
    CHECK(back.bitmap == a.bitmap);
    CHECK(back.echoTs == a.echoTs);
    CHECK(back.delay == a.delay);
}

void testLoopback() {
    g_case = "loopback";
    Config hostCfg;
    hostCfg.psk = kKey;
    hostCfg.bindAddress = "127.0.0.1";

    auto host = Endpoint::listen(hostCfg);
    CHECK(host != nullptr);
    if (!host)
        return;

    Sink sink;
    sink.attach(host);

    Config viewCfg;
    viewCfg.psk = kKey;
    viewCfg.bindAddress = "127.0.0.1";

    auto viewer = Endpoint::connect(viewCfg, "127.0.0.1", host->localPort());
    CHECK(viewer != nullptr);
    if (!viewer)
        return;

    uint32_t handshakeStart = nowMs();
    CHECK(viewer->waitConnected(3000));
    CHECK(nowMs() - handshakeStart < 500);
    CHECK(sink.connects.load() == 1);
    auto peer = viewer->localPeer();
    CHECK(peer != 0);

    const int kEvents = 200;
    bool allQueued = true;
    std::vector<InputEvent> batch;
    for (int i = 0; i < kEvents; ++i) {
        InputEvent ev;
        ev.kind = InputKind::MouseMove;
        ev.x = i;
        ev.y = -i;
        ev.timeMs = static_cast<uint32_t>(i);
        batch.push_back(ev);
        if (batch.size() < 8 && i + 1 < kEvents)
            continue;
        if (!viewer->sendInput(peer, batch.data(), batch.size()))
            allQueued = false;
        batch.clear();
    }
    CHECK(allQueued);

    const int kFrames = 60;
    std::vector<uint8_t> pcm(80, 0x5a);
    for (int i = 0; i < kFrames; ++i) {
        AudioFrame f;
        f.codec = AudioCodec::Pcm16;
        f.channels = 1;
        f.sampleRate = 48000;
        f.timestamp = static_cast<uint32_t>(i) * 960;
        f.payload = pcm;
        if (!viewer->sendAudio(peer, f))
            allQueued = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(allQueued);

    std::string hello = "hello from viewer";
    CHECK(viewer->sendControl(peer, hello.data(), hello.size()));

    CHECK(waitFor([&] { return sink.inputCount() >= kEvents && sink.audioCount() >= kFrames; },
                  5000));

    std::lock_guard<std::mutex> lk(sink.mtx);
    CHECK(sink.inputs.size() == kEvents);
    bool ordered = true;
    for (size_t i = 0; i < sink.inputs.size(); ++i) {
        if (sink.inputs[i].x != static_cast<int32_t>(i))
            ordered = false;
    }
    CHECK(ordered);
    CHECK(sink.audio.size() == kFrames);
    bool audioOrdered = true;
    for (size_t i = 0; i < sink.audio.size(); ++i) {
        if (sink.audio[i].timestamp != static_cast<uint32_t>(i) * 960)
            audioOrdered = false;
        if (sink.audio[i].payload != pcm)
            audioOrdered = false;
    }
    CHECK(audioOrdered);
    CHECK(sink.raw.size() == 1);
    CHECK(!sink.raw.empty() && sink.raw[0] == hello);
}

void testWrongKey() {
    g_case = "wrong-key";
    Config hostCfg;
    hostCfg.psk = kKey;
    hostCfg.bindAddress = "127.0.0.1";
    auto host = Endpoint::listen(hostCfg);
    CHECK(host != nullptr);
    if (!host)
        return;

    Config viewCfg;
    viewCfg.psk = "not-the-key";
    viewCfg.bindAddress = "127.0.0.1";
    viewCfg.handshakeTimeoutMs = 1000;

    auto viewer = Endpoint::connect(viewCfg, "127.0.0.1", host->localPort());
    CHECK(viewer != nullptr);
    if (!viewer)
        return;

    CHECK(!viewer->waitConnected(2000));
    CHECK(host->peers().empty());
    CHECK(viewer->localPeer() == 0);
}

void testFragmentation() {
    g_case = "fragmentation";
    Sink sink;
    std::shared_ptr<Endpoint> host, viewer;
    CHECK(startHost(Config(), &sink, host));
    if (!host)
        return;
    CHECK(dial(Config(), host->localPort(), viewer));
    if (!viewer)
        return;

    std::string blob;
    blob.reserve(64000);
    for (size_t i = 0; i < 64000; ++i)
        blob.push_back(static_cast<char>('a' + (i % 26)));

    CHECK(viewer->sendControl(viewer->localPeer(), blob.data(), blob.size()));
    CHECK(waitFor(
        [&] {
            std::lock_guard<std::mutex> lk(sink.mtx);
            return !sink.raw.empty();
        },
        8000));

    std::lock_guard<std::mutex> lk(sink.mtx);
    CHECK(sink.raw.size() == 1);
    CHECK(!sink.raw.empty() && sink.raw[0] == blob);
}

void testLossyLink() {
    g_case = "lossy-link";
    Sink sink;
    std::shared_ptr<Endpoint> host, viewer;
    CHECK(startHost(Config(), &sink, host));
    if (!host)
        return;

    Relay relay(host->localPort(), 12, 25, 0x5eedu);
    CHECK(relay.ok());
    if (!relay.ok())
        return;

    CHECK(dial(Config(), relay.port(), viewer));
    if (!viewer)
        return;

    auto peer = viewer->localPeer();
    const int kEvents = 200;
    const int kFrames = 100;
    std::vector<uint8_t> pcm(120, 0x11);

    for (int i = 0; i < kEvents; ++i) {
        InputEvent ev;
        ev.kind = InputKind::KeyDown;
        ev.code = static_cast<uint16_t>(i);
        ev.timeMs = nowMs();
        viewer->sendInput(peer, &ev, 1);

        if (i % 2 == 0) {
            AudioFrame f;
            f.codec = AudioCodec::Opus;
            f.channels = 1;
            f.sampleRate = 48000;
            f.timestamp = static_cast<uint32_t>(i / 2) * 960;
            f.payload = pcm;
            viewer->sendAudio(peer, f);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(waitFor([&] { return sink.inputCount() >= kEvents; }, 8000));

    std::lock_guard<std::mutex> lk(sink.mtx);
    CHECK(sink.inputs.size() == kEvents);
    bool ordered = true;
    for (size_t i = 0; i < sink.inputs.size(); ++i) {
        if (sink.inputs[i].code != static_cast<uint16_t>(i))
            ordered = false;
    }
    CHECK(ordered);

    CHECK(sink.audio.size() >= static_cast<size_t>(kFrames * 3 / 4));
    bool rising = true;
    for (size_t i = 1; i < sink.audio.size(); ++i) {
        if (sink.audio[i].timestamp <= sink.audio[i - 1].timestamp)
            rising = false;
    }
    CHECK(rising);

    Stats st;
    CHECK(viewer->stats(peer, st));
    std::printf("  [lossy] audio %zu/%d, ctl resent=%llu rtt=%ums\n", sink.audio.size(), kFrames,
                static_cast<unsigned long long>(st.control.packetsResent), st.control.rttMs);
}

void testChannelIsolation() {
    g_case = "isolation";
    std::shared_ptr<Endpoint> host, viewer;
    CHECK(startHost(Config(), nullptr, host));
    if (!host)
        return;

    std::mutex mtx;
    std::vector<uint32_t> latency;
    std::atomic<int> received{0};

    host->onInput([&](Endpoint::PeerId, const InputEvent& ev) {
        uint32_t now = nowMs();
        std::lock_guard<std::mutex> lk(mtx);
        latency.push_back(now > ev.timeMs ? now - ev.timeMs : 0);
        received++;
    });

    CHECK(dial(Config(), host->localPort(), viewer));
    if (!viewer)
        return;
    auto peer = viewer->localPeer();

    std::vector<uint8_t> chunk(800, 0x7f);
    for (int i = 0; i < 500; ++i) {
        AudioFrame f;
        f.codec = AudioCodec::Opus;
        f.channels = 1;
        f.sampleRate = 48000;
        f.timestamp = static_cast<uint32_t>(i) * 960;
        f.payload = chunk;
        viewer->sendAudio(peer, f);
    }

    const int kEvents = 50;
    for (int i = 0; i < kEvents; ++i) {
        InputEvent ev;
        ev.kind = InputKind::MouseMove;
        ev.x = i;
        ev.timeMs = nowMs();
        viewer->sendInput(peer, &ev, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    CHECK(waitFor([&] { return received.load() >= kEvents; }, 4000));

    Stats st;
    CHECK(viewer->stats(peer, st));
    CHECK(st.audio.packetsExpired > 100);
    CHECK(st.control.packetsSent >= kEvents);

    std::lock_guard<std::mutex> lk(mtx);
    CHECK(latency.size() == kEvents);
    uint32_t p95 = percentile(latency, 95);
    uint32_t worst = latency.empty() ? 0 : *std::max_element(latency.begin(), latency.end());
    std::printf("  [isolation] control p95=%ums max=%ums, audio expired=%llu sent=%llu\n", p95,
                worst, static_cast<unsigned long long>(st.audio.packetsExpired),
                static_cast<unsigned long long>(st.audio.packetsSent));
    CHECK(p95 < 150);
}

void testSendLimits() {
    g_case = "send-limits";
    Sink sink;
    std::shared_ptr<Endpoint> host, viewer;
    CHECK(startHost(Config(), &sink, host));
    if (!host)
        return;
    CHECK(dial(Config(), host->localPort(), viewer));
    if (!viewer)
        return;
    auto peer = viewer->localPeer();

    AudioFrame big;
    big.codec = AudioCodec::Pcm16;
    big.channels = 1;
    big.sampleRate = 48000;
    big.payload.assign(2000, 0x22);
    CHECK(!viewer->sendAudio(peer, big));

    AudioFrame ok = big;
    ok.payload.assign(1000, 0x22);
    ok.timestamp = 960;
    CHECK(viewer->sendAudio(peer, ok));
    CHECK(waitFor([&] { return sink.audioCount() >= 1; }, 2000));

    std::vector<InputEvent> many(300);
    for (size_t i = 0; i < many.size(); ++i) {
        many[i].kind = InputKind::MouseMove;
        many[i].x = static_cast<int32_t>(i);
    }
    CHECK(viewer->sendInput(peer, many.data(), many.size()));
    CHECK(waitFor([&] { return sink.inputCount() >= many.size(); }, 4000));

    std::lock_guard<std::mutex> lk(sink.mtx);
    CHECK(sink.inputs.size() == many.size());
    bool ordered = true;
    for (size_t i = 0; i < sink.inputs.size(); ++i) {
        if (sink.inputs[i].x != static_cast<int32_t>(i))
            ordered = false;
    }
    CHECK(ordered);
}

void testHelloReplay() {
    g_case = "hello-replay";
    Config cfg;
    cfg.psk = kKey;
    cfg.bindAddress = "127.0.0.1";

    auto host = Endpoint::listen(cfg);
    CHECK(host != nullptr);
    if (!host)
        return;

    SockAddr target;
    CHECK(resolveAddr("127.0.0.1", host->localPort(), target));

    AeadKey hs = handshakeKey(cfg.psk);
    HelloBody hb;
    hb.stamp = unixMs();
    randomBytes(hb.nonce, kNonceSize);

    std::vector<uint8_t> body;
    writeHello(body, hb);

    Header h;
    h.type = PacketType::Hello;
    h.session = 0;
    h.counter = randomU64() | 1;

    std::vector<uint8_t> packet;
    writeHeader(packet, h);
    hs.seal(h.counter, packet, kHeaderSize, body.data(), body.size());

    UdpSocket a, b, c;
    CHECK(a.bindTo("127.0.0.1", 0));
    CHECK(b.bindTo("127.0.0.1", 0));
    CHECK(c.bindTo("127.0.0.1", 0));
    CHECK(a.sendTo(packet.data(), packet.size(), target));
    CHECK(b.sendTo(packet.data(), packet.size(), target));
    CHECK(c.sendTo(packet.data(), packet.size(), target));

    CHECK(waitFor([&] { return !host->peers().empty(); }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(host->peers().size() == 1);
}

void testWelcomeForgery() {
    g_case = "welcome-forgery";
    Config cfg;
    cfg.psk = kKey;
    cfg.handshakeTimeoutMs = 20000;

    UdpSocket fake;
    CHECK(fake.bindTo("127.0.0.1", 0));

    std::shared_ptr<Endpoint> viewer;
    dial(cfg, fake.localPort(), viewer);
    CHECK(viewer != nullptr);
    if (!viewer)
        return;

    AeadKey hs = handshakeKey(cfg.psk);
    SockAddr from;
    HelloBody hb;
    CHECK(sniffHello(fake, hs, from, hb));
    if (!from.valid())
        return;

    const uint32_t session = 0x51ee7;
    uint8_t wrong[kNonceSize];
    randomBytes(wrong, kNonceSize);
    for (int i = 0; i < 4; ++i)
        replyWelcome(fake, hs, from, session, wrong, unixMs());
    CHECK(!viewer->waitConnected(800));

    for (int i = 0; i < 4; ++i)
        replyWelcome(fake, hs, from, session, hb.nonce, unixMs() - 120000);
    CHECK(!viewer->waitConnected(800));

    replyWelcome(fake, hs, from, session, hb.nonce, unixMs());
    CHECK(viewer->waitConnected(1500));
    CHECK(viewer->localPeer() == session);
}

void testLateHandler() {
    g_case = "late-handler";
    std::shared_ptr<Endpoint> host, viewer;
    CHECK(startHost(Config(), nullptr, host));
    if (!host)
        return;
    CHECK(dial(Config(), host->localPort(), viewer));
    if (!viewer)
        return;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::atomic<int> connects{0};
    host->onPeer([&](Endpoint::PeerId, PeerEvent e) {
        if (e == PeerEvent::Connected)
            connects++;
    });
    CHECK(waitFor([&] { return connects.load() == 1; }, 1000));
}

void testLateWelcome() {
    g_case = "late-welcome";
    Config cfg;
    cfg.psk = kKey;
    cfg.handshakeTimeoutMs = 600;

    UdpSocket fake;
    CHECK(fake.bindTo("127.0.0.1", 0));

    std::shared_ptr<Endpoint> viewer;
    dial(cfg, fake.localPort(), viewer);
    CHECK(viewer != nullptr);
    if (!viewer)
        return;

    std::atomic<int> connects{0};
    std::atomic<int> timeouts{0};
    viewer->onPeer([&](Endpoint::PeerId, PeerEvent e) {
        if (e == PeerEvent::Connected)
            connects++;
        else
            timeouts++;
    });

    AeadKey hs = handshakeKey(cfg.psk);
    SockAddr from;
    HelloBody hb;
    CHECK(sniffHello(fake, hs, from, hb));
    if (!from.valid())
        return;

    CHECK(waitFor([&] { return timeouts.load() > 0; }, 3000));

    for (int i = 0; i < 3; ++i)
        replyWelcome(fake, hs, from, 0xabcdef, hb.nonce, unixMs());

    CHECK(!viewer->waitConnected(800));
    CHECK(connects.load() == 0);
    CHECK(viewer->peers().empty());
}

void testAudioResync() {
    g_case = "audio-resync";
    Config cfg;
    cfg.audioDeadlineMs = 60;

    std::shared_ptr<Endpoint> host, viewer;
    CHECK(startHost(cfg, nullptr, host));
    if (!host)
        return;

    std::mutex mtx;
    std::vector<uint32_t> stamps;
    host->onAudio([&](Endpoint::PeerId, const AudioFrame& f) {
        std::lock_guard<std::mutex> lk(mtx);
        stamps.push_back(f.timestamp);
    });

    CHECK(dial(cfg, host->localPort(), viewer));
    if (!viewer)
        return;
    auto peer = viewer->localPeer();

    std::vector<uint8_t> chunk(900, 0x44);
    for (int i = 0; i < 700; ++i) {
        AudioFrame f;
        f.codec = AudioCodec::Opus;
        f.channels = 1;
        f.sampleRate = 48000;
        f.timestamp = static_cast<uint32_t>(i);
        f.payload = chunk;
        viewer->sendAudio(peer, f);
    }

    Stats st;
    CHECK(waitFor(
        [&] {
            Stats s;
            return viewer->stats(peer, s) && s.audio.packetsExpired > 600;
        },
        4000));
    CHECK(viewer->stats(peer, st));
    CHECK(st.audio.packetsExpired > 600);

    std::vector<uint8_t> small(80, 0x55);
    for (int i = 0; i < 20; ++i) {
        AudioFrame f;
        f.codec = AudioCodec::Opus;
        f.channels = 1;
        f.sampleRate = 48000;
        f.timestamp = 1000000 + static_cast<uint32_t>(i);
        f.payload = small;
        viewer->sendAudio(peer, f);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    CHECK(waitFor(
        [&] {
            std::lock_guard<std::mutex> lk(mtx);
            size_t late = 0;
            for (uint32_t t : stamps) {
                if (t >= 1000000)
                    late++;
            }
            return late >= 15;
        },
        4000));

    std::lock_guard<std::mutex> lk(mtx);
    size_t late = 0;
    for (uint32_t t : stamps) {
        if (t >= 1000000)
            late++;
    }
    std::printf("  [resync] expired=%llu, post-gap frames delivered=%zu/20\n",
                static_cast<unsigned long long>(st.audio.packetsExpired), late);
    CHECK(late >= 15);
}

void testIpv6() {
    g_case = "ipv6";
    Config cfg;
    cfg.psk = kKey;
    cfg.bindAddress = "::1";

    auto host = Endpoint::listen(cfg);
    CHECK(host != nullptr);
    if (!host)
        return;

    Sink sink;
    sink.attach(host);

    auto viewer = Endpoint::connect(cfg, "::1", host->localPort());
    CHECK(viewer != nullptr);
    if (!viewer)
        return;
    CHECK(viewer->waitConnected(3000));

    auto peer = viewer->localPeer();
    InputEvent ev;
    ev.kind = InputKind::KeyDown;
    ev.code = 0x42;
    CHECK(viewer->sendInput(peer, &ev, 1));

    AudioFrame f;
    f.codec = AudioCodec::Opus;
    f.channels = 1;
    f.sampleRate = 48000;
    f.payload.assign(64, 0x33);
    CHECK(viewer->sendAudio(peer, f));

    CHECK(waitFor([&] { return sink.inputCount() >= 1 && sink.audioCount() >= 1; }, 3000));

    std::lock_guard<std::mutex> lk(sink.mtx);
    CHECK(!sink.inputs.empty() && sink.inputs[0].code == 0x42);
    CHECK(!sink.audio.empty() && sink.audio[0].payload.size() == 64);
}

void testStunParse() {
    g_case = "stun-parse";
    uint8_t txid[kStunTxidSize];
    randomBytes(txid, sizeof(txid));

    std::vector<uint8_t> req;
    buildStunRequest(txid, req);
    CHECK(req.size() == kStunHeaderSize);
    CHECK(isStunMessage(req.data(), req.size()));

    SockAddr mapped;
    CHECK(resolveAddr("203.0.113.7", 51234, mapped));

    std::vector<uint8_t> reply;
    buildBindingSuccess(txid, mapped, reply);

    SockAddr back;
    CHECK(parseStunResponse(reply.data(), reply.size(), txid, back));
    CHECK(back.text() == "203.0.113.7:51234");

    uint8_t other[kStunTxidSize];
    randomBytes(other, sizeof(other));
    CHECK(!parseStunResponse(reply.data(), reply.size(), other, back));
    CHECK(!parseStunResponse(reply.data(), kStunHeaderSize, txid, back));

    Header h;
    h.type = PacketType::Data;
    h.session = 1;
    h.counter = 1;
    std::vector<uint8_t> rtm;
    writeHeader(rtm, h);
    rtm.resize(kHeaderSize + kTagSize);
    CHECK(!isStunMessage(rtm.data(), rtm.size()));

    SockAddr parsed;
    CHECK(parseEndpoint("127.0.0.1:9000", parsed));
    CHECK(parsed.text() == "127.0.0.1:9000");
    CHECK(parseEndpoint("[::1]:9000", parsed));
    CHECK(parsed.text() == "[::1]:9000");
    CHECK(!parseEndpoint("127.0.0.1", parsed));
    CHECK(!parseEndpoint("127.0.0.1:70000", parsed));
}

void testStunDiscovery() {
    g_case = "stun";
    StunServer stun;
    CHECK(stun.ok());
    if (!stun.ok())
        return;

    Config cfg;
    cfg.psk = kKey;
    cfg.bindAddress = "127.0.0.1";
    cfg.stunServers.push_back("127.0.0.1:" + std::to_string(stun.port()));

    auto ep = Endpoint::listen(cfg);
    CHECK(ep != nullptr);
    if (!ep)
        return;

    CHECK(ep->gatherPublicAddress(3000));
    std::string seen = ep->publicAddress();
    std::printf("  [stun] reflexive=%s\n", seen.c_str());
    CHECK(seen == "127.0.0.1:" + std::to_string(ep->localPort()));
}

void testNatPunch() {
    g_case = "nat-punch";
    Config cfg;
    cfg.psk = kKey;
    cfg.bindAddress = "127.0.0.1";
    cfg.handshakeTimeoutMs = 15000;

    auto host = Endpoint::listen(cfg);
    CHECK(host != nullptr);
    if (!host)
        return;

    Sink sink;
    sink.attach(host);

    SockAddr hostAddr;
    CHECK(resolveAddr("127.0.0.1", host->localPort(), hostAddr));

    Doorman nat(hostAddr);
    CHECK(nat.ok());
    if (!nat.ok())
        return;

    auto viewer = Endpoint::connect(cfg, "127.0.0.1", nat.port());
    CHECK(viewer != nullptr);
    if (!viewer)
        return;

    CHECK(!viewer->waitConnected(900));
    CHECK(nat.blocked() > 0);

    host->addRemoteCandidate("127.0.0.1:" + std::to_string(nat.port()));
    CHECK(viewer->waitConnected(6000));

    auto peer = viewer->localPeer();
    InputEvent ev;
    ev.kind = InputKind::KeyDown;
    ev.code = 0x37;
    CHECK(viewer->sendInput(peer, &ev, 1));
    CHECK(waitFor([&] { return sink.inputCount() >= 1; }, 3000));
    std::printf("  [nat-punch] blocked=%d before punch\n", nat.blocked());
}

void testDisconnect() {
    g_case = "disconnect";
    Sink sink;
    std::shared_ptr<Endpoint> host, viewer;
    CHECK(startHost(Config(), &sink, host));
    if (!host)
        return;
    CHECK(dial(Config(), host->localPort(), viewer));
    if (!viewer)
        return;
    CHECK(host->peers().size() == 1);

    viewer->disconnect(viewer->localPeer());
    CHECK(waitFor([&] { return sink.closes.load() > 0; }, 2000));
    CHECK(waitFor([&] { return host->peers().empty(); }, 2000));
}

}

int main() {
    testAes();
    testCmac();
    testGcm();
    testAeadWithHeader();
    testKeys();
    testReplay();
    testInputCodec();
    testAudioCodec();
    testHeaderParse();
    testAckRoundTrip();
    testLoopback();
    testWrongKey();
    testFragmentation();
    testLossyLink();
    testChannelIsolation();
    testSendLimits();
    testHelloReplay();
    testWelcomeForgery();
    testLateHandler();
    testLateWelcome();
    testAudioResync();
    testIpv6();
    testStunParse();
    testStunDiscovery();
    testNatPunch();
    testDisconnect();

    std::printf("checks: %d, failed: %d\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
