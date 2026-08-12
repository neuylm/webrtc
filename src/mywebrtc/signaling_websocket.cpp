/**
 * signaling_websocket.cpp — Layer E: WebSocket 信令默认实现
 */
#include "signaling_channel.hpp"

#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <rtc/websocket.hpp>

namespace mywebrtc {

using json = nlohmann::json;

struct WebSocketSignalingChannel::Impl {
    std::shared_ptr<rtc::WebSocket> ws;
    TypedCallbackMap<void(const SignalingMessage&)> msg_cbs;
    std::atomic<bool> connected{false};
};

WebSocketSignalingChannel::WebSocketSignalingChannel()
    : impl_(std::make_unique<Impl>()) {}

WebSocketSignalingChannel::~WebSocketSignalingChannel() {
    close();
}

bool WebSocketSignalingChannel::connect(const std::string& url) {
    impl_->ws = std::make_shared<rtc::WebSocket>();
    setupCallbacks();

    std::promise<bool> pr;
    auto fut = pr.get_future();

    impl_->ws->onOpen([&pr]() { pr.set_value(true); });
    impl_->ws->onError([&pr](std::string) { pr.set_value(false); });

    impl_->ws->open(url);
    bool ok = fut.get();
    impl_->connected.store(ok, std::memory_order_release);
    return ok;
}

void WebSocketSignalingChannel::setupCallbacks() {
    impl_->ws->onMessage([self = this](auto data) {
        if (!std::holds_alternative<std::string>(data)) return;
        SignalingMessage msg;
        try {
            auto j = json::parse(std::get<std::string>(data));
            msg.peer_id = j.value("id", "");
            msg.type    = j.value("type", "");
            if (j.contains("description"))
                msg.description = j["description"].template get<std::string>();
            if (j.contains("candidate"))
                msg.candidate = j["candidate"].template get<std::string>();
            if (j.contains("mid"))
                msg.mid = j["mid"].template get<std::string>();
            msg.raw_json = j.dump();
        } catch (...) {
            return;
        }

        auto cbs = self->impl_->msg_cbs.snapshot();
        for (auto& cb : cbs) cb(msg);
    });
}

bool WebSocketSignalingChannel::send(const std::string& peerId, const SignalingMessage& msg) {
    if (!isConnected() || !impl_->ws || !impl_->ws->isOpen()) return false;
    json j;
    j["id"] = peerId;
    j["type"] = msg.type;
    if (msg.description) j["description"] = *msg.description;
    if (msg.candidate)   j["candidate"] = *msg.candidate;
    if (msg.mid)         j["mid"] = *msg.mid;
    return impl_->ws->send(j.dump());
}

Subscription WebSocketSignalingChannel::onMessage(OnMessage cb) {
    auto id = impl_->msg_cbs.add(std::move(cb));
    auto* reg = &impl_->msg_cbs;
    return Subscription([reg, id]{ reg->remove(id); });
}

void WebSocketSignalingChannel::close() {
    if (impl_->ws) {
        impl_->connected.store(false, std::memory_order_release);
        impl_->ws->close();
        impl_->ws.reset();
    }
}

bool WebSocketSignalingChannel::isConnected() const {
    return impl_->connected.load(std::memory_order_acquire);
}

} /* namespace mywebrtc */
