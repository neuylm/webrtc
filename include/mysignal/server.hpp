#ifndef MYSIGNAL_SERVER_H
#define MYSIGNAL_SERVER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mysignal {

using LogSink = std::function<void(const std::string&)>;

struct SignalConfig {
    std::string bindAddress;
    uint16_t port = 8000;
    std::string token;
    size_t maxClients = 256;
    size_t maxQueued = 32;
    uint32_t queueTtlMs = 30000;
    size_t maxMessageSize = 256 * 1024;
    uint32_t idleTimeoutMs = 120000;
    LogSink log;
};

struct SignalStats {
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t relayed = 0;
    uint64_t queued = 0;
    uint64_t flushed = 0;
    uint64_t dropped = 0;
    size_t clients = 0;
};

class SignalServer {
public:
    static std::shared_ptr<SignalServer> start(const SignalConfig& cfg);
    ~SignalServer();

    SignalServer(const SignalServer&) = delete;
    SignalServer& operator=(const SignalServer&) = delete;

    uint16_t port() const;
    SignalStats stats() const;
    std::vector<std::string> clients() const;
    void stop();

private:
    SignalServer();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct StunConfig {
    std::string bindAddress;
    uint16_t port = 3478;
    LogSink log;
};

struct StunStats {
    uint64_t requests = 0;
    uint64_t responses = 0;
    uint64_t ignored = 0;
};

class StunServer {
public:
    static std::shared_ptr<StunServer> start(const StunConfig& cfg);
    ~StunServer();

    StunServer(const StunServer&) = delete;
    StunServer& operator=(const StunServer&) = delete;

    uint16_t port() const;
    StunStats stats() const;
    void stop();

private:
    StunServer();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
