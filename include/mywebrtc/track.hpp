/**
 * track.hpp — Layer B: RAII Track 封装
 */
#ifndef MYRTC_TRACK_H
#define MYRTC_TRACK_H

#include <functional>
#include <memory>
#include <string>

#include "async.hpp"
#include "callback_registry.hpp"
#include "rtc/track.hpp"
#include "rtc/description.hpp"

namespace mywebrtc {

class Track : public std::enable_shared_from_this<Track> {
public:
    static std::shared_ptr<Track> create(std::shared_ptr<rtc::Track> t) {
        auto p = std::shared_ptr<Track>(new Track(std::move(t)));
        p->setupCallbacks();
        return p;
    }
    ~Track();

    Track(const Track&) = delete;
    Track& operator=(const Track&) = delete;

    std::string mid() const;
    rtc::Description::Direction direction() const;
    rtc::Description::Media description() const;
    void setDescription(rtc::Description::Media d);

    bool send(const rtc::byte* data, size_t size);
    void sendFrame(const rtc::byte* data, size_t size, rtc::FrameInfo info);
    void sendFrame(rtc::binary data, rtc::FrameInfo info);

    bool requestKeyframe(rtc::SSRC ssrc = 0, bool retransmit = false);
    bool requestBitrate(unsigned int bitrate);

    bool isOpen() const;
    bool isClosed() const;
    void close();

    void setMediaHandler(std::shared_ptr<rtc::MediaHandler> h);
    void chainMediaHandler(std::shared_ptr<rtc::MediaHandler> h);
    std::shared_ptr<rtc::MediaHandler> getMediaHandler();

    Subscription onFrame(std::function<void(rtc::binary, rtc::FrameInfo)>);
    Subscription onOpen(std::function<void()>);
    Subscription onClosed(std::function<void()>);

    rtc::Track* underlying() { return t_.get(); }
    std::shared_ptr<rtc::Track> underlyingShared() { return t_; }

private:
    explicit Track(std::shared_ptr<rtc::Track> t) : t_(std::move(t)) {}
    void setupCallbacks();
    std::shared_ptr<rtc::Track> t_;
    TypedCallbackMap<void(rtc::binary, rtc::FrameInfo)> frame_cbs_;
    TypedCallbackMap<void()>                           open_cbs_;
    TypedCallbackMap<void()>                           closed_cbs_;
};

} /* namespace mywebrtc */

#endif /* MYRTC_TRACK_H */
