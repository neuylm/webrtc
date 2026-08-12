/**
 * configuration.hpp — Layer B: Configuration
 *
 * Builder 风格 + 默认值的配置对象，比原 rtc::Configuration 更易用。
 */
#ifndef MYRTC_CONFIGURATION_H
#define MYRTC_CONFIGURATION_H

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "rtc/configuration.hpp"  /* 复用底层 rtc::Configuration */

namespace mywebrtc {

class ConfigurationBuilder {
public:
    ConfigurationBuilder& addIceServer(const std::string& url) {
        rtc_.iceServers.emplace_back(url);
        return *this;
    }
    ConfigurationBuilder& setBindAddress(const std::string& addr) {
        rtc_.bindAddress = addr;
        return *this;
    }
    ConfigurationBuilder& setMtu(size_t mtu) {
        rtc_.mtu = mtu;
        return *this;
    }
    ConfigurationBuilder& setMaxMessageSize(size_t sz) {
        rtc_.maxMessageSize = sz;
        return *this;
    }
    ConfigurationBuilder& enableIceUdpMux(bool b = true) {
        rtc_.enableIceUdpMux = b;
        return *this;
    }
    ConfigurationBuilder& disableAutoNegotiation(bool b = true) {
        rtc_.disableAutoNegotiation = b;
        return *this;
    }
    ConfigurationBuilder& forceMediaTransport(bool b = true) {
        rtc_.forceMediaTransport = b;
        return *this;
    }
    ConfigurationBuilder& setCertificateType(rtc::CertificateType t) {
        rtc_.certificateType = t;
        return *this;
    }
    /* 业务层配置 */
    ConfigurationBuilder& setSignalingUrl(const std::string& url) {
        signaling_url_ = url;
        return *this;
    }
    ConfigurationBuilder& setAuthToken(const std::string& t) {
        auth_token_ = t;
        return *this;
    }
    ConfigurationBuilder& setPeerId(const std::string& id) {
        peer_id_ = id;
        return *this;
    }

    rtc::Configuration build() const { return rtc_; }

    const std::optional<std::string>& signalingUrl() const { return signaling_url_; }
    const std::optional<std::string>& authToken() const { return auth_token_; }
    const std::optional<std::string>& peerId() const { return peer_id_; }

private:
    rtc::Configuration rtc_;
    std::optional<std::string> signaling_url_;
    std::optional<std::string> auth_token_;
    std::optional<std::string> peer_id_;
};

} /* namespace mywebrtc */

#endif /* MYRTC_CONFIGURATION_H */
