#ifndef MYRTM_CHANNEL_H
#define MYRTM_CHANNEL_H

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "myrtm/config.hpp"

#include "wire.hpp"

namespace myrtm {

constexpr size_t kPacketOverhead = kHeaderSize + kDataHeaderSize + kTagSize;
constexpr uint32_t kMinRto = 40;
constexpr uint32_t kInitRto = 200;
constexpr uint32_t kMaxRto = 2000;
constexpr uint16_t kFastAckLimit = 2;
constexpr uint32_t kRecvWindow = 512;
constexpr size_t kSendBacklog = 4096;

struct Segment {
    uint32_t seq = 0;
    uint8_t frag = 0;
    bool sent = false;
    bool acked = false;
    uint16_t fastack = 0;
    uint32_t queuedAt = 0;
    uint32_t lastSent = 0;
    uint32_t rto = kInitRto;
    std::vector<uint8_t> data;
};

class SendChannel {
public:
    using Emit = std::function<void(const DataHeader&, const uint8_t*, size_t)>;

    SendChannel(uint8_t channel, size_t payloadLimit, uint32_t rateBps, uint32_t deadlineMs,
                bool allowFragment);

    bool push(const uint8_t* data, size_t len, uint32_t now);
    void flush(uint32_t now, const Emit& emit);
    void onAck(const AckBody& ack, uint32_t now);
    void fillStats(ChannelStats& s) const;

private:
    size_t wireSize(const Segment& s) const { return s.data.size() + kPacketOverhead; }
    void refill(uint32_t now);
    bool spend(size_t bytes);
    void expire(uint32_t now);
    void updateRtt(uint32_t sample);
    void adjustRate(uint32_t now, bool loss);
    size_t window() const;

    uint8_t channel_;
    size_t limit_;
    uint32_t deadline_;
    bool allowFragment_;
    uint32_t baseRate_;
    uint32_t rate_;
    double tokens_;
    uint32_t tokenAt_ = 0;
    bool tokenInit_ = false;
    uint32_t nextSeq_ = 1;
    uint32_t srtt_ = 0;
    uint32_t rttvar_ = 0;
    uint32_t rto_ = kInitRto;
    uint32_t lastCut_ = 0;
    uint32_t lastGrow_ = 0;
    std::deque<Segment> buf_;
    ChannelStats stats_;
};

class RecvChannel {
public:
    using Deliver = std::function<void(const uint8_t*, size_t)>;

    RecvChannel(uint8_t channel, bool ordered, uint32_t holdMs);

    void onData(const DataHeader& h, const uint8_t* p, size_t n, uint32_t now);
    void poll(uint32_t now, const Deliver& out);
    void setHold(uint32_t ms) { hold_ = ms; }
    bool ackPending() const { return ackPending_; }
    AckBody takeAck(uint32_t now);
    void fillStats(ChannelStats& s) const;

private:
    struct Item {
        uint8_t frag = 0;
        uint32_t arrival = 0;
        std::vector<uint8_t> data;
    };

    uint8_t channel_;
    bool ordered_;
    uint32_t hold_;
    uint32_t rcvNxt_ = 1;
    uint32_t echoTs_ = 0;
    uint32_t echoAt_ = 0;
    bool ackPending_ = false;
    std::map<uint32_t, Item> buf_;
    ChannelStats stats_;
};

}

#endif
