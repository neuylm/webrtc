#include "myrtm/endpoint.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

using namespace myrtm;
using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_inputs{0};
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_audioBytes{0};
std::mutex g_out;

extern "C" void onInterrupt(int) {
    g_stop.store(true);
}

void say(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_out);
    std::cout << line << std::endl;
}

const char* eventName(PeerEvent e) {
    switch (e) {
    case PeerEvent::Connected:
        return "connected";
    case PeerEvent::Closed:
        return "closed";
    case PeerEvent::TimedOut:
        return "timeout";
    }
    return "?";
}

std::string sizeText(uint64_t n) {
    if (n < 1024)
        return std::to_string(n) + "B";
    if (n < 1024 * 1024)
        return std::to_string(n / 1024) + "K";
    return std::to_string(n / (1024 * 1024)) + "M";
}

void printStats(const std::shared_ptr<Endpoint>& ep, Endpoint::PeerId peer) {
    Stats st;
    if (!ep->stats(peer, st))
        return;

    std::ostringstream os;
    os << "[peer " << peer << "] link=" << st.linkRttMs << "ms"
       << " | ctl rtt=" << st.control.rttMs << "ms rto=" << st.control.rtoMs
       << "ms msg=" << st.control.messagesSent << "/" << st.control.messagesRecv
       << " pkt=" << st.control.packetsSent << " resent=" << st.control.packetsResent
       << " | audio " << st.audio.rateBps / 1000 << "kbps pkt=" << st.audio.packetsSent << "/"
       << st.audio.packetsRecv << " resent=" << st.audio.packetsResent
       << " expired=" << st.audio.packetsExpired << " lost=" << st.audio.packetsLost << " | up "
       << sizeText(st.control.bytesSent + st.audio.bytesSent) << " down "
       << sizeText(st.control.bytesRecv + st.audio.bytesRecv) << " bad=" << st.authFailures << "/"
       << st.replayDrops << " | in=" << g_inputs.load() << " frames=" << g_frames.load() << " "
       << sizeText(g_audioBytes.load());
    say(os.str());
}

int runHost(uint16_t port, const std::string& key) {
    Config cfg;
    cfg.psk = key;
    cfg.bindPort = port;

    auto ep = Endpoint::listen(cfg);
    if (!ep) {
        std::cerr << "bind failed on port " << port << std::endl;
        return 1;
    }
    say("host listening on udp/" + std::to_string(ep->localPort()));

    ep->onPeer([](Endpoint::PeerId id, PeerEvent e) {
        say("peer " + std::to_string(id) + " " + eventName(e));
    });
    ep->onInput([](Endpoint::PeerId, const InputEvent& ev) {
        g_inputs++;
        if (ev.kind == InputKind::KeyDown)
            say("key down code=" + std::to_string(ev.code));
    });
    ep->onAudio([](Endpoint::PeerId, const AudioFrame& f) {
        g_frames++;
        g_audioBytes += f.payload.size();
    });
    ep->onControl([](Endpoint::PeerId id, const uint8_t* p, size_t n) {
        say("control from " + std::to_string(id) + ": " +
            std::string(reinterpret_cast<const char*>(p), n));
    });

    while (!g_stop.load()) {
        std::this_thread::sleep_for(1s);
        for (auto id : ep->peers())
            printStats(ep, id);
    }
    ep->stop();
    return 0;
}

int runViewer(const std::string& host, uint16_t port, const std::string& key) {
    Config cfg;
    cfg.psk = key;

    auto ep = Endpoint::connect(cfg, host, port);
    if (!ep) {
        std::cerr << "connect failed" << std::endl;
        return 1;
    }
    ep->onPeer([](Endpoint::PeerId id, PeerEvent e) {
        say("peer " + std::to_string(id) + " " + eventName(e));
    });

    if (!ep->waitConnected(5000)) {
        std::cerr << "handshake timeout" << std::endl;
        return 1;
    }
    auto peer = ep->localPeer();
    say("connected as " + std::to_string(peer) + " from udp/" + std::to_string(ep->localPort()));

    std::vector<uint8_t> opus(80, 0x3c);
    uint32_t rtpTs = 0;
    auto lastAudio = std::chrono::steady_clock::now();
    auto lastReport = lastAudio;
    int tick = 0;

    while (!g_stop.load()) {
        std::this_thread::sleep_for(8ms);

        InputEvent mv;
        mv.kind = InputKind::MouseMove;
        mv.x = (tick % 20) - 10;
        mv.y = (tick % 7) - 3;
        mv.timeMs = static_cast<uint32_t>(tick) * 8;
        ep->sendInput(peer, &mv, 1);

        if (++tick % 40 == 0) {
            InputEvent key;
            key.kind = InputKind::KeyDown;
            key.code = 0x41;
            ep->sendInput(peer, &key, 1);
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastAudio >= 20ms) {
            lastAudio = now;
            AudioFrame f;
            f.codec = AudioCodec::Opus;
            f.channels = 1;
            f.sampleRate = 48000;
            f.timestamp = rtpTs;
            f.payload = opus;
            rtpTs += 960;
            ep->sendAudio(peer, f);
        }
        if (now - lastReport >= 1s) {
            lastReport = now;
            printStats(ep, peer);
        }
    }
    ep->stop();
    return 0;
}

}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "usage:\n"
                  << "  rtm-demo host [port] [key]\n"
                  << "  rtm-demo viewer <host> <port> [key]" << std::endl;
        return 1;
    }

    std::signal(SIGINT, onInterrupt);
    std::signal(SIGTERM, onInterrupt);

    std::string mode = argv[1];
    if (mode == "host") {
        uint16_t port = argc > 2 ? static_cast<uint16_t>(std::atoi(argv[2])) : 7500;
        std::string key = argc > 3 ? argv[3] : "rtm-demo-key";
        return runHost(port, key);
    }
    if (mode == "viewer") {
        if (argc < 4) {
            std::cerr << "viewer needs host and port" << std::endl;
            return 1;
        }
        uint16_t port = static_cast<uint16_t>(std::atoi(argv[3]));
        std::string key = argc > 4 ? argv[4] : "rtm-demo-key";
        return runViewer(argv[2], port, key);
    }

    std::cerr << "unknown mode " << mode << std::endl;
    return 1;
}
