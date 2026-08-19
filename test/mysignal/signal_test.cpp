#include "mysignal/server.hpp"

#include "base64.hpp"
#include "frame.hpp"
#include "handshake.hpp"
#include "jsonpick.hpp"
#include "net.hpp"
#include "sha1.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace mysignal;

namespace {

int g_total = 0;
int g_failed = 0;
const char* g_case = "";

void report(bool ok, const char* what, int line) {
    g_total++;
    if (ok)
        return;
    g_failed++;
    std::printf("  FAIL [%s] line %d: %s\n", g_case, line, what);
}

#define CHECK(x) report((x), #x, __LINE__)

std::string toHex(const uint8_t* p, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(digits[p[i] >> 4]);
        s.push_back(digits[p[i] & 0x0f]);
    }
    return s;
}

std::string hashOf(const std::string& text) {
    uint8_t digest[kSha1Size];
    sha1(reinterpret_cast<const uint8_t*>(text.data()), text.size(), digest);
    return toHex(digest, kSha1Size);
}

std::string b64(const std::string& text) {
    return base64(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
    for (int spent = 0; spent < timeoutMs; spent += 10) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

Handle dialTcp(uint16_t port) {
    Peer target;
    if (!resolveHost("127.0.0.1", port, true, target))
        return kNoHandle;

    Handle fd = socket(target.sa.ss_family, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    if (fd == INVALID_SOCKET)
        return kNoHandle;
#else
    if (fd < 0)
        return kNoHandle;
#endif

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&target.sa),
                  static_cast<socklen_t>(target.len)) != 0) {
        closeHandle(fd);
        return kNoHandle;
    }
    return fd;
}

class Wire {
public:
    ~Wire() { shut(); }

    bool dial(uint16_t port) {
        fd_ = dialTcp(port);
        return fd_ != kNoHandle;
    }

    void shut() {
        closeHandle(fd_);
    }

    bool put(const std::string& raw) {
        return put(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    }

    bool put(const std::vector<uint8_t>& raw) {
        return put(raw.data(), raw.size());
    }

    bool put(const uint8_t* p, size_t n) {
        size_t sent = 0;
        while (sent < n) {
            int step = pushBytes(fd_, p + sent, n - sent);
            if (step < 0)
                return false;
            if (step == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            sent += size_t(step);
        }
        return true;
    }

    int pump(int timeoutMs) {
        PollSlot slot;
        std::memset(&slot, 0, sizeof(slot));
        slot.fd = fd_;
        slot.events = POLLIN;

        if (waitOn(&slot, 1, timeoutMs) <= 0)
            return 0;

        uint8_t buf[4096];
        int n = pullBytes(fd_, buf, sizeof(buf));
        if (n <= 0)
            return n;
        held_.append(reinterpret_cast<const char*>(buf), size_t(n));
        return n;
    }

    std::string& held() { return held_; }

private:
    Handle fd_ = kNoHandle;
    std::string held_;
};

void maskInto(uint8_t opcode, bool fin, const std::string& payload, std::vector<uint8_t>& out) {
    out.push_back(uint8_t((fin ? 0x80 : 0x00) | opcode));

    size_t n = payload.size();
    uint8_t lenByte = 0x80;
    if (n < 126) {
        out.push_back(uint8_t(lenByte | n));
    } else if (n <= 0xffff) {
        out.push_back(uint8_t(lenByte | 126));
        out.push_back(uint8_t(n >> 8));
        out.push_back(uint8_t(n));
    } else {
        out.push_back(uint8_t(lenByte | 127));
        for (int i = 7; i >= 0; --i)
            out.push_back(uint8_t((uint64_t(n) >> (i * 8)) & 0xff));
    }

    const uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    out.insert(out.end(), mask, mask + 4);
    for (size_t i = 0; i < n; ++i)
        out.push_back(uint8_t(uint8_t(payload[i]) ^ mask[i & 3]));
}

class Client {
public:
    bool shake(uint16_t port, const std::string& target) {
        if (!wire_.dial(port))
            return false;

        std::string request;
        request += "GET " + target + " HTTP/1.1\r\n";
        request += "Host: 127.0.0.1\r\n";
        request += "Upgrade: websocket\r\n";
        request += "Connection: Upgrade\r\n";
        request += "Sec-WebSocket-Key: " + key_ + "\r\n";
        request += "Sec-WebSocket-Version: 13\r\n";
        request += "\r\n";
        if (!wire_.put(request))
            return false;

        for (int spent = 0; spent < 2000; spent += 20) {
            size_t end = wire_.held().find("\r\n\r\n");
            if (end != std::string::npos) {
                std::string head = wire_.held().substr(0, end);
                status_ = 0;
                if (head.size() > 12)
                    status_ = std::atoi(head.c_str() + 9);
                bool accepted = status_ == 101 &&
                                head.find("Sec-WebSocket-Accept: " + acceptKey(key_)) !=
                                    std::string::npos;
                wire_.held().erase(0, end + 4);
                return accepted;
            }
            if (wire_.pump(20) < 0)
                return false;
        }
        return false;
    }

    bool sendText(const std::string& text) {
        std::vector<uint8_t> raw;
        maskInto(kOpText, true, text, raw);
        return wire_.put(raw);
    }

    bool sendPiece(uint8_t opcode, bool fin, const std::string& text) {
        std::vector<uint8_t> raw;
        maskInto(opcode, fin, text, raw);
        return wire_.put(raw);
    }

    bool sendRaw(const std::vector<uint8_t>& raw) { return wire_.put(raw); }

    bool waitFrame(Frame& out, int timeoutMs) {
        for (int spent = 0; spent <= timeoutMs; spent += 20) {
            size_t used = 0;
            FrameState st = readFrame(reinterpret_cast<const uint8_t*>(wire_.held().data()),
                                      wire_.held().size(), 1 << 20, false, out, used);
            if (st == FrameState::Ok) {
                wire_.held().erase(0, used);
                return true;
            }
            if (st == FrameState::Bad)
                return false;
            if (wire_.pump(20) < 0)
                return false;
        }
        return false;
    }

    bool waitText(std::string& out, int timeoutMs) {
        int spent = 0;
        while (spent <= timeoutMs) {
            Frame f;
            if (!waitFrame(f, timeoutMs - spent))
                return false;
            if (f.opcode == kOpText) {
                out.assign(f.payload.begin(), f.payload.end());
                return true;
            }
            spent += 20;
        }
        return false;
    }

    int status() const { return status_; }
    void shut() { wire_.shut(); }

private:
    Wire wire_;
    std::string key_ = "dGhlIHNhbXBsZSBub25jZQ==";
    int status_ = 0;
};

void putU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v & 0xff));
}

std::vector<uint8_t> bindingRequest(const uint8_t txid[12], uint16_t type) {
    std::vector<uint8_t> out;
    putU16(out, type);
    putU16(out, 0);
    putU16(out, 0x2112);
    putU16(out, 0xa442);
    out.insert(out.end(), txid, txid + 12);
    return out;
}

bool readMapped(const std::vector<uint8_t>& msg, const uint8_t txid[12], uint16_t wanted,
                int& family, uint8_t addr[16], uint16_t& port) {
    if (msg.size() < 20)
        return false;
    if (msg[0] != 0x01 || msg[1] != 0x01)
        return false;
    if (std::memcmp(msg.data() + 8, txid, 12) != 0)
        return false;

    uint16_t length = uint16_t((uint16_t(msg[2]) << 8) | msg[3]);
    if (size_t(length) + 20 != msg.size())
        return false;

    uint8_t mask[16] = {0x21, 0x12, 0xa4, 0x42};
    std::memcpy(mask + 4, txid, 12);

    size_t cursor = 20;
    while (cursor + 4 <= msg.size()) {
        uint16_t attr = uint16_t((uint16_t(msg[cursor]) << 8) | msg[cursor + 1]);
        uint16_t alen = uint16_t((uint16_t(msg[cursor + 2]) << 8) | msg[cursor + 3]);
        size_t body = cursor + 4;
        size_t padded = (size_t(alen) + 3u) & ~size_t(3u);
        if (body + padded > msg.size())
            return false;

        if (attr == wanted && alen >= 8) {
            bool xored = wanted == 0x0020;
            family = msg[body + 1] == 0x02 ? AF_INET6 : AF_INET;
            uint16_t raw = uint16_t((uint16_t(msg[body + 2]) << 8) | msg[body + 3]);
            port = xored ? uint16_t(raw ^ 0x2112) : raw;

            size_t width = family == AF_INET6 ? 16u : 4u;
            if (alen != 4 + width)
                return false;
            for (size_t i = 0; i < width; ++i)
                addr[i] = uint8_t(msg[body + 4 + i] ^ (xored ? mask[i] : 0));
            return true;
        }
        cursor = body + padded;
    }
    return false;
}

void checkSha1() {
    g_case = "sha1";
    CHECK(hashOf("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hashOf("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hashOf("The quick brown fox jumps over the lazy dog") ==
          "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
    CHECK(hashOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(hashOf("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopq"
                 "klmnopqrlmnopqrsmnopqrstnopqrstu") ==
          "a49b2446a02c645bf419f995b67091253a04a259");
    CHECK(hashOf(std::string(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

void checkBase64() {
    g_case = "base64";
    CHECK(b64("") == "");
    CHECK(b64("f") == "Zg==");
    CHECK(b64("fo") == "Zm8=");
    CHECK(b64("foo") == "Zm9v");
    CHECK(b64("foob") == "Zm9vYg==");
    CHECK(b64("fooba") == "Zm9vYmE=");
    CHECK(b64("foobar") == "Zm9vYmFy");
    CHECK(acceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

std::string upgradeRequest(const std::string& target, const std::string& extra = "",
                           const std::string& version = "13",
                           const std::string& key = "dGhlIHNhbXBsZSBub25jZQ==") {
    std::string out = "GET " + target + " HTTP/1.1\r\nHost: h\r\n";
    out += "Upgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\n";
    if (!key.empty())
        out += "Sec-WebSocket-Key: " + key + "\r\n";
    if (!version.empty())
        out += "Sec-WebSocket-Version: " + version + "\r\n";
    out += extra;
    out += "\r\n";
    return out;
}

void checkUpgrade() {
    g_case = "upgrade";

    Upgrade up;
    size_t used = 0;
    std::string good = upgradeRequest("/alice");
    CHECK(readUpgrade(good, used, up) == UpgradeState::Ok);
    CHECK(up.id == "alice");
    CHECK(up.key == "dGhlIHNhbXBsZSBub25jZQ==");
    CHECK(used == good.size());

    CHECK(readUpgrade(good.substr(0, good.size() - 2), used, up) == UpgradeState::NeedMore);
    CHECK(readUpgrade(upgradeRequest("/a", "", "8"), used, up) == UpgradeState::Bad);
    CHECK(readUpgrade(upgradeRequest("/a", "", "13", ""), used, up) == UpgradeState::Bad);
    CHECK(readUpgrade(upgradeRequest("/a", "", "13", "short"), used, up) == UpgradeState::Bad);
    CHECK(readUpgrade("POST /a HTTP/1.1\r\n\r\n", used, up) == UpgradeState::Bad);
    CHECK(readUpgrade("GET a HTTP/1.1\r\n\r\n", used, up) == UpgradeState::Bad);

    CHECK(readUpgrade(upgradeRequest("/bob?token=hunter2"), used, up) == UpgradeState::Ok);
    CHECK(up.id == "bob");
    CHECK(up.token == "hunter2");

    CHECK(readUpgrade(upgradeRequest("/ro%6Fm/extra?x=1&token=a%20b"), used, up) ==
          UpgradeState::Ok);
    CHECK(up.id == "room");
    CHECK(up.token == "a b");

    std::string trailing = good + "leftover";
    CHECK(readUpgrade(trailing, used, up) == UpgradeState::Ok);
    CHECK(trailing.size() - used == 8);

    CHECK(validId("alice"));
    CHECK(validId("a.b-c_d~1"));
    CHECK(!validId(""));
    CHECK(!validId("bad/id"));
    CHECK(!validId("bad id"));
    CHECK(!validId(std::string(65, 'x')));
    CHECK(validId(std::string(64, 'x')));

    CHECK(sameSecret("abc", "abc"));
    CHECK(!sameSecret("abc", "abd"));
    CHECK(!sameSecret("abc", "ab"));
    CHECK(percentDecode("a%2Bb%zz%") == "a+b%zz%");
}

void checkFrames() {
    g_case = "frames";

    std::vector<uint8_t> raw;
    writeText("hello", raw);
    CHECK(raw.size() == 7);
    CHECK(raw[0] == 0x81);
    CHECK(raw[1] == 5);

    Frame f;
    size_t used = 0;
    CHECK(readFrame(raw.data(), raw.size(), 4096, false, f, used) == FrameState::Ok);
    CHECK(used == raw.size());
    CHECK(f.fin && f.opcode == kOpText);
    CHECK(std::string(f.payload.begin(), f.payload.end()) == "hello");

    CHECK(readFrame(raw.data(), 1, 4096, false, f, used) == FrameState::NeedMore);
    CHECK(readFrame(raw.data(), raw.size() - 1, 4096, false, f, used) == FrameState::NeedMore);
    CHECK(readFrame(raw.data(), raw.size(), 4096, true, f, used) == FrameState::Bad);

    std::vector<uint8_t> masked;
    maskInto(kOpText, true, "ping me", masked);
    CHECK(readFrame(masked.data(), masked.size(), 4096, true, f, used) == FrameState::Ok);
    CHECK(std::string(f.payload.begin(), f.payload.end()) == "ping me");

    std::vector<uint8_t> medium;
    maskInto(kOpBinary, true, std::string(300, 'z'), medium);
    CHECK(medium[1] == (0x80 | 126));
    CHECK(readFrame(medium.data(), medium.size(), 4096, true, f, used) == FrameState::Ok);
    CHECK(f.payload.size() == 300);

    std::vector<uint8_t> huge;
    maskInto(kOpBinary, true, std::string(70000, 'q'), huge);
    CHECK(huge[1] == (0x80 | 127));
    CHECK(readFrame(huge.data(), huge.size(), 1 << 20, true, f, used) == FrameState::Ok);
    CHECK(f.payload.size() == 70000);
    CHECK(readFrame(huge.data(), huge.size(), 4096, true, f, used) == FrameState::Bad);

    std::vector<uint8_t> reserved = raw;
    reserved[0] = uint8_t(reserved[0] | 0x40);
    CHECK(readFrame(reserved.data(), reserved.size(), 4096, false, f, used) == FrameState::Bad);

    std::vector<uint8_t> unknown = raw;
    unknown[0] = 0x83;
    CHECK(readFrame(unknown.data(), unknown.size(), 4096, false, f, used) == FrameState::Bad);

    std::vector<uint8_t> splitPing;
    maskInto(kOpPing, false, "x", splitPing);
    CHECK(readFrame(splitPing.data(), splitPing.size(), 4096, true, f, used) == FrameState::Bad);

    std::vector<uint8_t> fatClose;
    maskInto(kOpClose, true, std::string(200, 'c'), fatClose);
    CHECK(readFrame(fatClose.data(), fatClose.size(), 4096, true, f, used) == FrameState::Bad);

    std::vector<uint8_t> stumpyClose;
    maskInto(kOpClose, true, std::string("\x03", 1), stumpyClose);
    CHECK(readFrame(stumpyClose.data(), stumpyClose.size(), 4096, true, f, used) ==
          FrameState::Bad);

    std::vector<uint8_t> bye;
    writeClose(1001, bye);
    CHECK(readFrame(bye.data(), bye.size(), 4096, false, f, used) == FrameState::Ok);
    CHECK(f.opcode == kOpClose && f.payload.size() == 2);
    CHECK(f.payload[0] == 0x03 && f.payload[1] == 0xe9);
}

void checkJson() {
    g_case = "json";

    std::string value;
    Span span;
    std::string msg = R"({"id":"bob","type":"offer"})";
    CHECK(pickTopString(msg, "id", value, span));
    CHECK(value == "bob");
    CHECK(msg.substr(span.begin, span.end - span.begin) == "\"bob\"");
    CHECK(splice(msg, span, quoteJson("alice")) == R"({"id":"alice","type":"offer"})");

    std::string later = R"({"type":"candidate","mid":"0","id":"carol"})";
    CHECK(pickTopString(later, "id", value, span));
    CHECK(value == "carol");

    std::string nested = R"({"payload":{"id":"inner"},"id":"outer"})";
    CHECK(pickTopString(nested, "id", value, span));
    CHECK(value == "outer");

    std::string listy = R"({"list":[1,2,{"id":"no"}],"id":"yes"})";
    CHECK(pickTopString(listy, "id", value, span));
    CHECK(value == "yes");

    std::string quoted = R"({"description":"a\"b{\"id\":\"fake\"}","id":"real"})";
    CHECK(pickTopString(quoted, "id", value, span));
    CHECK(value == "real");

    std::string spaced = "{ \"id\" : \"pad\" , \"n\" : 12 }";
    CHECK(pickTopString(spaced, "id", value, span));
    CHECK(value == "pad");

    CHECK(!pickTopString(R"({"type":"offer"})", "id", value, span));
    CHECK(!pickTopString(R"({"id":42})", "id", value, span));
    CHECK(!pickTopString(R"({"id":null})", "id", value, span));
    CHECK(!pickTopString("not json", "id", value, span));
    CHECK(!pickTopString("", "id", value, span));
    CHECK(!pickTopString("{", "id", value, span));
    CHECK(!pickTopString(R"({"id":"unterminated)", "id", value, span));

    CHECK(quoteJson("a\"b\\c") == "\"a\\\"b\\\\c\"");
    CHECK(quoteJson(std::string("x\x01" "y")) == "\"x\\u0001y\"");
}

std::shared_ptr<SignalServer> spin(SignalConfig cfg) {
    cfg.port = 0;
    cfg.bindAddress = "127.0.0.1";
    return SignalServer::start(cfg);
}

void checkRelay() {
    g_case = "relay";

    SignalConfig cfg;
    auto server = spin(cfg);
    CHECK(server != nullptr);
    if (!server)
        return;

    Client alice;
    Client bob;
    CHECK(alice.shake(server->port(), "/alice"));
    CHECK(bob.shake(server->port(), "/bob"));
    CHECK(waitFor([&] { return server->clients().size() == 2; }, 1000));

    auto roster = server->clients();
    CHECK(roster.size() == 2 && roster[0] == "alice" && roster[1] == "bob");

    CHECK(alice.sendText(R"({"id":"bob","type":"offer","description":"v=0"})"));
    std::string got;
    CHECK(bob.waitText(got, 1000));
    CHECK(got == R"({"id":"alice","type":"offer","description":"v=0"})");

    CHECK(bob.sendText(R"({"id":"alice","type":"answer","description":"v=0 back"})"));
    CHECK(alice.waitText(got, 1000));
    CHECK(got == R"({"id":"bob","type":"answer","description":"v=0 back"})");

    CHECK(alice.sendPiece(kOpText, false, R"({"id":"bo)"));
    CHECK(alice.sendPiece(kOpContinuation, true, R"(b","type":"rtm"})"));
    CHECK(bob.waitText(got, 1000));
    CHECK(got == R"({"id":"alice","type":"rtm"})");

    CHECK(alice.sendText(R"({"id":"alice","type":"echo"})"));
    CHECK(alice.waitText(got, 1000));
    CHECK(got == R"({"id":"alice","type":"echo"})");

    std::vector<uint8_t> ping;
    maskInto(kOpPing, true, "knock", ping);
    CHECK(alice.sendRaw(ping));
    Frame pong;
    CHECK(alice.waitFrame(pong, 1000));
    CHECK(pong.opcode == kOpPong);
    CHECK(std::string(pong.payload.begin(), pong.payload.end()) == "knock");

    CHECK(alice.sendText(R"({"type":"noid"})"));
    CHECK(alice.sendText(R"({"id":"bad id","type":"x"})"));
    CHECK(alice.sendText("garbage"));
    CHECK(alice.sendText(R"({"id":"bob","type":"still here"})"));
    CHECK(bob.waitText(got, 1000));
    CHECK(got == R"({"id":"alice","type":"still here"})");

    CHECK(waitFor([&] { return server->stats().relayed >= 5; }, 1000));
    auto st = server->stats();
    CHECK(st.accepted == 2);
    CHECK(st.clients == 2);

    bob.shut();
    CHECK(waitFor([&] { return server->clients().size() == 1; }, 2000));
    server->stop();
}

void checkFarewell() {
    g_case = "farewell";

    SignalConfig cfg;
    auto server = spin(cfg);
    CHECK(server != nullptr);
    if (!server)
        return;

    Client bob;
    CHECK(bob.shake(server->port(), "/bob"));

    Wire oneShot;
    CHECK(oneShot.dial(server->port()));

    std::string head;
    head += "GET /dave HTTP/1.1\r\n";
    head += "Host: 127.0.0.1\r\n";
    head += "Upgrade: websocket\r\n";
    head += "Connection: Upgrade\r\n";
    head += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
    head += "Sec-WebSocket-Version: 13\r\n";
    head += "\r\n";

    std::vector<uint8_t> burst(head.begin(), head.end());
    maskInto(kOpText, true, R"({"id":"bob","type":"last words"})", burst);
    CHECK(oneShot.put(burst));
    oneShot.shut();

    std::string got;
    CHECK(bob.waitText(got, 2000));
    CHECK(got == R"({"id":"dave","type":"last words"})");

    Client alice;
    CHECK(alice.shake(server->port(), "/alice"));
    std::vector<uint8_t> bye;
    maskInto(kOpClose, true, std::string("\x0b\xb9", 2), bye);
    CHECK(alice.sendRaw(bye));

    Frame echoed;
    CHECK(alice.waitFrame(echoed, 1500));
    CHECK(echoed.opcode == kOpClose);
    CHECK(echoed.payload.size() == 2);
    CHECK(uint16_t((uint16_t(echoed.payload[0]) << 8) | echoed.payload[1]) == 3001);

    Client stub;
    CHECK(stub.shake(server->port(), "/stub"));
    std::vector<uint8_t> stumpy;
    maskInto(kOpClose, true, std::string("\x03", 1), stumpy);
    CHECK(stub.sendRaw(stumpy));

    Frame protest;
    CHECK(stub.waitFrame(protest, 1500));
    CHECK(protest.opcode == kOpClose);
    CHECK(protest.payload.size() == 2);
    CHECK(uint16_t((uint16_t(protest.payload[0]) << 8) | protest.payload[1]) == 1002);

    server->stop();
}

void checkMailbox() {
    g_case = "mailbox";

    SignalConfig cfg;
    cfg.maxQueued = 3;
    auto server = spin(cfg);
    CHECK(server != nullptr);
    if (!server)
        return;

    Client alice;
    CHECK(alice.shake(server->port(), "/alice"));

    CHECK(alice.sendText(R"({"id":"bob","seq":1})"));
    CHECK(alice.sendText(R"({"id":"bob","seq":2})"));
    CHECK(waitFor([&] { return server->stats().queued == 2; }, 1000));

    Client bob;
    CHECK(bob.shake(server->port(), "/bob"));

    std::string first;
    std::string second;
    CHECK(bob.waitText(first, 1000));
    CHECK(bob.waitText(second, 1000));
    CHECK(first == R"({"id":"alice","seq":1})");
    CHECK(second == R"({"id":"alice","seq":2})");
    CHECK(waitFor([&] { return server->stats().flushed == 2; }, 1000));

    for (int i = 0; i < 5; ++i)
        CHECK(alice.sendText(R"({"id":"carol","seq":)" + std::to_string(i) + "}"));
    CHECK(waitFor([&] { return server->stats().dropped >= 2; }, 1000));

    Client carol;
    CHECK(carol.shake(server->port(), "/carol"));
    std::string kept;
    CHECK(carol.waitText(kept, 1000));
    CHECK(kept == R"({"id":"alice","seq":2})");

    server->stop();
}

void checkMailboxCap() {
    g_case = "mailbox-cap";

    SignalConfig cfg;
    cfg.maxQueued = 1;
    auto server = spin(cfg);
    CHECK(server != nullptr);
    if (!server)
        return;

    Client alice;
    CHECK(alice.shake(server->port(), "/alice"));

    bool allSent = true;
    for (int i = 0; i < 300; ++i)
        allSent = alice.sendText(R"({"id":"ghost-)" + std::to_string(i) + R"(","seq":1})") && allSent;
    CHECK(allSent);

    CHECK(waitFor([&] { return server->stats().queued == 256; }, 3000));
    CHECK(waitFor([&] { return server->stats().dropped >= 44; }, 1000));

    Client early;
    CHECK(early.shake(server->port(), "/ghost-0"));
    std::string got;
    CHECK(early.waitText(got, 1000));
    CHECK(got == R"({"id":"alice","seq":1})");

    Client late;
    CHECK(late.shake(server->port(), "/ghost-299"));
    std::string nothing;
    CHECK(!late.waitText(nothing, 400));

    server->stop();
}

void checkExpiry() {
    g_case = "expiry";

    SignalConfig cfg;
    cfg.queueTtlMs = 1000;
    auto server = spin(cfg);
    CHECK(server != nullptr);
    if (!server)
        return;

    Client alice;
    CHECK(alice.shake(server->port(), "/alice"));
    CHECK(alice.sendText(R"({"id":"ghost","seq":1})"));
    CHECK(waitFor([&] { return server->stats().queued == 1; }, 1000));
    CHECK(waitFor([&] { return server->stats().dropped == 1; }, 3000));

    Client ghost;
    CHECK(ghost.shake(server->port(), "/ghost"));
    std::string nothing;
    CHECK(!ghost.waitText(nothing, 400));
    CHECK(server->stats().flushed == 0);

    server->stop();
}

void checkDoor() {
    g_case = "door";

    SignalConfig cfg;
    cfg.token = "let-me-in";
    auto server = spin(cfg);
    CHECK(server != nullptr);
    if (!server)
        return;

    Client wrong;
    CHECK(!wrong.shake(server->port(), "/alice?token=nope"));
    CHECK(wrong.status() == 401);

    Client missing;
    CHECK(!missing.shake(server->port(), "/alice"));
    CHECK(missing.status() == 401);

    Client right;
    CHECK(right.shake(server->port(), "/alice?token=let-me-in"));
    CHECK(right.status() == 101);

    Client sloppy;
    CHECK(sloppy.shake(server->port(), "/bad%20id?token=let-me-in") == false);
    CHECK(sloppy.status() == 400);

    Wire plain;
    CHECK(plain.dial(server->port()));
    CHECK(plain.put(std::string("GET /x HTTP/1.1\r\nHost: h\r\n\r\n")));

    bool refused = false;
    for (int spent = 0; spent < 1500 && !refused; spent += 20) {
        plain.pump(20);
        refused = plain.held().find(" 400 ") != std::string::npos;
    }
    CHECK(refused);

    CHECK(waitFor([&] { return server->stats().rejected >= 4; }, 1000));
    CHECK(server->stats().accepted == 1);

    server->stop();
}

void checkHalfOpen() {
    g_case = "half-open";

    SignalConfig cfg;
    auto server = spin(cfg);
    CHECK(server != nullptr);
    if (!server)
        return;

    std::vector<std::unique_ptr<Wire>> quiet;
    for (int i = 0; i < 16; ++i) {
        std::unique_ptr<Wire> w(new Wire());
        CHECK(w->dial(server->port()));
        quiet.push_back(std::move(w));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(server->stats().rejected == 0);

    Client crowded;
    CHECK(!crowded.shake(server->port(), "/crowded"));
    CHECK(waitFor([&] { return server->stats().rejected >= 1; }, 1000));

    for (auto& w : quiet)
        w->shut();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    Client roomy;
    CHECK(roomy.shake(server->port(), "/roomy"));
    CHECK(waitFor([&] { return server->clients().size() == 1; }, 1000));

    server->stop();
}

void checkTakeover() {
    g_case = "takeover";

    SignalConfig cfg;
    auto server = spin(cfg);
    CHECK(server != nullptr);
    if (!server)
        return;

    Client first;
    CHECK(first.shake(server->port(), "/alice"));
    CHECK(waitFor([&] { return server->clients().size() == 1; }, 1000));

    Client second;
    CHECK(second.shake(server->port(), "/alice"));

    Frame closing;
    CHECK(first.waitFrame(closing, 1500));
    CHECK(closing.opcode == kOpClose);

    Client bob;
    CHECK(bob.shake(server->port(), "/bob"));
    CHECK(bob.sendText(R"({"id":"alice","type":"reaches-the-new-one"})"));

    std::string got;
    CHECK(second.waitText(got, 1000));
    CHECK(got == R"({"id":"bob","type":"reaches-the-new-one"})");
    CHECK(waitFor([&] { return server->clients().size() == 2; }, 2000));

    server->stop();
}

void checkLimits() {
    g_case = "limits";

    SignalConfig cfg;
    cfg.maxMessageSize = 4096;
    auto server = spin(cfg);
    CHECK(server != nullptr);
    if (!server)
        return;

    Client alice;
    Client bob;
    CHECK(alice.shake(server->port(), "/alice"));
    CHECK(bob.shake(server->port(), "/bob"));

    std::string pad(3000, 'p');
    CHECK(alice.sendText(R"({"id":"bob","pad":")" + pad + "\"}"));
    std::string got;
    CHECK(bob.waitText(got, 2000));
    CHECK(got.size() > 3000);
    CHECK(got.find(R"("id":"alice")") == 1);

    std::vector<uint8_t> oversized;
    oversized.push_back(0x81);
    oversized.push_back(uint8_t(0x80 | 126));
    oversized.push_back(0x20);
    oversized.push_back(0x00);
    for (int i = 0; i < 4; ++i)
        oversized.push_back(0x11);
    for (int i = 0; i < 64; ++i)
        oversized.push_back(0x22);
    CHECK(bob.sendRaw(oversized));

    Frame closing;
    CHECK(bob.waitFrame(closing, 1500));
    CHECK(closing.opcode == kOpClose);
    CHECK(waitFor([&] { return server->clients().size() == 1; }, 2000));

    server->stop();
}

void checkTorrent() {
    g_case = "torrent";

    SignalConfig cfg;
    auto server = spin(cfg);
    CHECK(server != nullptr);
    if (!server)
        return;

    Client alice;
    Client bob;
    CHECK(alice.shake(server->port(), "/alice"));
    CHECK(bob.shake(server->port(), "/bob"));
    CHECK(waitFor([&] { return server->clients().size() == 2; }, 1000));

    bool allSent = true;
    for (int i = 0; i < 1600; ++i)
        allSent = alice.sendText(R"({"id":"bob","seq":)" + std::to_string(i) + "}") && allSent;
    CHECK(allSent);

    CHECK(waitFor([&] { return server->stats().relayed >= 1024; }, 4000));
    CHECK(waitFor([&] { return server->stats().dropped > 0; }, 4000));
    CHECK(server->stats().relayed + server->stats().dropped <= 1600);

    std::string got;
    CHECK(bob.waitText(got, 1000));
    CHECK(got.find(R"({"id":"alice","seq":)") == 0);
    CHECK(server->clients().size() == 2);

    server->stop();
}

void checkHeartbeat() {
    g_case = "heartbeat";

    SignalConfig cfg;
    cfg.idleTimeoutMs = 4000;
    auto server = spin(cfg);
    CHECK(server != nullptr);
    if (!server)
        return;

    Client alice;
    CHECK(alice.shake(server->port(), "/alice"));

    Frame beat;
    CHECK(alice.waitFrame(beat, 3000));
    CHECK(beat.opcode == kOpPing);
    CHECK(beat.payload.empty());

    CHECK(waitFor([&] { return server->clients().empty(); }, 6000));
    server->stop();
}

std::shared_ptr<StunServer> spinStun() {
    StunConfig cfg;
    cfg.bindAddress = "127.0.0.1";
    cfg.port = 0;
    return StunServer::start(cfg);
}

void checkStun() {
    g_case = "stun";

    auto server = spinStun();
    CHECK(server != nullptr);
    if (!server)
        return;

    Peer target;
    CHECK(resolveHost("127.0.0.1", server->port(), false, target));

    Datagram client;
    CHECK(client.open("127.0.0.1", 0));
    uint16_t mine = client.port();
    CHECK(mine != 0);

    uint8_t txid[12];
    for (int i = 0; i < 12; ++i)
        txid[i] = uint8_t(0xa0 + i);

    auto request = bindingRequest(txid, 0x0001);
    CHECK(request.size() == 20);
    CHECK(client.send(request.data(), request.size(), target));

    std::vector<uint8_t> buf(1024);
    Peer from;
    int n = 0;
    for (int spent = 0; spent < 1000 && n <= 0; spent += 20)
        n = client.recv(buf.data(), buf.size(), from, 20);
    CHECK(n > 0);
    if (n <= 0) {
        server->stop();
        return;
    }

    std::vector<uint8_t> reply(buf.begin(), buf.begin() + n);
    int family = 0;
    uint8_t addr[16] = {0};
    uint16_t port = 0;
    CHECK(readMapped(reply, txid, 0x0020, family, addr, port));
    CHECK(family == AF_INET);
    CHECK(port == mine);
    CHECK(addr[0] == 127 && addr[3] == 1);

    int plainFamily = 0;
    uint8_t plainAddr[16] = {0};
    uint16_t plainPort = 0;
    CHECK(readMapped(reply, txid, 0x0001, plainFamily, plainAddr, plainPort));
    CHECK(plainPort == mine);
    CHECK(std::memcmp(addr, plainAddr, 4) == 0);

    uint8_t other[12];
    for (int i = 0; i < 12; ++i)
        other[i] = uint8_t(i * 7 + 1);
    CHECK(!readMapped(reply, other, 0x0020, family, addr, port));

    auto withAttr = bindingRequest(txid, 0x0001);
    withAttr[2] = 0x00;
    withAttr[3] = 0x08;
    putU16(withAttr, 0x8022);
    putU16(withAttr, 4);
    withAttr.push_back('t');
    withAttr.push_back('e');
    withAttr.push_back('s');
    withAttr.push_back('t');
    CHECK(client.send(withAttr.data(), withAttr.size(), target));
    n = 0;
    for (int spent = 0; spent < 1000 && n <= 0; spent += 20)
        n = client.recv(buf.data(), buf.size(), from, 20);
    CHECK(n > 0);

    uint64_t before = server->stats().ignored;

    auto indication = bindingRequest(txid, 0x0011);
    CHECK(client.send(indication.data(), indication.size(), target));

    auto badCookie = bindingRequest(txid, 0x0001);
    badCookie[4] = 0x00;
    CHECK(client.send(badCookie.data(), badCookie.size(), target));

    auto badLength = bindingRequest(txid, 0x0001);
    badLength[3] = 0x08;
    CHECK(client.send(badLength.data(), badLength.size(), target));

    auto ragged = bindingRequest(txid, 0x0001);
    ragged[3] = 0x04;
    ragged.push_back(0x00);
    ragged.push_back(0x06);
    ragged.push_back(0xff);
    ragged.push_back(0xff);
    CHECK(client.send(ragged.data(), ragged.size(), target));

    std::string junk = "hello stun";
    CHECK(client.send(reinterpret_cast<const uint8_t*>(junk.data()), junk.size(), target));

    CHECK(waitFor([&] { return server->stats().ignored >= before + 5; }, 2000));
    CHECK(client.recv(buf.data(), buf.size(), from, 300) == 0);

    auto st = server->stats();
    CHECK(st.requests == 2);
    CHECK(st.responses == 2);

    server->stop();
}

void checkStunV6() {
    g_case = "stun-v6";

    StunConfig cfg;
    cfg.bindAddress = "::1";
    cfg.port = 0;
    auto server = StunServer::start(cfg);
    if (!server) {
        std::printf("  skipped [stun-v6]: no ipv6 loopback here\n");
        return;
    }

    Peer target;
    CHECK(resolveHost("::1", server->port(), false, target));

    Datagram client;
    CHECK(client.open("::1", 0));
    uint16_t mine = client.port();

    uint8_t txid[12];
    for (int i = 0; i < 12; ++i)
        txid[i] = uint8_t(i * 3 + 5);

    auto request = bindingRequest(txid, 0x0001);
    CHECK(client.send(request.data(), request.size(), target));

    std::vector<uint8_t> buf(1024);
    Peer from;
    int n = 0;
    for (int spent = 0; spent < 1000 && n <= 0; spent += 20)
        n = client.recv(buf.data(), buf.size(), from, 20);
    CHECK(n > 0);
    if (n <= 0) {
        server->stop();
        return;
    }

    std::vector<uint8_t> reply(buf.begin(), buf.begin() + n);
    int family = 0;
    uint8_t addr[16] = {0};
    uint16_t port = 0;
    CHECK(readMapped(reply, txid, 0x0020, family, addr, port));
    CHECK(family == AF_INET6);
    CHECK(port == mine);

    uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    CHECK(std::memcmp(addr, loopback, 16) == 0);

    int plainFamily = 0;
    uint8_t plainAddr[16] = {0};
    uint16_t plainPort = 0;
    CHECK(readMapped(reply, txid, 0x0001, plainFamily, plainAddr, plainPort));
    CHECK(plainFamily == AF_INET6);
    CHECK(plainPort == mine);
    CHECK(std::memcmp(plainAddr, loopback, 16) == 0);

    server->stop();
}

void checkBindFailure() {
    g_case = "bind";

    SignalConfig first;
    first.bindAddress = "127.0.0.1";
    first.port = 0;
    auto taken = SignalServer::start(first);
    CHECK(taken != nullptr);
    if (!taken)
        return;

    SignalConfig clash;
    clash.bindAddress = "127.0.0.1";
    clash.port = taken->port();
    auto denied = SignalServer::start(clash);
    CHECK(denied == nullptr);

    SignalConfig nowhere;
    nowhere.bindAddress = "192.0.2.1";
    nowhere.port = 0;
    CHECK(SignalServer::start(nowhere) == nullptr);

    taken->stop();
}

}

int main() {
    startupSockets();

    checkSha1();
    checkBase64();
    checkUpgrade();
    checkFrames();
    checkJson();
    checkRelay();
    checkFarewell();
    checkMailbox();
    checkMailboxCap();
    checkExpiry();
    checkDoor();
    checkHalfOpen();
    checkTakeover();
    checkLimits();
    checkTorrent();
    checkHeartbeat();
    checkStun();
    checkStunV6();
    checkBindFailure();

    std::printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
