/**
 * signaling_channel.hpp — Layer E: 信令通道抽象
 */
#ifndef MYRTC_SIGNALING_CHANNEL_H
#define MYRTC_SIGNALING_CHANNEL_H

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "async.hpp"
#include "callback_registry.hpp"

namespace mywebrtc {

struct SignalingMessage {
    std::string peer_id;
    std::string type;
    std::optional<std::string> description;
    std::optional<std::string> candidate;
    std::optional<std::string> mid;
    std::optional<std::string> raw_json;
};

class SignalingChannel {
public:
    using OnMessage = std::function<void(const SignalingMessage&)>;

    virtual ~SignalingChannel() = default;
    virtual bool connect(const std::string& url) = 0;
    virtual bool send(const std::string& peerId, const SignalingMessage& msg) = 0;
    virtual Subscription onMessage(OnMessage cb) = 0;
    virtual void close() = 0;
    virtual bool isConnected() const = 0;
};

class WebSocketSignalingChannel : public SignalingChannel {
public:
    WebSocketSignalingChannel();
    ~WebSocketSignalingChannel() override;

    bool connect(const std::string& url) override;
    bool send(const std::string& peerId, const SignalingMessage& msg) override;
    Subscription onMessage(OnMessage cb) override;
    void close() override;
    bool isConnected() const override;

private:
    void setupCallbacks();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} /* namespace mywebrtc */

#endif /* MYRTC_SIGNALING_CHANNEL_H */
