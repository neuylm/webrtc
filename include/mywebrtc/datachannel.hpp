/**
 * datachannel.hpp — Layer B: RAII DataChannel 封装
 */
#ifndef MYRTC_DATA_CHANNEL_H
#define MYRTC_DATA_CHANNEL_H

#include <functional>
#include <memory>
#include <string>
#include <variant>

#include "async.hpp"
#include "callback_registry.hpp"
#include "rtc/datachannel.hpp"
#include "rtc/common.hpp"

namespace mywebrtc {

class PeerConnection;

class DataChannel : public std::enable_shared_from_this<DataChannel> {
public:
    static std::shared_ptr<DataChannel> create(std::shared_ptr<rtc::DataChannel> dc) {
        auto p = std::shared_ptr<DataChannel>(new DataChannel(std::move(dc)));
        p->setupCallbacks();
        return p;
    }
    ~DataChannel();

    DataChannel(const DataChannel&) = delete;
    DataChannel& operator=(const DataChannel&) = delete;

    using message_variant = rtc::message_variant;

    bool send(const std::string& text);
    bool send(const rtc::byte* data, size_t size);
    bool sendBinary(rtc::binary data);

    void close();
    bool isOpen() const;
    bool isClosed() const;
    size_t maxMessageSize() const;
    size_t bufferedAmount() const;

    std::optional<uint16_t> id() const;
    std::optional<uint16_t> stream() const;
    std::string label() const;
    std::string protocol() const;
    rtc::Reliability reliability() const;

    Subscription onOpen(std::function<void()>);
    Subscription onClosed(std::function<void()>);
    Subscription onError(std::function<void(std::string)>);
    Subscription onMessage(std::function<void(message_variant)>);
    Subscription onMessage(std::function<void(rtc::binary)>,
                           std::function<void(std::string)>);
    Subscription onBufferedAmountLow(std::function<void()>);

    rtc::DataChannel* underlying() { return dc_.get(); }
    std::shared_ptr<rtc::DataChannel> underlyingShared() { return dc_; }

private:
    explicit DataChannel(std::shared_ptr<rtc::DataChannel> dc) : dc_(std::move(dc)) {}
    void setupCallbacks();
    std::shared_ptr<rtc::DataChannel> dc_;
    TypedCallbackMap<void()>                          open_cbs_;
    TypedCallbackMap<void()>                          closed_cbs_;
    TypedCallbackMap<void(std::string)>               error_cbs_;
    TypedCallbackMap<void(message_variant)>           msg_cbs_;
    std::function<void(rtc::binary)>                   bin_cb_;
    std::function<void(std::string)>                   str_cb_;
    std::mutex                                        bin_str_mtx_;
    TypedCallbackMap<void()>                          bal_cbs_;
};

} /* namespace mywebrtc */

#endif /* MYRTC_DATA_CHANNEL_H */
