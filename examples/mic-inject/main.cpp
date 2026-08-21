#include "myaudio/device.hpp"
#include "myaudio/mic.hpp"
#include "myaudio/share.hpp"
#include "myrtm/endpoint.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
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
std::mutex g_out;

extern "C" void onInterrupt(int) {
    g_stop.store(true);
}

void say(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_out);
    std::cout << line << std::endl;
}

bool numberArg(const char* text, long& out) {
    char* stop = nullptr;
    long value = std::strtol(text, &stop, 10);
    if (!stop || *stop != '\0')
        return false;
    out = value;
    return true;
}

void usage() {
    std::cout << "usage: mic-inject [--psk SECRET] [--port N] [--sink wasapi|shared|null]\n"
                 "                  [--device NAME] [--share NAME] [--mic NAME] [--rate N]\n"
                 "                  [--channels N] [--target MS] [--tone] [--list]\n"
                 "                  [--monitor NAME]"
              << std::endl;
}

std::string meter(float peak) {
    int lit = static_cast<int>(peak * 20.0f);
    if (lit > 20)
        lit = 20;
    std::string bar(static_cast<size_t>(lit < 0 ? 0 : lit), '#');
    bar.resize(20, '.');
    return bar;
}

void listDevices() {
    std::cout << "render endpoints:" << std::endl;
    for (const myaudio::DeviceInfo& d : myaudio::devices(false)) {
        std::cout << "  " << (d.isDefault ? "* " : "  ") << d.name << "  " << d.format.rate
                  << " Hz / " << static_cast<int>(d.format.channels) << " ch"
                  << (d.cable ? "  [cable]" : "") << std::endl;
    }
    std::cout << "capture endpoints:" << std::endl;
    for (const myaudio::DeviceInfo& d : myaudio::devices(true)) {
        std::cout << "  " << (d.isDefault ? "* " : "  ") << d.name << "  " << d.format.rate
                  << " Hz / " << static_cast<int>(d.format.channels) << " ch"
                  << (d.cable ? "  [cable]" : "") << std::endl;
    }
    std::vector<myaudio::MicRecord> mics = myaudio::registeredMics();
    std::cout << "registered mics: " << mics.size() << std::endl;
    for (const myaudio::MicRecord& m : mics) {
        std::cout << "  " << m.name << " sink=" << m.sink << " share=" << m.share
                  << " device=" << m.device << " pid=" << m.pid << std::endl;
    }
}

int monitor(const std::string& name) {
    std::shared_ptr<myaudio::ShareReader> reader = myaudio::ShareReader::open(name);
    if (!reader) {
        std::cerr << "cannot open share " << name << std::endl;
        return 1;
    }
    myaudio::Format fmt = reader->format();
    say("reading " + name + " at " + std::to_string(fmt.rate) + " Hz / " +
        std::to_string(static_cast<int>(fmt.channels)) + " ch");

    std::vector<float> buf(fmt.rate ? fmt.rate / 5 * fmt.channels : 4800);
    uint64_t total = 0;
    auto last = std::chrono::steady_clock::now();
    float peak = 0.0f;

    while (!g_stop.load()) {
        reader->wait(200);
        size_t got = reader->read(buf.data(), buf.size() / (fmt.channels ? fmt.channels : 1));
        for (size_t i = 0; i < got * fmt.channels; ++i) {
            float a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak)
                peak = a;
        }
        total += got;

        auto now = std::chrono::steady_clock::now();
        if (now - last >= 1s) {
            last = now;
            std::ostringstream os;
            os << "frames=" << total << " dropped=" << reader->dropped() << " [" << meter(peak)
               << "]";
            say(os.str());
            peak = 0.0f;
        }
    }
    return 0;
}

void console(const std::shared_ptr<myaudio::VirtualMic>& mic) {
    std::string line;
    while (!g_stop.load() && std::getline(std::cin, line)) {
        std::istringstream is(line);
        std::string cmd;
        is >> cmd;
        if (cmd.empty())
            continue;

        if (cmd == "quit" || cmd == "exit") {
            g_stop.store(true);
            return;
        }
        if (cmd == "list") {
            for (myaudio::VirtualMic::SourceId id : mic->sources()) {
                myaudio::SourceStats st;
                if (!mic->sourceStats(id, st))
                    continue;
                std::ostringstream os;
                os << "  peer " << id << " gain=" << st.gain << (st.muted ? " muted" : "")
                   << " depth=" << st.depthMs << "/" << st.targetMs << "ms drift=" << st.driftPpm
                   << "ppm lost=" << st.framesLost << " dry=" << st.underruns;
                say(os.str());
            }
            continue;
        }
        if (cmd == "vol") {
            double v = 1.0;
            if (is >> v) {
                mic->setMaster(static_cast<float>(v));
                say("master " + std::to_string(mic->master()));
            }
            continue;
        }

        long id = 0;
        if (!(is >> id)) {
            say("commands: list | mute ID | unmute ID | gain ID V | vol V | quit");
            continue;
        }
        myaudio::VirtualMic::SourceId peer = static_cast<myaudio::VirtualMic::SourceId>(id);

        if (cmd == "mute") {
            mic->setMute(peer, true);
            say("muted " + std::to_string(id));
        } else if (cmd == "unmute") {
            mic->setMute(peer, false);
            say("unmuted " + std::to_string(id));
        } else if (cmd == "gain") {
            double v = 1.0;
            if (is >> v) {
                mic->setGain(peer, static_cast<float>(v));
                say("gain " + std::to_string(id) + " -> " + std::to_string(v));
            }
        } else {
            say("commands: list | mute ID | unmute ID | gain ID V | vol V | quit");
        }
    }
}

}

int main(int argc, char* argv[]) {
    std::string psk = "cloudgame";
    std::string sink = "wasapi";
    std::string device;
    std::string share = "myaudio-mic";
    std::string micName = "MyMic";
    std::string watch;
    long port = 0;
    long rate = 48000;
    long channels = 2;
    long target = 60;
    bool tone = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        bool hasNext = i + 1 < argc;

        if (arg == "--list") {
            listDevices();
            return 0;
        } else if (arg == "--tone") {
            tone = true;
        } else if (arg == "--monitor" && hasNext) {
            watch = argv[++i];
        } else if (arg == "--psk" && hasNext) {
            psk = argv[++i];
        } else if (arg == "--sink" && hasNext) {
            sink = argv[++i];
        } else if (arg == "--device" && hasNext) {
            device = argv[++i];
        } else if (arg == "--share" && hasNext) {
            share = argv[++i];
        } else if (arg == "--mic" && hasNext) {
            micName = argv[++i];
        } else if (arg == "--port" && hasNext) {
            if (!numberArg(argv[++i], port) || port < 0 || port > 65535) {
                usage();
                return 1;
            }
        } else if (arg == "--rate" && hasNext) {
            if (!numberArg(argv[++i], rate) || rate < 8000 || rate > 192000) {
                usage();
                return 1;
            }
        } else if (arg == "--channels" && hasNext) {
            if (!numberArg(argv[++i], channels) || channels < 1 || channels > 2) {
                usage();
                return 1;
            }
        } else if (arg == "--target" && hasNext) {
            if (!numberArg(argv[++i], target) || target < 10 || target > 500) {
                usage();
                return 1;
            }
        } else {
            usage();
            return 1;
        }
    }

    std::signal(SIGINT, onInterrupt);
#ifdef SIGTERM
    std::signal(SIGTERM, onInterrupt);
#endif

    if (!watch.empty())
        return monitor(watch);

    myaudio::MicConfig cfg;
    cfg.mix.format.rate = static_cast<uint32_t>(rate);
    cfg.mix.format.channels = static_cast<uint8_t>(channels);
    cfg.pace.targetMs = static_cast<uint32_t>(target);
    cfg.sink.deviceMatch = device;
    cfg.sink.shareName = share;
    cfg.micName = micName;
    cfg.log = [](const std::string& text) { say(text); };

    if (sink == "null")
        cfg.sink.kind = myaudio::SinkKind::Null;
    else if (sink == "shared")
        cfg.sink.kind = myaudio::SinkKind::Shared;
    else if (sink == "wasapi")
        cfg.sink.kind = myaudio::SinkKind::Wasapi;
    else {
        usage();
        return 1;
    }

    std::shared_ptr<myaudio::VirtualMic> mic = myaudio::VirtualMic::start(cfg);
    if (!mic) {
        std::cerr << "virtual mic failed to start" << std::endl;
        return 1;
    }
    say(std::string("opus decode ") + (myaudio::opusAvailable() ? "on" : "off"));

    if (cfg.sink.kind == myaudio::SinkKind::Wasapi) {
        myaudio::DeviceInfo render;
        myaudio::DeviceInfo capture;
        if (myaudio::findCable(render, capture) && !capture.name.empty()) {
            say("games should pick microphone \"" + capture.name + "\"");
        } else {
            myaudio::DeviceInfo def;
            if (myaudio::defaultDevice(false, def))
                say("no virtual cable installed, feeding \"" + def.name +
                    "\" instead of a capture endpoint");
        }
    } else if (cfg.sink.kind == myaudio::SinkKind::Shared) {
        say("readers can attach to share " + mic->device());
    }

    std::shared_ptr<myrtm::Endpoint> ep;
    if (!tone) {
        myrtm::Config rtm;
        rtm.psk = psk;
        rtm.bindPort = static_cast<uint16_t>(port);
        ep = myrtm::Endpoint::listen(rtm);
        if (!ep) {
            std::cerr << "rtm listen failed" << std::endl;
            return 1;
        }
        say("rtm listening on udp/" + std::to_string(ep->localPort()));

        myaudio::VirtualMic* raw = mic.get();
        ep->onAudio([raw](myrtm::Endpoint::PeerId peer, const myrtm::AudioFrame& f) {
            raw->feed(peer, f);
        });
        ep->onPeer([raw](myrtm::Endpoint::PeerId peer, myrtm::PeerEvent e) {
            if (e == myrtm::PeerEvent::Connected) {
                say("peer " + std::to_string(peer) + " joined");
            } else {
                say("peer " + std::to_string(peer) + " left");
                raw->drop(peer);
            }
        });
    }

    std::thread cli(console, mic);

    double phase = 0.0;
    uint32_t ts = 0;
    auto lastTone = std::chrono::steady_clock::now();
    auto lastReport = lastTone;
    std::vector<uint8_t> payload(960 * 2);

    while (!g_stop.load()) {
        auto now = std::chrono::steady_clock::now();

        if (tone && now - lastTone >= 20ms) {
            lastTone += 20ms;
            for (size_t i = 0; i < 960; ++i) {
                double v = std::sin(phase) * 0.3;
                phase += 2.0 * 3.14159265358979323846 * 440.0 / 48000.0;
                int16_t s = static_cast<int16_t>(v * 32767.0);
                payload[i * 2] = static_cast<uint8_t>(s & 0xff);
                payload[i * 2 + 1] = static_cast<uint8_t>((s >> 8) & 0xff);
            }
            myrtm::AudioFrame f;
            f.codec = myrtm::AudioCodec::Pcm16;
            f.channels = 1;
            f.sampleRate = 48000;
            f.timestamp = ts;
            f.payload = payload;
            ts += 960;
            mic->feed(1, f);
        }

        if (now - lastReport >= 2s) {
            lastReport = now;
            myaudio::MicStats st = mic->stats();
            std::ostringstream os;
            os << "mix " << st.format.rate << "Hz/" << static_cast<int>(st.format.channels)
               << "ch on " << st.device << " | sources=" << st.sources
               << " frames=" << st.sinkFrames << " limited=" << st.limitedQuanta
               << " restarts=" << st.sinkRestarts << " [" << meter(st.peak) << "]";
            say(os.str());
            for (myaudio::VirtualMic::SourceId id : mic->sources()) {
                myaudio::SourceStats ss;
                if (!mic->sourceStats(id, ss))
                    continue;
                std::ostringstream line;
                line << "  peer " << id << " in=" << ss.framesIn << " lost=" << ss.framesLost
                     << " hid=" << ss.concealed << " dry=" << ss.underruns
                     << " over=" << ss.overruns << " depth=" << ss.depthMs << "/" << ss.targetMs
                     << "ms drift=" << ss.driftPpm << "ppm" << (ss.muted ? " muted" : "");
                say(line.str());
            }
        }

        std::this_thread::sleep_for(5ms);
    }

    if (ep)
        ep->stop();
    mic->stop();
    g_stop.store(true);
    std::cout.flush();
    cli.detach();
    std::_Exit(0);
}
