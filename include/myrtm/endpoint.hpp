#ifndef MYRTM_ENDPOINT_H
#define MYRTM_ENDPOINT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "myrtm/codec.hpp"
#include "myrtm/config.hpp"

namespace myrtm {

class Endpoint {
public:
    using PeerId = uint32_t;
    using PeerHandler = std::function<void(PeerId, PeerEvent)>;
    using InputHandler = std::function<void(PeerId, const InputEvent&)>;
    using AudioHandler = std::function<void(PeerId, const AudioFrame&)>;
    using ControlHandler = std::function<void(PeerId, const uint8_t*, size_t)>;

    static std::shared_ptr<Endpoint> listen(const Config& cfg);
    static std::shared_ptr<Endpoint> connect(const Config& cfg, const std::string& host,
                                             uint16_t port);
    ~Endpoint();

    Endpoint(const Endpoint&) = delete;
    Endpoint& operator=(const Endpoint&) = delete;

    void onPeer(PeerHandler fn);
    void onInput(InputHandler fn);
    void onAudio(AudioHandler fn);
    void onControl(ControlHandler fn);

    bool sendInput(PeerId peer, const InputEvent* events, size_t count);
    bool sendAudio(PeerId peer, const AudioFrame& frame);
    bool sendControl(PeerId peer, const void* data, size_t len);

    bool gatherPublicAddress(uint32_t timeoutMs);
    std::string publicAddress() const;
    void addRemoteCandidate(const std::string& endpoint);

    bool waitConnected(uint32_t timeoutMs);
    PeerId localPeer() const;
    uint16_t localPort() const;
    std::vector<PeerId> peers() const;
    bool stats(PeerId peer, Stats& out) const;
    void disconnect(PeerId peer);
    void stop();

private:
    Endpoint();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
