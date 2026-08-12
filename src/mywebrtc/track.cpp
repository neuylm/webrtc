/**
 * track.cpp — Layer B 实现
 */
#include "track.hpp"

namespace mywebrtc {

Track::~Track() {
    try { if (t_) t_->close(); } catch (...) {}
}

void Track::setupCallbacks() {
    t_->onOpen([self = weak_from_this()]() {
        if (auto sp = self.lock()) {
            auto cbs = sp->open_cbs_.snapshot();
            for (auto& cb : cbs) cb();
        }
    });
    t_->onClosed([self = weak_from_this()]() {
        if (auto sp = self.lock()) {
            auto cbs = sp->closed_cbs_.snapshot();
            for (auto& cb : cbs) cb();
        }
    });
    t_->onFrame([self = weak_from_this()](rtc::binary data, rtc::FrameInfo info) {
        if (auto sp = self.lock()) {
            auto cbs = sp->frame_cbs_.snapshot();
            for (auto& cb : cbs) cb(data, info);
        }
    });
}

std::string Track::mid() const { return t_->mid(); }
rtc::Description::Direction Track::direction() const { return t_->direction(); }
rtc::Description::Media Track::description() const { return t_->description(); }
void Track::setDescription(rtc::Description::Media d) { t_->setDescription(std::move(d)); }

bool Track::send(const rtc::byte* data, size_t size) { return t_->send(data, size); }
void Track::sendFrame(const rtc::byte* data, size_t size, rtc::FrameInfo info) {
    t_->sendFrame(data, size, std::move(info));
}
void Track::sendFrame(rtc::binary data, rtc::FrameInfo info) {
    t_->sendFrame(std::move(data), std::move(info));
}

bool Track::requestKeyframe(rtc::SSRC ssrc, bool retransmit) {
    return t_->requestKeyframe(ssrc, retransmit);
}
bool Track::requestBitrate(unsigned int bitrate) { return t_->requestBitrate(bitrate); }

bool Track::isOpen() const { return t_->isOpen(); }
bool Track::isClosed() const { return t_->isClosed(); }
void Track::close() { t_->close(); }

void Track::setMediaHandler(std::shared_ptr<rtc::MediaHandler> h) { t_->setMediaHandler(std::move(h)); }
void Track::chainMediaHandler(std::shared_ptr<rtc::MediaHandler> h) { t_->chainMediaHandler(std::move(h)); }
std::shared_ptr<rtc::MediaHandler> Track::getMediaHandler() { return t_->getMediaHandler(); }

Subscription Track::onFrame(std::function<void(rtc::binary, rtc::FrameInfo)> cb) {
    auto id = frame_cbs_.add(std::move(cb));
    auto* reg = &frame_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

Subscription Track::onOpen(std::function<void()> cb) {
    auto id = open_cbs_.add(std::move(cb));
    auto* reg = &open_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

Subscription Track::onClosed(std::function<void()> cb) {
    auto id = closed_cbs_.add(std::move(cb));
    auto* reg = &closed_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

} /* namespace mywebrtc */
