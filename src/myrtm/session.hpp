#ifndef MYRTM_SESSION_H
#define MYRTM_SESSION_H

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "myrtm/config.hpp"

#include "channel.hpp"
#include "crypto.hpp"
#include "socket.hpp"
#include "wire.hpp"

namespace myrtm {

struct Delivered {
    uint32_t peer = 0;
    uint8_t channel = 0;
    std::vector<uint8_t> data;
};

struct Deliveries {
    std::vector<Delivered> messages;
    std::vector<std::pair<uint32_t, PeerEvent>> events;
};

bool helloStampFresh(uint64_t stamp);

class Session {
public:
    using Sink = std::function<void(const uint8_t*, size_t, const SockAddr&)>;

    Session(const Config& cfg, const AeadKey& hs, bool server, const SockAddr& peer, Sink sink);

    uint32_t id() const { return id_; }
    bool established() const { return established_; }
    bool dead() const { return dead_; }
    const SockAddr& peerAddr() const { return peer_; }
    const uint8_t* remoteNonce() const { return remoteNonce_; }

    void startClient(uint32_t now);
    void acceptClient(uint32_t id, const uint8_t nonce[kNonceSize], uint32_t now);
    void sendWelcome();

    void onDatagram(const uint8_t* pkt, size_t len, const Header& h, const SockAddr& from,
                    uint32_t now, Deliveries& out);
    void update(uint32_t now, Deliveries& out);
    bool sendMessage(uint8_t channel, const uint8_t* data, size_t len, uint32_t now);
    void shutdown();
    void fillStats(Stats& s) const;

private:
    void emit(PacketType type, const AeadKey& key, const uint8_t* payload, size_t len);
    void emitData(const DataHeader& h, const uint8_t* p, size_t n);
    void sendHello(uint32_t now);
    void sendPing(uint32_t now);
    void sendAcks(uint32_t now);
    void flushChannels(uint32_t now);
    void drainRecv(uint32_t now, Deliveries& out);

    Config cfg_;
    AeadKey hs_;
    bool server_;
    SockAddr peer_;
    Sink sink_;

    uint32_t id_ = 0;
    bool established_ = false;
    bool dead_ = false;

    uint8_t localNonce_[kNonceSize] = {0};
    uint8_t remoteNonce_[kNonceSize] = {0};
    SessionKeys keys_;
    ReplayWindow replay_;
    uint64_t txCounter_ = 0;

    uint32_t startedAt_ = 0;
    uint32_t lastRecv_ = 0;
    uint32_t lastHello_ = 0;
    uint32_t lastPing_ = 0;
    uint32_t linkRtt_ = 0;

    uint64_t authFailures_ = 0;
    uint64_t replayDrops_ = 0;

    SendChannel sendCtl_;
    SendChannel sendAudio_;
    RecvChannel recvCtl_;
    RecvChannel recvAudio_;

    std::vector<uint8_t> body_;
    std::vector<uint8_t> pkt_;
    std::vector<uint8_t> plain_;
};

}

#endif
