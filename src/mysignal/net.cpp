#include "net.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

namespace mysignal {
namespace {

#ifdef _WIN32
struct WinsockGuard {
    WinsockGuard() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WinsockGuard() { WSACleanup(); }
};
#endif

bool wouldBlock() {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

bool alive(Handle h) {
#ifdef _WIN32
    return h != INVALID_SOCKET;
#else
    return h >= 0;
#endif
}

}

void startupSockets() {
#ifdef _WIN32
    static WinsockGuard guard;
    (void)guard;
#endif
}

uint64_t nowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

std::string Peer::text() const {
    if (!valid())
        return std::string();

    char host[NI_MAXHOST] = {0};
    char serv[NI_MAXSERV] = {0};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&sa), static_cast<socklen_t>(len), host,
                    sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return std::string();

    std::string out;
    if (sa.ss_family == AF_INET6) {
        out += '[';
        out += host;
        out += ']';
    } else {
        out += host;
    }
    out += ':';
    out += serv;
    return out;
}

bool Peer::split(int& family, uint8_t addr[16], uint16_t& port) const {
    if (!valid())
        return false;

    if (sa.ss_family == AF_INET) {
        const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(&sa);
        family = AF_INET;
        std::memcpy(addr, &sin->sin_addr, 4);
        port = ntohs(sin->sin_port);
        return true;
    }
    if (sa.ss_family == AF_INET6) {
        const sockaddr_in6* sin = reinterpret_cast<const sockaddr_in6*>(&sa);
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(&sin->sin6_addr);
        port = ntohs(sin->sin6_port);

        static const uint8_t v4prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (std::memcmp(raw, v4prefix, sizeof(v4prefix)) == 0) {
            family = AF_INET;
            std::memcpy(addr, raw + 12, 4);
            return true;
        }
        family = AF_INET6;
        std::memcpy(addr, raw, 16);
        return true;
    }
    return false;
}

bool resolveHost(const std::string& host, uint16_t port, bool stream, Peer& out) {
    startupSockets();

    char portText[16];
    std::snprintf(portText, sizeof(portText), "%u", static_cast<unsigned>(port));

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = stream ? SOCK_STREAM : SOCK_DGRAM;
    hints.ai_protocol = stream ? IPPROTO_TCP : IPPROTO_UDP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* res = nullptr;
    if (getaddrinfo(host.empty() ? nullptr : host.c_str(), portText, &hints, &res) != 0)
        return false;

    bool found = false;
    if (res) {
        std::memcpy(&out.sa, res->ai_addr, res->ai_addrlen);
        out.len = static_cast<int>(res->ai_addrlen);
        found = true;
    }
    freeaddrinfo(res);
    return found;
}

void closeHandle(Handle& h) {
    if (!alive(h))
        return;
#ifdef _WIN32
    closesocket(h);
    h = INVALID_SOCKET;
#else
    ::close(h);
    h = -1;
#endif
}

void unblock(Handle h) {
    if (!alive(h))
        return;
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(h, FIONBIO, &on);
#else
    int flags = fcntl(h, F_GETFL, 0);
    if (flags >= 0)
        fcntl(h, F_SETFL, flags | O_NONBLOCK);
#endif
}

int waitOn(PollSlot* slots, size_t count, int timeoutMs) {
    if (count == 0)
        return 0;
#ifdef _WIN32
    return WSAPoll(slots, static_cast<ULONG>(count), timeoutMs);
#else
    return ::poll(slots, static_cast<nfds_t>(count), timeoutMs);
#endif
}

uint16_t boundPort(Handle h) {
    if (!alive(h))
        return 0;

    sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getsockname(h, reinterpret_cast<sockaddr*>(&ss), &len) != 0)
        return 0;
    if (ss.ss_family == AF_INET)
        return ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
    if (ss.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
    return 0;
}

namespace {

std::vector<std::string> candidateHosts(const std::string& addr) {
    if (!addr.empty())
        return {addr};
    return {"::", "0.0.0.0"};
}

void shareAddress(Handle h) {
#ifdef _WIN32
    int on = 1;
    setsockopt(h, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
#else
    int on = 1;
    setsockopt(h, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
#endif
}

void openBothStacks(Handle h, int family) {
    if (family != AF_INET6)
        return;
    int off = 0;
    setsockopt(h, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
}

}

Listener::~Listener() {
    shut();
}

bool Listener::open(const std::string& addr, uint16_t port) {
    startupSockets();

    for (const auto& host : candidateHosts(addr)) {
        Peer local;
        if (!resolveHost(host, port, true, local))
            continue;

        fd_ = socket(local.sa.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (!alive(fd_))
            continue;

        shareAddress(fd_);
        openBothStacks(fd_, local.sa.ss_family);

        if (bind(fd_, reinterpret_cast<const sockaddr*>(&local.sa),
                 static_cast<socklen_t>(local.len)) == 0 &&
            ::listen(fd_, 64) == 0) {
            unblock(fd_);
            return true;
        }
        closeHandle(fd_);
    }
    return false;
}

Handle Listener::take(Peer& from) {
    if (!alive(fd_))
        return kNoHandle;

    socklen_t len = sizeof(from.sa);
    Handle fresh = ::accept(fd_, reinterpret_cast<sockaddr*>(&from.sa), &len);
    if (!alive(fresh))
        return kNoHandle;

    from.len = static_cast<int>(len);
    unblock(fresh);

    int on = 1;
    setsockopt(fresh, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
    return fresh;
}

void Listener::shut() {
    closeHandle(fd_);
}

Datagram::~Datagram() {
    shut();
}

bool Datagram::open(const std::string& addr, uint16_t port) {
    startupSockets();

    for (const auto& host : candidateHosts(addr)) {
        Peer local;
        if (!resolveHost(host, port, false, local))
            continue;

        fd_ = socket(local.sa.ss_family, SOCK_DGRAM, IPPROTO_UDP);
        if (!alive(fd_))
            continue;

        openBothStacks(fd_, local.sa.ss_family);

#ifdef _WIN32
        BOOL behave = FALSE;
        DWORD ret = 0;
        WSAIoctl(fd_, SIO_UDP_CONNRESET, &behave, sizeof(behave), nullptr, 0, &ret, nullptr,
                 nullptr);
#endif

        int bufSize = 512 * 1024;
        setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSize),
                   sizeof(bufSize));

        if (bind(fd_, reinterpret_cast<const sockaddr*>(&local.sa),
                 static_cast<socklen_t>(local.len)) == 0)
            return true;

        closeHandle(fd_);
    }
    return false;
}

int Datagram::recv(uint8_t* buf, size_t len, Peer& from, int timeoutMs) {
    if (!alive(fd_))
        return -1;

    PollSlot slot;
    std::memset(&slot, 0, sizeof(slot));
    slot.fd = fd_;
    slot.events = POLLIN;

    if (waitOn(&slot, 1, timeoutMs) <= 0)
        return 0;

    socklen_t fromLen = sizeof(from.sa);
    int n = static_cast<int>(recvfrom(fd_, reinterpret_cast<char*>(buf), static_cast<int>(len), 0,
                                      reinterpret_cast<sockaddr*>(&from.sa), &fromLen));
    if (n <= 0)
        return 0;

    from.len = static_cast<int>(fromLen);
    return n;
}

bool Datagram::send(const uint8_t* buf, size_t len, const Peer& to) {
    if (!alive(fd_) || !to.valid())
        return false;

    int n = static_cast<int>(sendto(fd_, reinterpret_cast<const char*>(buf), static_cast<int>(len),
                                    0, reinterpret_cast<const sockaddr*>(&to.sa),
                                    static_cast<socklen_t>(to.len)));
    return n == static_cast<int>(len);
}

void Datagram::shut() {
    closeHandle(fd_);
}

int pullBytes(Handle h, uint8_t* buf, size_t len) {
    if (!alive(h))
        return -1;

    int n = static_cast<int>(::recv(h, reinterpret_cast<char*>(buf), static_cast<int>(len), 0));
    if (n > 0)
        return n;
    if (n == 0)
        return -1;
    return wouldBlock() ? 0 : -1;
}

int pushBytes(Handle h, const uint8_t* buf, size_t len) {
    if (!alive(h))
        return -1;

    int n = static_cast<int>(::send(h, reinterpret_cast<const char*>(buf), static_cast<int>(len), 0));
    if (n >= 0)
        return n;
    return wouldBlock() ? 0 : -1;
}

}
