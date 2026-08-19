#include "mysignal/server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

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
    std::cout << "usage: signal-server [--bind ADDR] [--ws-port N] [--stun-port N] "
                 "[--token SECRET] [--quiet]"
              << std::endl;
}

}

int main(int argc, char* argv[]) {
    std::string bind;
    std::string token;
    long wsPort = 8000;
    long stunPort = 3478;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        bool hasNext = i + 1 < argc;

        if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "--bind" && hasNext) {
            bind = argv[++i];
        } else if (arg == "--token" && hasNext) {
            token = argv[++i];
        } else if (arg == "--ws-port" && hasNext) {
            if (!numberArg(argv[++i], wsPort) || wsPort < 0 || wsPort > 65535) {
                usage();
                return 1;
            }
        } else if (arg == "--stun-port" && hasNext) {
            if (!numberArg(argv[++i], stunPort) || stunPort < 0 || stunPort > 65535) {
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

    mysignal::LogSink log;
    if (!quiet)
        log = [](const std::string& line) { say(line); };

    mysignal::SignalConfig sig;
    sig.bindAddress = bind;
    sig.port = static_cast<uint16_t>(wsPort);
    sig.token = token;
    sig.log = log;

    auto signaling = mysignal::SignalServer::start(sig);
    if (!signaling) {
        std::cerr << "cannot bind signaling port " << wsPort << std::endl;
        return 1;
    }

    mysignal::StunConfig stun;
    stun.bindAddress = bind;
    stun.port = static_cast<uint16_t>(stunPort);
    stun.log = log;

    auto punching = mysignal::StunServer::start(stun);
    if (!punching) {
        std::cerr << "cannot bind stun port " << stunPort << std::endl;
        return 1;
    }

    say("signaling on ws://" + (bind.empty() ? std::string("0.0.0.0") : bind) + ":" +
        std::to_string(signaling->port()) + "/<id>" + (token.empty() ? "" : "?token=<secret>"));
    say("stun on udp " + (bind.empty() ? std::string("0.0.0.0") : bind) + ":" +
        std::to_string(punching->port()));

    auto lastReport = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        std::this_thread::sleep_for(100ms);

        auto now = std::chrono::steady_clock::now();
        if (now - lastReport < 10s)
            continue;
        lastReport = now;

        auto s = signaling->stats();
        auto t = punching->stats();
        say("clients=" + std::to_string(s.clients) + " relayed=" + std::to_string(s.relayed) +
            " held=" + std::to_string(s.queued) + " flushed=" + std::to_string(s.flushed) +
            " dropped=" + std::to_string(s.dropped) + " rejected=" + std::to_string(s.rejected) +
            " | stun req=" + std::to_string(t.requests) + " ans=" + std::to_string(t.responses));
    }

    say("shutting down");
    punching->stop();
    signaling->stop();
    return 0;
}
