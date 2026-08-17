#include "mysignal/server.hpp"

#include "frame.hpp"
#include "handshake.hpp"
#include "jsonpick.hpp"
#include "net.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace mysignal {
namespace {

const size_t kHeadLimit = 8192;
const size_t kOutLimit = 4 * 1024 * 1024;
const size_t kMailboxCap = 256;
const size_t kHeldBudget = 8 * 1024 * 1024;
const uint64_t kShakeTimeoutMs = 10000;
const uint64_t kLingerMs = 2000;
const int kPollWaitMs = 50;

SignalConfig sanitize(const SignalConfig& in) {
    SignalConfig out = in;
    out.maxClients = std::min<size_t>(std::max<size_t>(out.maxClients, 1), 4096);
    out.maxQueued = std::min<size_t>(out.maxQueued, 1024);
    out.maxMessageSize =
        std::min<size_t>(std::max<size_t>(out.maxMessageSize, 1024), 8 * 1024 * 1024);
    if (out.idleTimeoutMs < 4000)
        out.idleTimeoutMs = 4000;
    if (out.queueTtlMs < 1000)
        out.queueTtlMs = 1000;
    return out;
}

}

struct SignalServer::Impl {
    struct Conn {
        Handle fd = kNoHandle;
        std::string who;
        std::string id;
        std::string in;
        std::vector<uint8_t> out;
        std::vector<uint8_t> msg;
        uint8_t msgOp = 0;
        bool shook = false;
        bool assembling = false;
        bool closing = false;
        bool dying = false;
        bool pinged = false;
        uint64_t lastIn = 0;
        uint64_t closedAt = 0;
    };

    struct Pending {
        uint64_t expire = 0;
        std::string text;
    };

    SignalConfig cfg;
    Listener door;
    std::thread worker;
    std::atomic<bool> running{false};

    std::vector<std::unique_ptr<Conn>> conns;
    std::unordered_map<std::string, Conn*> byId;
    std::unordered_map<std::string, std::deque<Pending>> mailbox;
    size_t heldBytes = 0;
    SignalStats tally;

    mutable std::mutex mtx;
    SignalStats shared;
    std::vector<std::string> roster;

    void note(const std::string& line) {
        if (cfg.log)
            cfg.log(line);
    }

    void run() {
        std::vector<PollSlot> slots;
        while (running.load()) {
            slots.clear();
            PollSlot entry;
            std::memset(&entry, 0, sizeof(entry));
            entry.fd = door.handle();
            entry.events = POLLIN;
            slots.push_back(entry);

            for (auto& c : conns) {
                PollSlot slot;
                std::memset(&slot, 0, sizeof(slot));
                slot.fd = c->fd;
                slot.events = POLLIN;
                if (!c->out.empty())
                    slot.events = short(slot.events | POLLOUT);
                slots.push_back(slot);
            }

            size_t watched = slots.size() - 1;
            int ready = waitOn(slots.data(), slots.size(), kPollWaitMs);
            uint64_t now = nowMs();

            if (ready > 0) {
                for (size_t i = 0; i < watched; ++i) {
                    short ev = slots[i + 1].revents;
                    if (ev == 0)
                        continue;
                    Conn& c = *conns[i];
                    if (ev & (POLLERR | POLLNVAL)) {
                        c.dying = true;
                        continue;
                    }
                    if (ev & POLLIN)
                        feed(c, now);
                    if (!c.dying && (ev & POLLOUT))
                        drain(c);
                    if (ev & POLLHUP)
                        c.dying = true;
                }
                if (slots[0].revents & POLLIN)
                    admit(now);
            }

            sweep(now);
            reap();
            publish();
        }
    }

    void admit(uint64_t now) {
        for (int i = 0; i < 16; ++i) {
            Peer from;
            Handle fd = door.take(from);
            if (fd == kNoHandle)
                return;

            if (conns.size() >= cfg.maxClients) {
                closeHandle(fd);
                tally.rejected++;
                note("refused a connection, client limit reached");
                continue;
            }

            auto c = std::make_unique<Conn>();
            c->fd = fd;
            c->who = from.text();
            c->lastIn = now;
            conns.push_back(std::move(c));
        }
    }

    void feed(Conn& c, uint64_t now) {
        uint8_t buf[8192];
        for (int loops = 0; loops < 8; ++loops) {
            int n = pullBytes(c.fd, buf, sizeof(buf));
            if (n < 0) {
                c.dying = true;
                return;
            }
            if (n == 0)
                break;
            c.lastIn = now;
            c.pinged = false;
            c.in.append(reinterpret_cast<const char*>(buf), size_t(n));
        }

        if (c.closing) {
            c.in.clear();
            return;
        }
        if (!c.shook)
            shakeHands(c, now);
        if (c.shook && !c.closing)
            chew(c);
    }

    void shakeHands(Conn& c, uint64_t now) {
        Upgrade up;
        size_t used = 0;
        UpgradeState st = readUpgrade(c.in, used, up);

        if (st == UpgradeState::NeedMore) {
            if (c.in.size() > kHeadLimit)
                refuse(c, 431, "Request Header Fields Too Large", now);
            return;
        }
        if (st == UpgradeState::Bad || !validId(up.id)) {
            refuse(c, 400, "Bad Request", now);
            return;
        }
        if (!cfg.token.empty() && !sameSecret(cfg.token, up.token)) {
            note("rejected " + c.who + ", bad token");
            refuse(c, 401, "Unauthorized", now);
            return;
        }

        c.in.erase(0, used);
        std::string reply = buildAccept(up.key);
        c.out.insert(c.out.end(), reply.begin(), reply.end());
        c.id = up.id;
        c.shook = true;
        tally.accepted++;

        auto old = byId.find(c.id);
        if (old != byId.end() && old->second != &c) {
            note("id " + c.id + " reclaimed, dropping " + old->second->who);
            goodbye(*old->second, 1001, now);
        }
        byId[c.id] = &c;
        note("client " + c.id + " online from " + c.who);

        drain(c);
        deliver(c, now);
    }

    void chew(Conn& c) {
        while (!c.dying && !c.closing) {
            Frame f;
            size_t used = 0;
            FrameState st = readFrame(reinterpret_cast<const uint8_t*>(c.in.data()), c.in.size(),
                                      cfg.maxMessageSize, true, f, used);

            if (st == FrameState::NeedMore) {
                if (c.in.size() > cfg.maxMessageSize + 64)
                    quit(c, 1009);
                return;
            }
            if (st == FrameState::Bad) {
                quit(c, 1002);
                return;
            }
            c.in.erase(0, used);

            if (f.opcode == kOpPing) {
                writeFrame(kOpPong, f.payload.data(), f.payload.size(), c.out);
                continue;
            }
            if (f.opcode == kOpPong)
                continue;
            if (f.opcode == kOpClose) {
                quit(c, 1000);
                return;
            }

            if (f.opcode == kOpContinuation) {
                if (!c.assembling) {
                    quit(c, 1002);
                    return;
                }
            } else {
                if (c.assembling) {
                    quit(c, 1002);
                    return;
                }
                c.assembling = true;
                c.msgOp = f.opcode;
                c.msg.clear();
            }

            if (c.msg.size() + f.payload.size() > cfg.maxMessageSize) {
                quit(c, 1009);
                return;
            }
            c.msg.insert(c.msg.end(), f.payload.begin(), f.payload.end());
            if (!f.fin)
                continue;

            c.assembling = false;
            if (c.msgOp == kOpText)
                relay(c, std::string(c.msg.begin(), c.msg.end()));
            c.msg.clear();
        }
    }

    void relay(Conn& from, const std::string& text) {
        std::string dest;
        Span span;
        if (!pickTopString(text, "id", dest, span)) {
            note("dropping a message from " + from.id + " without a target id");
            return;
        }
        if (!validId(dest))
            return;

        std::string rewritten = splice(text, span, quoteJson(from.id));

        auto it = byId.find(dest);
        if (it != byId.end() && !it->second->dying && !it->second->closing) {
            post(*it->second, rewritten);
            tally.relayed++;
            return;
        }
        stash(dest, rewritten);
    }

    void toss(std::deque<Pending>& box) {
        heldBytes -= box.front().text.size();
        box.pop_front();
        tally.dropped++;
    }

    void stash(const std::string& dest, const std::string& text) {
        if (cfg.maxQueued == 0) {
            tally.dropped++;
            return;
        }
        if (mailbox.size() >= kMailboxCap && mailbox.find(dest) == mailbox.end()) {
            tally.dropped++;
            note("too many offline peers to hold anything for " + dest);
            return;
        }

        auto& box = mailbox[dest];
        while (box.size() >= cfg.maxQueued)
            toss(box);
        while (!box.empty() && heldBytes + text.size() > kHeldBudget)
            toss(box);

        if (heldBytes + text.size() > kHeldBudget) {
            if (box.empty())
                mailbox.erase(dest);
            tally.dropped++;
            return;
        }

        box.push_back(Pending{nowMs() + cfg.queueTtlMs, text});
        heldBytes += text.size();
        tally.queued++;
        note("held a message for offline " + dest);
    }

    void deliver(Conn& c, uint64_t now) {
        auto it = mailbox.find(c.id);
        if (it == mailbox.end())
            return;

        for (auto& held : it->second) {
            heldBytes -= held.text.size();
            if (held.expire <= now) {
                tally.dropped++;
                continue;
            }
            if (c.dying || c.closing)
                continue;
            post(c, held.text);
            tally.flushed++;
        }
        mailbox.erase(it);
    }

    void post(Conn& c, const std::string& text) {
        if (c.out.size() + text.size() + 16 > kOutLimit) {
            note("client " + c.id + " is not draining, cutting it off");
            quit(c, 1008);
            return;
        }
        writeText(text, c.out);
        drain(c);
    }

    void drain(Conn& c) {
        while (!c.out.empty()) {
            int n = pushBytes(c.fd, c.out.data(), c.out.size());
            if (n < 0) {
                c.dying = true;
                return;
            }
            if (n == 0)
                return;
            c.out.erase(c.out.begin(), c.out.begin() + n);
        }
        if (c.closing)
            c.dying = true;
    }

    void goodbye(Conn& c, uint16_t code, uint64_t now) {
        if (c.closing || c.dying)
            return;
        writeClose(code, c.out);
        c.closing = true;
        c.closedAt = now;
        drain(c);
    }

    void quit(Conn& c, uint16_t code) {
        goodbye(c, code, nowMs());
    }

    void refuse(Conn& c, int code, const std::string& reason, uint64_t now) {
        if (c.closing || c.dying)
            return;
        std::string body = buildRefusal(code, reason);
        c.out.insert(c.out.end(), body.begin(), body.end());
        c.closing = true;
        c.closedAt = now;
        c.in.clear();
        tally.rejected++;
        drain(c);
    }

    void sweep(uint64_t now) {
        for (auto& holder : conns) {
            Conn& c = *holder;
            if (c.dying)
                continue;
            if (c.closing) {
                if (now - c.closedAt > kLingerMs)
                    c.dying = true;
                continue;
            }
            if (!c.shook) {
                if (now - c.lastIn > kShakeTimeoutMs)
                    c.dying = true;
                continue;
            }

            uint64_t idle = now - c.lastIn;
            if (idle > cfg.idleTimeoutMs) {
                note("client " + c.id + " went quiet, dropping it");
                c.dying = true;
                continue;
            }
            if (!c.pinged && idle > cfg.idleTimeoutMs / 2) {
                uint8_t nothing[1] = {0};
                writeFrame(kOpPing, nothing, 0, c.out);
                c.pinged = true;
                drain(c);
            }
        }

        for (auto it = mailbox.begin(); it != mailbox.end();) {
            auto& box = it->second;
            while (!box.empty() && box.front().expire <= now)
                toss(box);
            if (box.empty())
                it = mailbox.erase(it);
            else
                ++it;
        }
    }

    void reap() {
        for (auto it = conns.begin(); it != conns.end();) {
            Conn* c = it->get();
            if (!c->dying) {
                ++it;
                continue;
            }

            auto slot = byId.find(c->id);
            if (slot != byId.end() && slot->second == c) {
                byId.erase(slot);
                note("client " + c->id + " offline");
            }
            closeHandle(c->fd);
            it = conns.erase(it);
        }
    }

    void publish() {
        std::vector<std::string> ids;
        ids.reserve(byId.size());
        for (auto& kv : byId)
            ids.push_back(kv.first);
        std::sort(ids.begin(), ids.end());

        std::lock_guard<std::mutex> lk(mtx);
        shared = tally;
        shared.clients = ids.size();
        roster = std::move(ids);
    }

    void shutdown() {
        for (auto& holder : conns) {
            Conn& c = *holder;
            if (!c.closing && c.shook) {
                writeClose(1001, c.out);
                drain(c);
            }
            closeHandle(c.fd);
        }
        conns.clear();
        byId.clear();
        mailbox.clear();
        heldBytes = 0;
        door.shut();
    }
};

SignalServer::SignalServer() : impl_(std::make_unique<Impl>()) {}

SignalServer::~SignalServer() {
    stop();
}

std::shared_ptr<SignalServer> SignalServer::start(const SignalConfig& cfg) {
    std::shared_ptr<SignalServer> self(new SignalServer());
    self->impl_->cfg = sanitize(cfg);

    if (!self->impl_->door.open(self->impl_->cfg.bindAddress, self->impl_->cfg.port))
        return nullptr;

    Impl* impl = self->impl_.get();
    impl->running.store(true);
    impl->worker = std::thread([impl] { impl->run(); });
    return self;
}

uint16_t SignalServer::port() const {
    return impl_->door.port();
}

SignalStats SignalServer::stats() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->shared;
}

std::vector<std::string> SignalServer::clients() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->roster;
}

void SignalServer::stop() {
    if (!impl_->running.exchange(false))
        return;
    if (impl_->worker.joinable())
        impl_->worker.join();
    impl_->shutdown();
}

}
