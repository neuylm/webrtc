#include "myrtm/endpoint.hpp"

#include "session.hpp"
#include "socket.hpp"
#include "stun.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace myrtm {

namespace {

constexpr uint32_t kHelloCacheMs = 70000;
constexpr size_t kHelloCacheMax = 256;
constexpr int kDrainPerWake = 32;
constexpr uint32_t kPunchIntervalMs = 250;
constexpr uint32_t kPunchLifetimeMs = 20000;
constexpr size_t kMaxPunchTargets = 8;
constexpr uint32_t kStunRetryMs = 300;
constexpr int kStunAttempts = 4;

Config sanitize(const Config& in) {
    Config cfg = in;
    if (cfg.mtu < 256)
        cfg.mtu = 256;
    if (cfg.mtu > kMaxDatagram)
        cfg.mtu = uint16_t(kMaxDatagram);
    return cfg;
}

}

struct Endpoint::Impl {
    struct SeenHello {
        uint8_t nonce[kNonceSize];
        uint32_t at;
    };

    struct PunchTarget {
        SockAddr addr;
        uint32_t addedAt = 0;
        uint32_t lastSent = 0;
    };

    struct StunQuery {
        SockAddr server;
        uint8_t txid[kStunTxidSize] = {0};
        uint32_t lastSent = 0;
        int attempts = 0;
        bool done = false;
    };

    Config cfg;
    AeadKey hs;
    bool server = false;
    UdpSocket sock;

    std::unique_ptr<Session> client;
    std::unordered_map<uint32_t, std::unique_ptr<Session>> sessions;
    std::deque<SeenHello> seenHellos;
    std::vector<PunchTarget> punchTargets;
    std::vector<StunQuery> stunQueries;
    SockAddr reflexive;
    std::vector<std::pair<PeerId, PeerEvent>> pendingEvents;
    std::atomic<bool> buffered{false};

    mutable std::mutex mtx;
    std::condition_variable cv;
    std::thread worker;
    std::atomic<bool> running{false};

    PeerHandler peerCb;
    InputHandler inputCb;
    AudioHandler audioCb;
    ControlHandler controlCb;

    Session::Sink sink() {
        return [this](const uint8_t* p, size_t n, const SockAddr& to) {
            sock.sendTo(p, n, to);
        };
    }

    bool ready() const {
        if (server)
            return !sessions.empty();
        return client && (client->established() || client->dead());
    }

    Session* find(uint32_t peer) {
        if (!server) {
            if (client && client->established() && !client->dead() &&
                (peer == 0 || peer == client->id()))
                return client.get();
            return nullptr;
        }
        auto it = sessions.find(peer);
        return it == sessions.end() ? nullptr : it->second.get();
    }

    void start() {
        running.store(true);
        worker = std::thread([this] { run(); });
    }

    void run();
    void handle(const uint8_t* p, size_t n, const SockAddr& from, uint32_t now, Deliveries& out);
    void handleHello(const uint8_t* p, size_t n, const Header& h, const SockAddr& from,
                     uint32_t now, Deliveries& out);
    void handlePunch(const uint8_t* p, size_t n, const Header& h, const SockAddr& from,
                     uint32_t now);
    void handleStun(const uint8_t* p, size_t n, const SockAddr& from);
    void sendPunch(const SockAddr& to);
    void addPunchTarget(const SockAddr& addr, uint32_t now);
    bool sessionAt(const SockAddr& addr) const;
    void servicePaths(uint32_t now);
    void tick(uint32_t now, Deliveries& out);
    void dispatch(Deliveries& out);
};

void Endpoint::Impl::run() {
    std::vector<uint8_t> buf(kMaxDatagram);
    while (running.load(std::memory_order_relaxed)) {
        SockAddr from;
        int n = sock.recvFrom(buf.data(), buf.size(), from, 5);

        Deliveries out;
        {
            std::lock_guard<std::mutex> lk(mtx);
            bool before = ready();
            for (int i = 0; n > 0 && i < kDrainPerWake; ++i) {
                handle(buf.data(), static_cast<size_t>(n), from, nowMs(), out);
                n = sock.recvFrom(buf.data(), buf.size(), from, 0);
            }
            uint32_t now = nowMs();
            tick(now, out);
            if (ready() != before)
                cv.notify_all();
        }
        dispatch(out);
    }
}

void Endpoint::Impl::handle(const uint8_t* p, size_t n, const SockAddr& from, uint32_t now,
                            Deliveries& out) {
    if (isStunMessage(p, n)) {
        handleStun(p, n, from);
        return;
    }

    Header h;
    if (!readHeader(p, n, h))
        return;

    if (h.type == PacketType::Punch) {
        handlePunch(p, n, h, from, now);
        return;
    }

    if (!server) {
        if (client)
            client->onDatagram(p, n, h, from, now, out);
        return;
    }

    if (h.type == PacketType::Hello) {
        handleHello(p, n, h, from, now, out);
        return;
    }

    auto it = sessions.find(h.session);
    if (it == sessions.end())
        return;
    it->second->onDatagram(p, n, h, from, now, out);
}

void Endpoint::Impl::handleHello(const uint8_t* p, size_t n, const Header& h, const SockAddr& from,
                                 uint32_t now, Deliveries& out) {
    if (n != kHeaderSize + kHelloBodySize + kTagSize)
        return;

    std::vector<uint8_t> plain;
    if (!hs.open(h.counter, p, kHeaderSize, p + kHeaderSize, n - kHeaderSize, plain))
        return;

    Reader r(plain.data(), plain.size());
    HelloBody hb;
    if (!readHello(r, hb))
        return;
    if (!helloStampFresh(hb.stamp))
        return;

    for (auto& entry : sessions) {
        Session* s = entry.second.get();
        if (std::memcmp(s->remoteNonce(), hb.nonce, kNonceSize) == 0) {
            s->sendWelcome();
            return;
        }
    }

    while (!seenHellos.empty() &&
           seqDiff(now, seenHellos.front().at) > static_cast<int32_t>(kHelloCacheMs))
        seenHellos.pop_front();
    for (const SeenHello& seen : seenHellos) {
        if (std::memcmp(seen.nonce, hb.nonce, kNonceSize) == 0)
            return;
    }

    if (sessions.size() >= cfg.maxPeers)
        return;

    uint32_t id = 0;
    do {
        id = randomU32();
    } while (id == 0 || sessions.find(id) != sessions.end());

    auto s = std::make_unique<Session>(cfg, hs, true, from, sink());
    s->acceptClient(id, hb.nonce, now);
    sessions.emplace(id, std::move(s));

    SeenHello seen;
    std::memcpy(seen.nonce, hb.nonce, kNonceSize);
    seen.at = now;
    seenHellos.push_back(seen);
    if (seenHellos.size() > kHelloCacheMax)
        seenHellos.pop_front();

    out.events.emplace_back(id, PeerEvent::Connected);
}

void Endpoint::Impl::sendPunch(const SockAddr& to) {
    HelloBody hb;
    hb.stamp = unixMs();
    randomBytes(hb.nonce, kNonceSize);

    std::vector<uint8_t> body;
    writeHello(body, hb);

    Header h;
    h.type = PacketType::Punch;
    h.session = 0;
    h.counter = randomU64() | 1;

    std::vector<uint8_t> pkt;
    writeHeader(pkt, h);
    hs.seal(h.counter, pkt, kHeaderSize, body.data(), body.size());
    sock.sendTo(pkt.data(), pkt.size(), to);
}

void Endpoint::Impl::addPunchTarget(const SockAddr& addr, uint32_t now) {
    if (!addr.valid())
        return;

    for (PunchTarget& t : punchTargets) {
        if (t.addr.sameAs(addr)) {
            t.addedAt = now;
            return;
        }
    }
    if (punchTargets.size() >= kMaxPunchTargets)
        return;

    PunchTarget t;
    t.addr = addr;
    t.addedAt = now;
    t.lastSent = now - kPunchIntervalMs;
    punchTargets.push_back(t);
}

bool Endpoint::Impl::sessionAt(const SockAddr& addr) const {
    if (client && client->established() && client->peerAddr().sameAs(addr))
        return true;
    for (const auto& entry : sessions) {
        if (entry.second->peerAddr().sameAs(addr))
            return true;
    }
    return false;
}

void Endpoint::Impl::handlePunch(const uint8_t* p, size_t n, const Header& h, const SockAddr& from,
                                 uint32_t now) {
    if (n != kHeaderSize + kHelloBodySize + kTagSize)
        return;

    std::vector<uint8_t> plain;
    if (!hs.open(h.counter, p, kHeaderSize, p + kHeaderSize, n - kHeaderSize, plain))
        return;

    Reader r(plain.data(), plain.size());
    HelloBody hb;
    if (!readHello(r, hb) || !helloStampFresh(hb.stamp))
        return;

    addPunchTarget(from, now);
}

void Endpoint::Impl::handleStun(const uint8_t* p, size_t n, const SockAddr& from) {
    for (StunQuery& q : stunQueries) {
        if (q.done || !q.server.sameAs(from))
            continue;
        SockAddr mapped;
        if (!parseStunResponse(p, n, q.txid, mapped))
            continue;
        q.done = true;
        reflexive = mapped;
        cv.notify_all();
        return;
    }
}

void Endpoint::Impl::servicePaths(uint32_t now) {
    for (auto it = punchTargets.begin(); it != punchTargets.end();) {
        if (seqDiff(now, it->addedAt) > static_cast<int32_t>(kPunchLifetimeMs) ||
            sessionAt(it->addr)) {
            it = punchTargets.erase(it);
            continue;
        }
        if (seqDiff(now, it->lastSent) >= static_cast<int32_t>(kPunchIntervalMs)) {
            it->lastSent = now;
            sendPunch(it->addr);
        }
        ++it;
    }

    for (StunQuery& q : stunQueries) {
        if (q.done || q.attempts >= kStunAttempts)
            continue;
        if (q.attempts && seqDiff(now, q.lastSent) < static_cast<int32_t>(kStunRetryMs))
            continue;
        std::vector<uint8_t> req;
        buildStunRequest(q.txid, req);
        sock.sendTo(req.data(), req.size(), q.server);
        q.lastSent = now;
        q.attempts++;
    }
}

void Endpoint::Impl::tick(uint32_t now, Deliveries& out) {
    servicePaths(now);

    if (client)
        client->update(now, out);

    for (auto it = sessions.begin(); it != sessions.end();) {
        it->second->update(now, out);
        if (it->second->dead())
            it = sessions.erase(it);
        else
            ++it;
    }
}

void Endpoint::Impl::dispatch(Deliveries& out) {
    if (out.events.empty() && out.messages.empty() && !buffered.load(std::memory_order_relaxed))
        return;

    PeerHandler pc;
    InputHandler ic;
    AudioHandler ac;
    ControlHandler cc;
    std::vector<std::pair<PeerId, PeerEvent>> events;
    {
        std::lock_guard<std::mutex> lk(mtx);
        pc = peerCb;
        ic = inputCb;
        ac = audioCb;
        cc = controlCb;
        if (pc) {
            events.swap(pendingEvents);
            events.insert(events.end(), out.events.begin(), out.events.end());
            buffered.store(false, std::memory_order_relaxed);
        } else {
            for (auto& ev : out.events) {
                if (pendingEvents.size() < 64)
                    pendingEvents.push_back(ev);
            }
            buffered.store(!pendingEvents.empty(), std::memory_order_relaxed);
        }
    }

    for (auto& ev : events)
        pc(ev.first, ev.second);

    std::vector<InputEvent> batch;
    for (auto& msg : out.messages) {
        if (msg.channel == kChanAudio) {
            if (!ac)
                continue;
            AudioFrame frame;
            if (decodeAudioFrame(msg.data.data(), msg.data.size(), frame))
                ac(msg.peer, frame);
            continue;
        }
        if (msg.data.empty())
            continue;
        uint8_t tag = msg.data[0];
        const uint8_t* body = msg.data.data() + 1;
        size_t len = msg.data.size() - 1;
        if (tag == kMsgInput) {
            if (!ic)
                continue;
            if (!decodeInputBatch(body, len, batch))
                continue;
            for (const InputEvent& e : batch)
                ic(msg.peer, e);
        } else if (tag == kMsgRaw) {
            if (cc)
                cc(msg.peer, body, len);
        }
    }
}

Endpoint::Endpoint() : impl_(new Impl()) {}

Endpoint::~Endpoint() {
    stop();
    if (impl_ && impl_->worker.get_id() == std::this_thread::get_id()) {
        impl_->worker.detach();
        impl_.release();
    }
}

std::shared_ptr<Endpoint> Endpoint::listen(const Config& cfg) {
    std::shared_ptr<Endpoint> ep(new Endpoint());
    Impl& im = *ep->impl_;
    im.cfg = sanitize(cfg);
    im.server = true;
    im.hs = handshakeKey(cfg.psk);
    if (!im.sock.bindTo(im.cfg.bindAddress, im.cfg.bindPort))
        return nullptr;
    im.start();
    return ep;
}

std::shared_ptr<Endpoint> Endpoint::connect(const Config& cfg, const std::string& host,
                                            uint16_t port) {
    SockAddr target;
    if (!resolveAddr(host, port, target))
        return nullptr;

    std::shared_ptr<Endpoint> ep(new Endpoint());
    Impl& im = *ep->impl_;
    im.cfg = sanitize(cfg);
    im.server = false;
    im.hs = handshakeKey(cfg.psk);

    std::string bind = im.cfg.bindAddress;
    if (target.sa.ss_family == AF_INET6 && bind == "0.0.0.0")
        bind = "::";
    if (!im.sock.bindTo(bind, im.cfg.bindPort))
        return nullptr;

    im.client.reset(new Session(im.cfg, im.hs, false, target, im.sink()));
    im.client->startClient(nowMs());
    im.start();
    return ep;
}

void Endpoint::onPeer(PeerHandler fn) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->peerCb = std::move(fn);
}

void Endpoint::onInput(InputHandler fn) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->inputCb = std::move(fn);
}

void Endpoint::onAudio(AudioHandler fn) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->audioCb = std::move(fn);
}

void Endpoint::onControl(ControlHandler fn) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->controlCb = std::move(fn);
}

bool Endpoint::sendInput(PeerId peer, const InputEvent* events, size_t count) {
    if (!events || count == 0)
        return false;

    std::lock_guard<std::mutex> lk(impl_->mtx);
    Session* s = impl_->find(peer);
    if (!s)
        return false;

    uint32_t now = nowMs();
    std::vector<uint8_t> msg;
    size_t off = 0;
    while (off < count) {
        size_t take = std::min(count - off, kMaxBatch);
        msg.clear();
        msg.push_back(kMsgInput);
        encodeInputBatch(events + off, take, msg);
        if (!s->sendMessage(kChanControl, msg.data(), msg.size(), now))
            return false;
        off += take;
    }
    return true;
}

bool Endpoint::sendAudio(PeerId peer, const AudioFrame& frame) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    Session* s = impl_->find(peer);
    if (!s)
        return false;

    std::vector<uint8_t> msg;
    msg.reserve(frame.payload.size() + 12);
    encodeAudioFrame(frame, msg);
    return s->sendMessage(kChanAudio, msg.data(), msg.size(), nowMs());
}

bool Endpoint::sendControl(PeerId peer, const void* data, size_t len) {
    if (!data && len)
        return false;

    std::lock_guard<std::mutex> lk(impl_->mtx);
    Session* s = impl_->find(peer);
    if (!s)
        return false;

    std::vector<uint8_t> msg;
    msg.reserve(len + 1);
    msg.push_back(kMsgRaw);
    if (len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        msg.insert(msg.end(), p, p + len);
    }
    return s->sendMessage(kChanControl, msg.data(), msg.size(), nowMs());
}

bool Endpoint::gatherPublicAddress(uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lk(impl_->mtx);
    if (impl_->cfg.stunServers.empty())
        return false;

    impl_->stunQueries.clear();
    for (const std::string& text : impl_->cfg.stunServers) {
        Impl::StunQuery q;
        if (!parseEndpoint(text, q.server))
            continue;
        randomBytes(q.txid, kStunTxidSize);
        impl_->stunQueries.push_back(q);
    }
    if (impl_->stunQueries.empty())
        return false;

    Impl* im = impl_.get();
    impl_->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                       [im] { return im->reflexive.valid(); });
    return impl_->reflexive.valid();
}

std::string Endpoint::publicAddress() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->reflexive.text();
}

void Endpoint::addRemoteCandidate(const std::string& endpoint) {
    SockAddr addr;
    if (!parseEndpoint(endpoint, addr))
        return;
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->addPunchTarget(addr, nowMs());
}

bool Endpoint::waitConnected(uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lk(impl_->mtx);
    Impl* im = impl_.get();
    bool ok = impl_->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                 [im] { return im->ready(); });
    if (!ok)
        return false;
    if (impl_->server)
        return !impl_->sessions.empty();
    return impl_->client && impl_->client->established();
}

Endpoint::PeerId Endpoint::localPeer() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->client ? impl_->client->id() : 0;
}

uint16_t Endpoint::localPort() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->sock.localPort();
}

std::vector<Endpoint::PeerId> Endpoint::peers() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    std::vector<PeerId> ids;
    if (impl_->server) {
        ids.reserve(impl_->sessions.size());
        for (const auto& entry : impl_->sessions)
            ids.push_back(entry.first);
    } else if (impl_->client && impl_->client->established() && !impl_->client->dead()) {
        ids.push_back(impl_->client->id());
    }
    return ids;
}

bool Endpoint::stats(PeerId peer, Stats& out) const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    Session* s = impl_->find(peer);
    if (!s)
        return false;
    s->fillStats(out);
    return true;
}

void Endpoint::disconnect(PeerId peer) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    Session* s = impl_->find(peer);
    if (!s)
        return;
    s->shutdown();
    if (impl_->server)
        impl_->sessions.erase(peer);
}

void Endpoint::stop() {
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        if (impl_->client)
            impl_->client->shutdown();
        for (auto& entry : impl_->sessions)
            entry.second->shutdown();
    }

    impl_->running.store(false);
    if (impl_->worker.get_id() == std::this_thread::get_id())
        return;
    if (impl_->worker.joinable())
        impl_->worker.join();

    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->sessions.clear();
    impl_->client.reset();
    impl_->sock.shut();
}

}
