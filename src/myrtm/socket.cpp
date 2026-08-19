#include "socket.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#endif

namespace myrtm {
namespace {

#ifdef _WIN32
struct WinsockGuard {
    WinsockGuard() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WinsockGuard() { WSACleanup(); }
};

void ensureWinsock() {
    static WinsockGuard guard;
    (void)guard;
}
#endif

}

bool SockAddr::sameAs(const SockAddr& o) const {
    if (len != o.len || len == 0)
        return false;
    if (sa.ss_family != o.sa.ss_family)
        return false;
    if (sa.ss_family == AF_INET) {
        const sockaddr_in* a = reinterpret_cast<const sockaddr_in*>(&sa);
        const sockaddr_in* b = reinterpret_cast<const sockaddr_in*>(&o.sa);
        return a->sin_port == b->sin_port &&
               std::memcmp(&a->sin_addr, &b->sin_addr, sizeof(a->sin_addr)) == 0;
    }
    if (sa.ss_family == AF_INET6) {
        const sockaddr_in6* a = reinterpret_cast<const sockaddr_in6*>(&sa);
        const sockaddr_in6* b = reinterpret_cast<const sockaddr_in6*>(&o.sa);
        return a->sin6_port == b->sin6_port &&
               std::memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(a->sin6_addr)) == 0;
    }
    return false;
}

std::string SockAddr::text() const {
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

bool parseEndpoint(const std::string& text, SockAddr& out) {
    std::string host;
    std::string port;

    if (!text.empty() && text[0] == '[') {
        size_t close = text.find(']');
        if (close == std::string::npos || close + 2 >= text.size() || text[close + 1] != ':')
            return false;
        host = text.substr(1, close - 1);
        port = text.substr(close + 2);
    } else {
        size_t colon = text.rfind(':');
        if (colon == std::string::npos || colon + 1 >= text.size())
            return false;
        host = text.substr(0, colon);
        port = text.substr(colon + 1);
    }

    unsigned value = 0;
    for (char c : port) {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + unsigned(c - '0');
        if (value > 65535)
            return false;
    }
    if (host.empty() || port.empty())
        return false;

    return resolveAddr(host, static_cast<uint16_t>(value), out);
}

bool resolveAddr(const std::string& host, uint16_t port, SockAddr& out) {
#ifdef _WIN32
    ensureWinsock();
#endif
    char portText[16];
    std::snprintf(portText, sizeof(portText), "%u", static_cast<unsigned>(port));

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

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

UdpSocket::UdpSocket() {
#ifdef _WIN32
    ensureWinsock();
#endif
}

UdpSocket::~UdpSocket() {
    shut();
}

bool UdpSocket::valid() const {
#ifdef _WIN32
    return fd_ != INVALID_SOCKET;
#else
    return fd_ >= 0;
#endif
}

bool UdpSocket::bindTo(const std::string& addr, uint16_t port) {
    SockAddr local;
    if (!resolveAddr(addr.empty() ? std::string("0.0.0.0") : addr, port, local))
        return false;

    fd_ = socket(local.sa.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (!valid())
        return false;

#ifdef _WIN32
    BOOL behave = FALSE;
    DWORD ret = 0;
    WSAIoctl(fd_, SIO_UDP_CONNRESET, &behave, sizeof(behave), nullptr, 0, &ret, nullptr, nullptr);
#endif

    if (local.sa.ss_family == AF_INET6) {
        int off = 0;
        setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
    }

    int bufSize = 512 * 1024;
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
    setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));

    if (bind(fd_, reinterpret_cast<const sockaddr*>(&local.sa),
             static_cast<socklen_t>(local.len)) != 0) {
        shut();
        return false;
    }
    return true;
}

void UdpSocket::shut() {
    if (!valid())
        return;
#ifdef _WIN32
    closesocket(fd_);
    fd_ = INVALID_SOCKET;
#else
    ::close(fd_);
    fd_ = -1;
#endif
}

uint16_t UdpSocket::localPort() const {
    if (!valid())
        return 0;
    sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getsockname(fd_, reinterpret_cast<sockaddr*>(&ss), &len) != 0)
        return 0;
    if (ss.ss_family == AF_INET)
        return ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
    if (ss.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
    return 0;
}

int UdpSocket::recvFrom(uint8_t* buf, size_t len, SockAddr& from, int timeoutMs) {
    if (!valid())
        return -1;

    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd_, &rd);

    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

#ifdef _WIN32
    int ready = select(0, &rd, nullptr, nullptr, &tv);
#else
    int ready = select(fd_ + 1, &rd, nullptr, nullptr, &tv);
#endif
    if (ready <= 0)
        return 0;

    socklen_t fromLen = sizeof(from.sa);
    int n = static_cast<int>(recvfrom(fd_, reinterpret_cast<char*>(buf), static_cast<int>(len), 0,
                                      reinterpret_cast<sockaddr*>(&from.sa), &fromLen));
    if (n <= 0)
        return 0;
    from.len = static_cast<int>(fromLen);
    return n;
}

bool UdpSocket::sendTo(const uint8_t* buf, size_t len, const SockAddr& to) {
    if (!valid() || !to.valid())
        return false;
    int n = static_cast<int>(sendto(fd_, reinterpret_cast<const char*>(buf), static_cast<int>(len),
                                    0, reinterpret_cast<const sockaddr*>(&to.sa),
                                    static_cast<socklen_t>(to.len)));
    return n == static_cast<int>(len);
}

}
