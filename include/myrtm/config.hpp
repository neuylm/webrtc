#ifndef MYRTM_CONFIG_H
#define MYRTM_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace myrtm {

struct Config {
    std::string psk;
    std::vector<std::string> stunServers;
    std::string bindAddress = "0.0.0.0";
    uint16_t bindPort = 0;
    uint16_t mtu = 1200;
    uint32_t controlRateBps = 512000;
    uint32_t audioBitrate = 96000;
    uint32_t audioJitterMs = 60;
    uint32_t audioDeadlineMs = 180;
    uint32_t handshakeTimeoutMs = 5000;
    uint32_t idleTimeoutMs = 15000;
    uint32_t pingIntervalMs = 1000;
    uint32_t maxPeers = 16;
};

enum class PeerEvent { Connected, Closed, TimedOut };

struct ChannelStats {
    uint64_t messagesSent = 0;
    uint64_t messagesRecv = 0;
    uint64_t packetsSent = 0;
    uint64_t packetsResent = 0;
    uint64_t packetsRecv = 0;
    uint64_t packetsExpired = 0;
    uint64_t packetsLost = 0;
    uint64_t bytesSent = 0;
    uint64_t bytesRecv = 0;
    uint32_t rttMs = 0;
    uint32_t rtoMs = 0;
    uint32_t rateBps = 0;
};

struct Stats {
    ChannelStats control;
    ChannelStats audio;
    uint32_t linkRttMs = 0;
    uint64_t authFailures = 0;
    uint64_t replayDrops = 0;
};

}

#endif
