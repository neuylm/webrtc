#include "myrtm/endpoint.hpp"

#include "mywebrtc/auth.hpp"
#include "mywebrtc/datachannel.hpp"
#include "mywebrtc/session_manager.hpp"
#include "mywebrtc/signaling_channel.hpp"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_inputs{0};
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_dcMessages{0};
std::mutex g_out;

extern "C" void onInterrupt(int) {
    g_stop.store(true);
}

void say(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_out);
    std::cout << line << std::endl;
}

bool splitEndpoint(const std::string& text, std::string& host, uint16_t& port) {
    size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon + 1 >= text.size())
        return false;
    host = text.substr(0, colon);
    long value = std::strtol(text.c_str() + colon + 1, nullptr, 10);
    if (value <= 0 || value > 65535)
        return false;
    port = static_cast<uint16_t>(value);
    return true;
}

void announce(const std::shared_ptr<mywebrtc::SignalingChannel>& signaling,
              const std::string& localId, const std::string& remoteId,
              const std::vector<std::string>& endpoints) {
    for (const auto& endpoint : endpoints) {
        mywebrtc::SignalingMessage msg;
        msg.peer_id = localId;
        msg.type = "rtm";
        msg.candidate = endpoint;
        signaling->send(remoteId, msg);
    }
}

std::vector<std::string> gather(const std::shared_ptr<myrtm::Endpoint>& rtm,
                                const std::string& stunAddr) {
    std::vector<std::string> out;
    out.push_back("127.0.0.1:" + std::to_string(rtm->localPort()));

    if (stunAddr.empty())
        return out;

    if (!rtm->gatherPublicAddress(2000)) {
        say("rtm stun lookup failed against " + stunAddr);
        return out;
    }

    std::string seen = rtm->publicAddress();
    say("rtm public address is " + seen + " per " + stunAddr);
    if (!seen.empty() && seen != out.front())
        out.push_back(seen);
    return out;
}

}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cout << "usage: rtm-session <host|viewer> <localId> <remoteId> [wsUrl] [key] [stun]"
                  << std::endl;
        return 1;
    }

    std::string role = argv[1];
    std::string localId = argv[2];
    std::string remoteId = argv[3];
    std::string wsUrl = argc > 4 ? argv[4] : "ws://127.0.0.1:8000";
    std::string key = argc > 5 ? argv[5] : "rtm-session-key";
    std::string stunAddr = argc > 6 ? argv[6] : "";

    bool isHost = role == "host";
    if (!isHost && role != "viewer") {
        std::cerr << "role must be host or viewer" << std::endl;
        return 1;
    }

    rtc::InitLogger(rtc::LogLevel::Warning);
    std::signal(SIGINT, onInterrupt);

    auto signaling = std::make_shared<mywebrtc::WebSocketSignalingChannel>();
    auto auth = std::make_shared<mywebrtc::DefaultAuthPolicy>(key);
    auto sm = std::make_shared<mywebrtc::SessionManager>(auth, signaling);
    if (!stunAddr.empty())
        sm->setIceServers({"stun:" + stunAddr});

    if (!sm->init(wsUrl, localId, key)) {
        std::cerr << "signaling connect failed" << std::endl;
        return 1;
    }
    say("signaling up as " + localId + ", peer " + remoteId);

    myrtm::Config cfg;
    cfg.psk = key;
    if (!stunAddr.empty())
        cfg.stunServers.push_back(stunAddr);

    std::mutex rtmMtx;
    std::shared_ptr<myrtm::Endpoint> rtm;
    std::vector<std::string> localCandidates;

    if (isHost) {
        rtm = myrtm::Endpoint::listen(cfg);
        if (!rtm) {
            std::cerr << "rtm listen failed" << std::endl;
            return 1;
        }
        localCandidates = gather(rtm, stunAddr);
        rtm->onPeer([](myrtm::Endpoint::PeerId id, myrtm::PeerEvent e) {
            say("rtm peer " + std::to_string(id) + (e == myrtm::PeerEvent::Connected
                                                        ? " connected"
                                                        : " gone"));
        });
        rtm->onInput([](myrtm::Endpoint::PeerId, const myrtm::InputEvent&) { g_inputs++; });
        rtm->onAudio([](myrtm::Endpoint::PeerId, const myrtm::AudioFrame&) { g_frames++; });
        say("rtm listening on " + localCandidates.front());
    }

    std::vector<mywebrtc::Subscription> subs;

    subs.push_back(signaling->onMessage([&](const mywebrtc::SignalingMessage& msg) {
        if (msg.type != "rtm" || !msg.candidate)
            return;

        std::string peerHost;
        uint16_t peerPort = 0;
        if (!splitEndpoint(*msg.candidate, peerHost, peerPort))
            return;

        std::lock_guard<std::mutex> lk(rtmMtx);
        if (!rtm) {
            rtm = myrtm::Endpoint::connect(cfg, peerHost, peerPort);
            if (!rtm)
                return;
            localCandidates = gather(rtm, stunAddr);
            say("rtm dialing " + *msg.candidate + " from " + localCandidates.front());
            announce(signaling, localId, remoteId, localCandidates);
            return;
        }
        rtm->addRemoteCandidate(*msg.candidate);
        say("rtm candidate from peer: " + *msg.candidate);
    }));

    subs.push_back(sm->onSessionStateChange(
        [](const std::string& peer, mywebrtc::SessionState s) {
            say("webrtc session[" + peer + "] " + mywebrtc::to_string(s));
        }));

    subs.push_back(sm->onIncomingDataChannel(
        [&](const std::string& peer, std::shared_ptr<mywebrtc::DataChannel> dc) {
            say("webrtc datachannel from " + peer + " label=" + dc->label());
            subs.push_back(dc->onMessage([](rtc::binary b) { (void)b; g_dcMessages++; },
                                         [](std::string) { g_dcMessages++; }));
            dc->send("hello from " + localId);
        }));

    bool caller = localId < remoteId;
    std::shared_ptr<mywebrtc::Session> webrtcSession;
    if (caller) {
        webrtcSession = sm->offer(remoteId);
        if (!webrtcSession) {
            say("webrtc offer rejected");
        } else if (webrtcSession->dc) {
            subs.push_back(webrtcSession->dc->onMessage(
                [](rtc::binary b) { (void)b; g_dcMessages++; },
                [](std::string) { g_dcMessages++; }));
        }
    }

    if (isHost)
        announce(signaling, localId, remoteId, localCandidates);

    std::vector<uint8_t> opus(80, 0x3c);
    uint32_t rtpTs = 0;
    auto lastAudio = std::chrono::steady_clock::now();
    auto lastReport = lastAudio;
    auto lastAnnounce = lastAudio;
    int tick = 0;

    while (!g_stop.load()) {
        std::this_thread::sleep_for(8ms);
        auto now = std::chrono::steady_clock::now();

        std::shared_ptr<myrtm::Endpoint> ep;
        {
            std::lock_guard<std::mutex> lk(rtmMtx);
            ep = rtm;
        }

        if (ep && !isHost && ep->localPeer() != 0) {
            auto peer = ep->localPeer();

            myrtm::InputEvent mv;
            mv.kind = myrtm::InputKind::MouseMove;
            mv.x = (tick % 20) - 10;
            mv.y = (tick % 7) - 3;
            mv.timeMs = static_cast<uint32_t>(tick) * 8;
            ep->sendInput(peer, &mv, 1);
            tick++;

            if (now - lastAudio >= 20ms) {
                lastAudio = now;
                myrtm::AudioFrame f;
                f.codec = myrtm::AudioCodec::Opus;
                f.channels = 1;
                f.sampleRate = 48000;
                f.timestamp = rtpTs;
                f.payload = opus;
                rtpTs += 960;
                ep->sendAudio(peer, f);
            }
        }

        if (isHost && ep && ep->peers().empty() && now - lastAnnounce >= 1s) {
            lastAnnounce = now;
            announce(signaling, localId, remoteId, localCandidates);
        }

        if (webrtcSession && webrtcSession->dc && webrtcSession->dc->isOpen() &&
            now - lastReport >= 1s) {
            webrtcSession->dc->send("tick " + std::to_string(tick));
        }

        if (now - lastReport >= 1s) {
            lastReport = now;
            std::ostringstream os;
            os << "rtm peers=" << (ep ? ep->peers().size() : 0) << " inputs=" << g_inputs.load()
               << " frames=" << g_frames.load() << " | webrtc dc-msgs=" << g_dcMessages.load();
            say(os.str());
        }
    }

    {
        std::lock_guard<std::mutex> lk(rtmMtx);
        if (rtm)
            rtm->stop();
    }
    sm->closeSession(remoteId);
    return 0;
}
