#ifndef MYSIGNAL_NET_H
#define MYSIGNAL_NET_H

#include <cstdint>
#include <string>
#include <vector>

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
#include <poll.h>
#include <sys/socket.h>
#endif

namespace mysignal {

#ifdef _WIN32
using Handle = SOCKET;
using PollSlot = WSAPOLLFD;
const Handle kNoHandle = INVALID_SOCKET;
#else
using Handle = int;
using PollSlot = pollfd;
const Handle kNoHandle = -1;
#endif

struct Peer {
    sockaddr_storage sa{};
    int len = 0;

    bool valid() const { return len > 0; }
    std::string text() const;
    bool split(int& family, uint8_t addr[16], uint16_t& port) const;
};

uint64_t nowMs();
void startupSockets();
bool resolveHost(const std::string& host, uint16_t port, bool stream, Peer& out);
void closeHandle(Handle& h);
void unblock(Handle h);
int waitOn(PollSlot* slots, size_t count, int timeoutMs);
uint16_t boundPort(Handle h);

class Listener {
public:
    Listener() = default;
    ~Listener();

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    bool open(const std::string& addr, uint16_t port);
    Handle take(Peer& from);
    Handle handle() const { return fd_; }
    uint16_t port() const { return boundPort(fd_); }
    void shut();

private:
    Handle fd_ = kNoHandle;
};

class Datagram {
public:
    Datagram() = default;
    ~Datagram();

    Datagram(const Datagram&) = delete;
    Datagram& operator=(const Datagram&) = delete;

    bool open(const std::string& addr, uint16_t port);
    int recv(uint8_t* buf, size_t len, Peer& from, int timeoutMs);
    bool send(const uint8_t* buf, size_t len, const Peer& to);
    uint16_t port() const { return boundPort(fd_); }
    void shut();

private:
    Handle fd_ = kNoHandle;
};

int pullBytes(Handle h, uint8_t* buf, size_t len);
int pushBytes(Handle h, const uint8_t* buf, size_t len);

}

#endif
