/**
 * peerconnection.cpp — Layer B 实现
 */
#include "peerconnection.hpp"
#include "datachannel.hpp"
#include "track.hpp"

#include <stdexcept>

namespace mywebrtc {

std::shared_ptr<PeerConnection> PeerConnection::create(rtc::Configuration cfg) {
    auto p = std::shared_ptr<PeerConnection>(new PeerConnection(std::move(cfg)));
    p->setupCallbacks();
    return p;
}

PeerConnection::PeerConnection(rtc::Configuration cfg)
    : pc_(std::make_shared<rtc::PeerConnection>(std::move(cfg))) {}

PeerConnection::~PeerConnection() {
    try { close(); } catch (...) {}
}

void PeerConnection::setupCallbacks() {
    pc_->onStateChange([self = weak_from_this()](State s) {
        if (auto sp = self.lock()) {
            auto cbs = sp->state_cbs_.snapshot();
            for (auto& cb : cbs) cb(s);
        }
    });
    pc_->onIceStateChange([self = weak_from_this()](IceState s) {
        if (auto sp = self.lock()) {
            auto cbs = sp->ice_state_cbs_.snapshot();
            for (auto& cb : cbs) cb(s);
        }
    });
    pc_->onLocalDescription([self = weak_from_this()](rtc::Description d) {
        if (auto sp = self.lock()) {
            auto cbs = sp->desc_cbs_.snapshot();
            for (auto& cb : cbs) {
                try { cb(std::string(d), d.typeString()); } catch (...) {}
            }
        }
    });
    pc_->onLocalCandidate([self = weak_from_this()](rtc::Candidate c) {
        if (auto sp = self.lock()) {
            auto cbs = sp->cand_cbs_.snapshot();
            for (auto& cb : cbs) cb(std::string(c), c.mid());
        }
    });
    pc_->onDataChannel([self = weak_from_this()](std::shared_ptr<rtc::DataChannel> dc) {
        if (auto sp = self.lock()) {
            auto wrapped = DataChannel::create(std::move(dc));
            auto cbs = sp->dc_cbs_.snapshot();
            for (auto& cb : cbs) cb(wrapped);
        }
    });
    pc_->onTrack([self = weak_from_this()](std::shared_ptr<rtc::Track> t) {
        if (auto sp = self.lock()) {
            auto wrapped = Track::create(std::move(t));
            auto cbs = sp->track_cbs_.snapshot();
            for (auto& cb : cbs) cb(wrapped);
        }
    });
}

std::future<std::string> PeerConnection::createOffer() {
    auto pr = std::make_shared<std::promise<std::string>>();
    auto fut = pr->get_future();
    try {
        pc_->onLocalDescription([pr](rtc::Description d) {
            pr->set_value(std::string(d));
        });
        pc_->setLocalDescription(rtc::Description::Type::Offer);
    } catch (...) {
        pr->set_exception(std::current_exception());
    }
    return fut;
}

std::future<std::string> PeerConnection::createAnswer() {
    auto pr = std::make_shared<std::promise<std::string>>();
    auto fut = pr->get_future();
    try {
        pc_->onLocalDescription([pr](rtc::Description d) {
            pr->set_value(std::string(d));
        });
        pc_->setLocalDescription(rtc::Description::Type::Answer);
    } catch (...) {
        pr->set_exception(std::current_exception());
    }
    return fut;
}

std::future<void> PeerConnection::setRemoteDescription(const std::string& sdp,
                                                       const std::string& type) {
    auto pr = std::make_shared<std::promise<void>>();
    auto fut = pr->get_future();
    try {
        rtc::Description::Type t = rtc::Description::Type::Unspec;
        if (type == "offer") t = rtc::Description::Type::Offer;
        else if (type == "answer") t = rtc::Description::Type::Answer;
        else if (type == "pranswer") t = rtc::Description::Type::Pranswer;
        pc_->setRemoteDescription(rtc::Description(sdp, t));
        pr->set_value();
    } catch (...) {
        pr->set_exception(std::current_exception());
    }
    return fut;
}

void PeerConnection::setLocalDescription(rtc::Description::Type type) {
    pc_->setLocalDescription(type);
}

void PeerConnection::addRemoteCandidate(const std::string& cand, const std::string& mid) {
    pc_->addRemoteCandidate(rtc::Candidate(cand, mid));
}

Subscription PeerConnection::onStateChange(std::function<void(State)> cb) {
    auto id = state_cbs_.add(std::move(cb));
    auto* reg = &state_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

Subscription PeerConnection::onIceStateChange(std::function<void(IceState)> cb) {
    auto id = ice_state_cbs_.add(std::move(cb));
    auto* reg = &ice_state_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

Subscription PeerConnection::onLocalDescription(
        std::function<void(const std::string&, const std::string&)> cb) {
    auto id = desc_cbs_.add(std::move(cb));
    auto* reg = &desc_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

Subscription PeerConnection::onLocalCandidate(
        std::function<void(const std::string&, const std::string&)> cb) {
    auto id = cand_cbs_.add(std::move(cb));
    auto* reg = &cand_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

Subscription PeerConnection::onDataChannel(
        std::function<void(std::shared_ptr<DataChannel>)> cb) {
    auto id = dc_cbs_.add(std::move(cb));
    auto* reg = &dc_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

Subscription PeerConnection::onTrack(
        std::function<void(std::shared_ptr<Track>)> cb) {
    auto id = track_cbs_.add(std::move(cb));
    auto* reg = &track_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

std::shared_ptr<DataChannel> PeerConnection::createDataChannel(const std::string& label) {
    return DataChannel::create(pc_->createDataChannel(label));
}

std::shared_ptr<Track> PeerConnection::addTrack(const rtc::Description::Media& media) {
    return Track::create(pc_->addTrack(media));
}

PeerConnection::State PeerConnection::state() const { return pc_->state(); }
PeerConnection::IceState PeerConnection::iceState() const { return pc_->iceState(); }
bool PeerConnection::negotiationNeeded() const { return pc_->negotiationNeeded(); }
std::optional<std::string> PeerConnection::localDescription() const {
    auto d = pc_->localDescription();
    if (d) return std::string(*d);
    return std::nullopt;
}
std::optional<std::string> PeerConnection::remoteDescription() const {
    auto d = pc_->remoteDescription();
    if (d) return std::string(*d);
    return std::nullopt;
}
std::optional<std::string> PeerConnection::localAddress() const { return pc_->localAddress(); }
std::optional<std::string> PeerConnection::remoteAddress() const { return pc_->remoteAddress(); }

void PeerConnection::close() { pc_->close(); }

} /* namespace mywebrtc */
