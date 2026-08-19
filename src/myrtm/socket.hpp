#ifndef MYRTM_SOCKET_H
#define MYRTM_SOCKET_H

#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace myrtm {

struct SockAddr {
    sockaddr_storage sa{};
    int len = 0;

    bool valid() const { return len > 0; }
    bool sameAs(const SockAddr& o) const;
    std::string text() const;
};

bool resolveAddr(const std::string& host, uint16_t port, SockAddr& out);
bool parseEndpoint(const std::string& text, SockAddr& out);

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool bindTo(const std::string& addr, uint16_t port);
    void shut();

    int recvFrom(uint8_t* buf, size_t len, SockAddr& from, int timeoutMs);
    bool sendTo(const uint8_t* buf, size_t len, const SockAddr& to);

    uint16_t localPort() const;
    bool valid() const;

private:
#ifdef _WIN32
    SOCKET fd_ = INVALID_SOCKET;
#else
    int fd_ = -1;
#endif
};

}

#endif
