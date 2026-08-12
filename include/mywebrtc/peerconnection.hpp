/**
 * peerconnection.hpp — Layer B: RAII PeerConnection 封装
 */
#ifndef MYRTC_PEER_CONNECTION_H
#define MYRTC_PEER_CONNECTION_H

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "async.hpp"
#include "callback_registry.hpp"
#include "rtc/peerconnection.hpp"
#include "rtc/description.hpp"

namespace mywebrtc {

class DataChannel;
class Track;
class EventBus;

class PeerConnection : public std::enable_shared_from_this<PeerConnection> {
public:
    using State      = rtc::PeerConnection::State;
    using IceState   = rtc::PeerConnection::IceState;
    using Gathering  = rtc::PeerConnection::GatheringState;
    using Signaling  = rtc::PeerConnection::SignalingState;

    static std::shared_ptr<PeerConnection> create(rtc::Configuration cfg);
    ~PeerConnection();

    PeerConnection(const PeerConnection&) = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;

    std::future<std::string> createOffer();
    std::future<std::string> createAnswer();
    std::future<void> setRemoteDescription(const std::string& sdp, const std::string& type);
    void setLocalDescription(rtc::Description::Type type = rtc::Description::Type::Unspec);
    void addRemoteCandidate(const std::string& cand, const std::string& mid);

    Subscription onStateChange(std::function<void(State)>);
    Subscription onIceStateChange(std::function<void(IceState)>);
    Subscription onLocalDescription(std::function<void(const std::string& sdp,
                                                       const std::string& type)>);
    Subscription onLocalCandidate(std::function<void(const std::string& cand,
                                                     const std::string& mid)>);
    Subscription onDataChannel(std::function<void(std::shared_ptr<DataChannel>)>);
    Subscription onTrack(std::function<void(std::shared_ptr<Track>)>);

    std::shared_ptr<DataChannel> createDataChannel(const std::string& label);
    std::shared_ptr<Track> addTrack(const rtc::Description::Media& media);

    State state() const;
    IceState iceState() const;
    bool negotiationNeeded() const;
    std::optional<std::string> localDescription() const;
    std::optional<std::string> remoteDescription() const;
    std::optional<std::string> localAddress() const;
    std::optional<std::string> remoteAddress() const;

    void close();
    rtc::PeerConnection* underlying() { return pc_.get(); }
    std::shared_ptr<rtc::PeerConnection> underlyingShared() { return pc_; }

private:
    explicit PeerConnection(rtc::Configuration cfg);
    void setupCallbacks();

    std::shared_ptr<rtc::PeerConnection> pc_;
    TypedCallbackMap<void(State)>       state_cbs_;
    TypedCallbackMap<void(IceState)>    ice_state_cbs_;
    TypedCallbackMap<void(const std::string&, const std::string&)> desc_cbs_;
    TypedCallbackMap<void(const std::string&, const std::string&)> cand_cbs_;
    TypedCallbackMap<void(std::shared_ptr<DataChannel>)> dc_cbs_;
    TypedCallbackMap<void(std::shared_ptr<Track>)>       track_cbs_;
};

} /* namespace mywebrtc */

#endif /* MYRTC_PEER_CONNECTION_H */
