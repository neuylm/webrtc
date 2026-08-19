#include "channel.hpp"

#include "util.hpp"

#include <algorithm>

namespace myrtm {

SendChannel::SendChannel(uint8_t channel, size_t payloadLimit, uint32_t rateBps,
                         uint32_t deadlineMs, bool allowFragment)
    : channel_(channel), limit_(payloadLimit), deadline_(deadlineMs),
      allowFragment_(allowFragment), baseRate_(rateBps), rate_(rateBps),
      tokens_(static_cast<double>(payloadLimit + kPacketOverhead) * 2.0) {}

bool SendChannel::push(const uint8_t* data, size_t len, uint32_t now) {
    if (!allowFragment_ && len > limit_)
        return false;

    size_t parts = len ? (len + limit_ - 1) / limit_ : 1;
    if (parts > 255)
        return false;
    if (buf_.size() + parts > kSendBacklog)
        return false;

    size_t off = 0;
    for (size_t i = 0; i < parts; ++i) {
        size_t take = std::min(limit_, len - off);
        Segment s;
        s.seq = nextSeq_++;
        s.frag = static_cast<uint8_t>(parts - 1 - i);
        s.queuedAt = now;
        s.data.assign(data + off, data + off + take);
        off += take;
        buf_.push_back(std::move(s));
    }
    stats_.messagesSent++;
    return true;
}

void SendChannel::refill(uint32_t now) {
    if (!tokenInit_) {
        tokenAt_ = now;
        tokenInit_ = true;
        return;
    }
    int32_t dt = seqDiff(now, tokenAt_);
    if (dt <= 0)
        return;
    tokenAt_ = now;
    tokens_ += static_cast<double>(rate_) * dt / 8000.0;
    double cap = static_cast<double>(limit_ + kPacketOverhead) * 4.0;
    if (tokens_ > cap)
        tokens_ = cap;
}

bool SendChannel::spend(size_t bytes) {
    if (tokens_ < static_cast<double>(bytes))
        return false;
    tokens_ -= static_cast<double>(bytes);
    return true;
}

size_t SendChannel::window() const {
    size_t unit = limit_ + kPacketOverhead;
    if (deadline_ == 0)
        return unit * 64;
    uint32_t rtt = srtt_ ? srtt_ : 50;
    size_t bytes = static_cast<size_t>(static_cast<uint64_t>(rate_) * rtt / 8000);
    return std::max(bytes, unit * 3);
}

void SendChannel::expire(uint32_t now) {
    while (!buf_.empty()) {
        Segment& f = buf_.front();
        if (f.acked) {
            buf_.pop_front();
            continue;
        }
        if (deadline_ && seqDiff(now, f.queuedAt) > static_cast<int32_t>(deadline_)) {
            stats_.packetsExpired++;
            buf_.pop_front();
            continue;
        }
        break;
    }
}

void SendChannel::updateRtt(uint32_t sample) {
    if (srtt_ == 0) {
        srtt_ = sample;
        rttvar_ = sample / 2;
    } else {
        uint32_t delta = sample > srtt_ ? sample - srtt_ : srtt_ - sample;
        rttvar_ = (3 * rttvar_ + delta) / 4;
        srtt_ = (7 * srtt_ + sample) / 8;
    }
    if (srtt_ == 0)
        srtt_ = 1;
    uint32_t spread = std::max<uint32_t>(rttvar_ * 4, 10);
    rto_ = std::min(std::max(srtt_ + spread, kMinRto), kMaxRto);
}

void SendChannel::adjustRate(uint32_t now, bool loss) {
    if (deadline_ == 0)
        return;
    uint32_t interval = std::max<uint32_t>(srtt_ ? srtt_ : 100, 50);
    if (loss) {
        if (seqDiff(now, lastCut_) >= static_cast<int32_t>(interval)) {
            uint32_t floorRate = std::max<uint32_t>(baseRate_ / 2, 8000);
            rate_ = std::max(rate_ * 85 / 100, floorRate);
            lastCut_ = now;
            lastGrow_ = now;
        }
        return;
    }
    if (seqDiff(now, lastGrow_) >= static_cast<int32_t>(interval)) {
        uint32_t cap = baseRate_ * 2;
        rate_ = std::min(rate_ + 4000, cap);
        lastGrow_ = now;
    }
}

void SendChannel::flush(uint32_t now, const Emit& emit) {
    refill(now);
    expire(now);

    auto transmit = [&](const Segment& s) {
        DataHeader h;
        h.channel = channel_;
        h.frag = s.frag;
        h.seq = s.seq;
        h.sendTs = now;
        emit(h, s.data.data(), s.data.size());
    };

    size_t inflight = 0;
    for (const Segment& s : buf_) {
        if (s.sent && !s.acked)
            inflight += wireSize(s);
    }

    bool loss = false;
    for (Segment& s : buf_) {
        if (!s.sent || s.acked)
            continue;
        int32_t idle = seqDiff(now, s.lastSent);
        bool timeout = idle >= static_cast<int32_t>(s.rto);
        bool fast = s.fastack >= kFastAckLimit &&
                    idle >= static_cast<int32_t>(std::max<uint32_t>(srtt_ / 2, 10));
        if (!timeout && !fast)
            continue;

        size_t need = wireSize(s);
        if (!spend(need))
            break;

        s.lastSent = now;
        s.fastack = 0;
        if (timeout)
            s.rto = std::min<uint32_t>(s.rto * 2, kMaxRto);
        loss = true;

        transmit(s);
        stats_.packetsSent++;
        stats_.packetsResent++;
        stats_.bytesSent += need;
    }

    size_t limit = window();
    uint32_t oldest = buf_.empty() ? nextSeq_ : buf_.front().seq;
    for (Segment& s : buf_) {
        if (s.sent)
            continue;
        if (seqDiff(s.seq, oldest) >= static_cast<int32_t>(kRecvWindow))
            break;
        size_t need = wireSize(s);
        if (inflight + need > limit)
            break;
        if (!spend(need))
            break;

        s.sent = true;
        s.lastSent = now;
        s.rto = rto_;
        inflight += need;

        transmit(s);
        stats_.packetsSent++;
        stats_.bytesSent += need;
    }

    adjustRate(now, loss);
}

void SendChannel::onAck(const AckBody& ack, uint32_t now) {
    if (ack.echoTs) {
        int32_t sample = seqDiff(now, ack.echoTs) - static_cast<int32_t>(ack.delay);
        if (sample >= 0 && sample < 60000)
            updateRtt(static_cast<uint32_t>(sample));
    }

    uint32_t highest = ack.una;
    for (Segment& s : buf_) {
        if (s.acked)
            continue;
        if (seqLess(s.seq, ack.una)) {
            s.acked = true;
            continue;
        }
        uint32_t off = s.seq - ack.una;
        if (off < 32 && (ack.bitmap & (1u << off))) {
            s.acked = true;
            if (seqLess(highest, s.seq + 1))
                highest = s.seq + 1;
        }
    }

    for (Segment& s : buf_) {
        if (!s.acked && s.sent && seqLess(s.seq, highest))
            s.fastack++;
    }

    expire(now);
}

void SendChannel::fillStats(ChannelStats& s) const {
    s = stats_;
    s.rttMs = srtt_;
    s.rtoMs = rto_;
    s.rateBps = rate_;
}

RecvChannel::RecvChannel(uint8_t channel, bool ordered, uint32_t holdMs)
    : channel_(channel), ordered_(ordered), hold_(holdMs) {}

void RecvChannel::onData(const DataHeader& h, const uint8_t* p, size_t n, uint32_t now) {
    stats_.packetsRecv++;
    stats_.bytesRecv += n + kPacketOverhead;

    echoTs_ = h.sendTs;
    echoAt_ = now;
    ackPending_ = true;

    if (!ordered_ && h.frag != 0)
        return;
    if (seqLess(h.seq, rcvNxt_))
        return;
    if (!seqLess(h.seq, rcvNxt_ + kRecvWindow)) {
        if (ordered_)
            return;
        int32_t jump = seqDiff(h.seq, rcvNxt_);
        if (jump > static_cast<int32_t>(kSendBacklog))
            return;
        stats_.packetsLost += static_cast<uint32_t>(jump);
        rcvNxt_ = h.seq;
        buf_.clear();
    }
    if (buf_.find(h.seq) != buf_.end())
        return;

    Item item;
    item.frag = h.frag;
    item.arrival = now;
    item.data.assign(p, p + n);
    buf_.emplace(h.seq, std::move(item));
}

void RecvChannel::poll(uint32_t now, const Deliver& out) {
    while (!buf_.empty()) {
        auto head = buf_.lower_bound(rcvNxt_);
        if (head == buf_.end())
            head = buf_.begin();

        if (head->first != rcvNxt_) {
            if (ordered_)
                break;
            if (seqDiff(now, head->second.arrival) < static_cast<int32_t>(hold_))
                break;
            stats_.packetsLost += static_cast<uint32_t>(seqDiff(head->first, rcvNxt_));
            rcvNxt_ = head->first;
            continue;
        }

        uint32_t count = static_cast<uint32_t>(head->second.frag) + 1;
        bool complete = true;
        bool broken = false;
        uint32_t newest = head->second.arrival;
        for (uint32_t k = 0; k < count; ++k) {
            auto it = buf_.find(rcvNxt_ + k);
            if (it == buf_.end()) {
                complete = false;
                break;
            }
            if (it->second.frag != static_cast<uint8_t>(count - 1 - k)) {
                broken = true;
                break;
            }
            if (seqDiff(it->second.arrival, newest) > 0)
                newest = it->second.arrival;
        }

        if (broken) {
            buf_.erase(rcvNxt_);
            rcvNxt_++;
            stats_.packetsLost++;
            continue;
        }

        if (!complete) {
            if (ordered_)
                break;
            if (seqDiff(now, head->second.arrival) < static_cast<int32_t>(hold_))
                break;
            buf_.erase(rcvNxt_);
            rcvNxt_++;
            stats_.packetsLost++;
            continue;
        }

        if (!ordered_ && seqDiff(now, newest) < static_cast<int32_t>(hold_))
            break;

        std::vector<uint8_t> msg;
        for (uint32_t k = 0; k < count; ++k) {
            auto it = buf_.find(rcvNxt_ + k);
            msg.insert(msg.end(), it->second.data.begin(), it->second.data.end());
            buf_.erase(it);
        }
        rcvNxt_ += count;
        stats_.messagesRecv++;
        out(msg.data(), msg.size());
    }
}

AckBody RecvChannel::takeAck(uint32_t now) {
    AckBody a;
    a.channel = channel_;
    a.una = rcvNxt_;
    a.bitmap = 0;
    for (uint32_t i = 0; i < 32; ++i) {
        if (buf_.find(rcvNxt_ + i) != buf_.end())
            a.bitmap |= (1u << i);
    }
    a.echoTs = echoTs_;
    int32_t d = seqDiff(now, echoAt_);
    a.delay = (d > 0 && d < 0xffff) ? static_cast<uint16_t>(d) : 0;
    ackPending_ = false;
    return a;
}

void RecvChannel::fillStats(ChannelStats& s) const {
    s.messagesRecv = stats_.messagesRecv;
    s.packetsRecv = stats_.packetsRecv;
    s.packetsLost = stats_.packetsLost;
    s.bytesRecv = stats_.bytesRecv;
}

}
