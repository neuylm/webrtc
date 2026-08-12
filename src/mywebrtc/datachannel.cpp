/**
 * datachannel.cpp — Layer B 实现
 */
#include "datachannel.hpp"

namespace mywebrtc {

DataChannel::~DataChannel() {
    try { if (dc_) dc_->close(); } catch (...) {}
}

void DataChannel::setupCallbacks() {
    dc_->onOpen([self = weak_from_this()]() {
        if (auto sp = self.lock()) {
            auto cbs = sp->open_cbs_.snapshot();
            for (auto& cb : cbs) cb();
        }
    });
    dc_->onClosed([self = weak_from_this()]() {
        if (auto sp = self.lock()) {
            auto cbs = sp->closed_cbs_.snapshot();
            for (auto& cb : cbs) cb();
        }
    });
    dc_->onError([self = weak_from_this()](std::string err) {
        if (auto sp = self.lock()) {
            auto cbs = sp->error_cbs_.snapshot();
            for (auto& cb : cbs) cb(err);
        }
    });
    dc_->onMessage([self = weak_from_this()](message_variant data) {
        if (auto sp = self.lock()) {
            {
                std::lock_guard<std::mutex> lk(sp->bin_str_mtx_);
                if (sp->bin_cb_ && std::holds_alternative<rtc::binary>(data)) {
                    sp->bin_cb_(std::get<rtc::binary>(data));
                } else if (sp->str_cb_ && std::holds_alternative<std::string>(data)) {
                    sp->str_cb_(std::get<std::string>(data));
                }
            }
            auto cbs = sp->msg_cbs_.snapshot();
            for (auto& cb : cbs) cb(data);
        }
    });
    dc_->onBufferedAmountLow([self = weak_from_this()]() {
        if (auto sp = self.lock()) {
            auto cbs = sp->bal_cbs_.snapshot();
            for (auto& cb : cbs) cb();
        }
    });
}

bool DataChannel::send(const std::string& text) { return dc_->send(text); }
bool DataChannel::send(const rtc::byte* data, size_t size) { return dc_->send(data, size); }
bool DataChannel::sendBinary(rtc::binary data) { return dc_->send(std::move(data)); }

void DataChannel::close() { dc_->close(); }
bool DataChannel::isOpen() const { return dc_->isOpen(); }
bool DataChannel::isClosed() const { return dc_->isClosed(); }
size_t DataChannel::maxMessageSize() const { return dc_->maxMessageSize(); }
size_t DataChannel::bufferedAmount() const { return dc_->bufferedAmount(); }
std::optional<uint16_t> DataChannel::id() const { return dc_->id(); }
std::optional<uint16_t> DataChannel::stream() const { return dc_->stream(); }
std::string DataChannel::label() const { return dc_->label(); }
std::string DataChannel::protocol() const { return dc_->protocol(); }
rtc::Reliability DataChannel::reliability() const { return dc_->reliability(); }

Subscription DataChannel::onOpen(std::function<void()> cb) {
    auto id = open_cbs_.add(std::move(cb));
    auto* reg = &open_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

Subscription DataChannel::onClosed(std::function<void()> cb) {
    auto id = closed_cbs_.add(std::move(cb));
    auto* reg = &closed_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

Subscription DataChannel::onError(std::function<void(std::string)> cb) {
    auto id = error_cbs_.add(std::move(cb));
    auto* reg = &error_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

Subscription DataChannel::onMessage(std::function<void(message_variant)> cb) {
    auto id = msg_cbs_.add(std::move(cb));
    auto* reg = &msg_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

Subscription DataChannel::onMessage(std::function<void(rtc::binary)> bin,
                                     std::function<void(std::string)> str) {
    {
        std::lock_guard<std::mutex> lk(bin_str_mtx_);
        bin_cb_ = std::move(bin);
        str_cb_ = std::move(str);
    }
    return Subscription([this]{
        std::lock_guard<std::mutex> lk(bin_str_mtx_);
        bin_cb_ = nullptr;
        str_cb_ = nullptr;
    });
}

Subscription DataChannel::onBufferedAmountLow(std::function<void()> cb) {
    auto id = bal_cbs_.add(std::move(cb));
    auto* reg = &bal_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

} /* namespace mywebrtc */
