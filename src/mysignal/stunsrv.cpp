#include "mysignal/server.hpp"

#include "net.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mysignal {
namespace {

const size_t kStunHeader = 20;
const size_t kStunTxid = 12;
const uint32_t kStunCookie = 0x2112a442;
const uint16_t kBindingRequest = 0x0001;
const uint16_t kBindingSuccess = 0x0101;
const uint16_t kAttrMappedAddress = 0x0001;
const uint16_t kAttrXorMappedAddress = 0x0020;

void putU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v & 0xff));
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t((v >> 24) & 0xff));
    out.push_back(uint8_t((v >> 16) & 0xff));
    out.push_back(uint8_t((v >> 8) & 0xff));
    out.push_back(uint8_t(v & 0xff));
}

bool looksLikeRequest(const uint8_t* p, size_t n) {
    if (n < kStunHeader || (n % 4) != 0)
        return false;
    if (p[0] & 0xc0)
        return false;

    uint16_t type = uint16_t((uint16_t(p[0]) << 8) | p[1]);
    if (type != kBindingRequest)
        return false;

    uint16_t length = uint16_t((uint16_t(p[2]) << 8) | p[3]);
    if (size_t(length) + kStunHeader != n)
        return false;

    uint32_t cookie = (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) | (uint32_t(p[6]) << 8) | p[7];
    if (cookie != kStunCookie)
        return false;

    size_t cursor = kStunHeader;
    while (cursor + 4 <= n) {
        uint16_t alen = uint16_t((uint16_t(p[cursor + 2]) << 8) | p[cursor + 3]);
        size_t padded = (size_t(alen) + 3u) & ~size_t(3u);
        if (cursor + 4 + padded > n)
            return false;
        cursor += 4 + padded;
    }
    return cursor == n;
}

void addAddress(std::vector<uint8_t>& out, uint16_t attr, int family, const uint8_t addr[16],
                uint16_t port, const uint8_t* txid) {
    bool v6 = family == AF_INET6;
    uint16_t bodyLen = uint16_t(v6 ? 20 : 8);

    putU16(out, attr);
    putU16(out, bodyLen);
    out.push_back(0);
    out.push_back(uint8_t(v6 ? 0x02 : 0x01));

    bool xored = attr == kAttrXorMappedAddress;
    uint8_t mask[16];
    mask[0] = uint8_t((kStunCookie >> 24) & 0xff);
    mask[1] = uint8_t((kStunCookie >> 16) & 0xff);
    mask[2] = uint8_t((kStunCookie >> 8) & 0xff);
    mask[3] = uint8_t(kStunCookie & 0xff);
    std::memcpy(mask + 4, txid, kStunTxid);

    putU16(out, xored ? uint16_t(port ^ (kStunCookie >> 16)) : port);

    size_t width = v6 ? 16u : 4u;
    for (size_t i = 0; i < width; ++i)
        out.push_back(uint8_t(addr[i] ^ (xored ? mask[i] : 0)));
}

bool buildSuccess(const uint8_t* request, const Peer& from, std::vector<uint8_t>& out) {
    int family = 0;
    uint8_t addr[16] = {0};
    uint16_t port = 0;
    if (!from.split(family, addr, port))
        return false;

    const uint8_t* txid = request + 8;

    std::vector<uint8_t> body;
    addAddress(body, kAttrXorMappedAddress, family, addr, port, txid);
    addAddress(body, kAttrMappedAddress, family, addr, port, txid);

    out.clear();
    putU16(out, kBindingSuccess);
    putU16(out, uint16_t(body.size()));
    putU32(out, kStunCookie);
    out.insert(out.end(), txid, txid + kStunTxid);
    out.insert(out.end(), body.begin(), body.end());
    return true;
}

}

struct StunServer::Impl {
    StunConfig cfg;
    Datagram sock;
    std::thread worker;
    std::atomic<bool> running{false};

    mutable std::mutex mtx;
    StunStats shared;

    void run() {
        std::vector<uint8_t> buf(2048);
        std::vector<uint8_t> reply;
        StunStats tally;

        while (running.load()) {
            Peer from;
            int n = sock.recv(buf.data(), buf.size(), from, 50);
            if (n < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (n == 0)
                continue;

            if (!looksLikeRequest(buf.data(), size_t(n))) {
                tally.ignored++;
            } else {
                tally.requests++;
                if (buildSuccess(buf.data(), from, reply) &&
                    sock.send(reply.data(), reply.size(), from)) {
                    tally.responses++;
                    if (cfg.log)
                        cfg.log("told " + from.text() + " where it came from");
                }
            }

            std::lock_guard<std::mutex> lk(mtx);
            shared = tally;
        }
    }
};

StunServer::StunServer() : impl_(std::make_unique<Impl>()) {}

StunServer::~StunServer() {
    stop();
}

std::shared_ptr<StunServer> StunServer::start(const StunConfig& cfg) {
    std::shared_ptr<StunServer> self(new StunServer());
    self->impl_->cfg = cfg;

    if (!self->impl_->sock.open(cfg.bindAddress, cfg.port))
        return nullptr;

    Impl* impl = self->impl_.get();
    impl->running.store(true);
    impl->worker = std::thread([impl] { impl->run(); });
    return self;
}

uint16_t StunServer::port() const {
    return impl_->sock.port();
}

StunStats StunServer::stats() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->shared;
}

void StunServer::stop() {
    if (!impl_->running.exchange(false))
        return;
    if (impl_->worker.joinable())
        impl_->worker.join();
    impl_->sock.shut();
}

}
