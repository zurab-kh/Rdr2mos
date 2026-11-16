#include "socket.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
using socklen_t = int;
using NativeSocket = SOCKET;
static const NativeSocket kInvalid = INVALID_SOCKET;
static void closeNative(NativeSocket s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using NativeSocket = int;
static const NativeSocket kInvalid = -1;
static void closeNative(NativeSocket s) { ::close(s); }
#endif

namespace coop {

static void ensureNetworkInit() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        done = true;
    }
#endif
}

std::string Endpoint::toString() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (ip >> 24) & 255u, (ip >> 16) & 255u, (ip >> 8) & 255u,
                  ip & 255u, static_cast<unsigned>(port));
    return buf;
}

UdpSocket::UdpSocket() : fd_(static_cast<intptr_t>(kInvalid)) {}
UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::isOpen() const { return static_cast<NativeSocket>(fd_) != kInvalid; }

bool UdpSocket::open(uint16_t port, std::string* error) {
    close();
    ensureNetworkInit();
    NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalid) {
        if (error) *error = "cannot create UDP socket";
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) *error = "cannot bind UDP port " + std::to_string(port) + " (already in use?)";
        closeNative(s);
        return false;
    }
#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    // Windows reports an ICMP "port unreachable" (a player that just quit) as an error on the next
    // receive of this socket. Those errors carry no datagram; switch them off at the source.
    BOOL reportResets = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &reportResets, sizeof(reportResets), nullptr, 0, &returned, nullptr, nullptr);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    // Bigger kernel buffers (best effort): a save transfer sends a window of ~64 KB at once, which
    // would overflow the small defaults while a game frame or a loading screen delays the next read.
    int bufferBytes = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufferBytes), sizeof(bufferBytes));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufferBytes), sizeof(bufferBytes));
    fd_ = static_cast<intptr_t>(s);
    return true;
}

void UdpSocket::close() {
    if (isOpen()) {
        closeNative(static_cast<NativeSocket>(fd_));
        fd_ = static_cast<intptr_t>(kInvalid);
    }
}

uint16_t UdpSocket::localPort() const {
    if (!isOpen()) return 0;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(static_cast<NativeSocket>(fd_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

bool UdpSocket::sendTo(const Endpoint& to, const uint8_t* data, size_t size) {
    if (!isOpen() || !to.valid()) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(to.ip);
    addr.sin_port = htons(to.port);
    auto sent = ::sendto(static_cast<NativeSocket>(fd_), reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
                         reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return sent == static_cast<decltype(sent)>(size);
}

int UdpSocket::recvFrom(Endpoint& from, uint8_t* buffer, size_t capacity) {
    if (!isOpen()) return -1;
    // A few retries skip the "connection reset" errors Windows reports on UDP sockets after
    // an ICMP port-unreachable, which would otherwise hide queued datagrams behind them.
    for (int attempt = 0; attempt < 8; ++attempt) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        auto n = ::recvfrom(static_cast<NativeSocket>(fd_), reinterpret_cast<char*>(buffer), static_cast<int>(capacity),
                            0, reinterpret_cast<sockaddr*>(&addr), &len);
        if (n >= 0) {
            from.ip = ntohl(addr.sin_addr.s_addr);
            from.port = ntohs(addr.sin_port);
            return static_cast<int>(n);
        }
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAECONNRESET || err == WSAEMSGSIZE) continue;
#endif
        break;
    }
    return -1;
}

bool UdpSocket::waitReadable(int timeoutMs) {
    if (!isOpen()) return false;
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(static_cast<NativeSocket>(fd_), &set);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    return select(0, &set, nullptr, nullptr, &tv) > 0;
#else
    pollfd p{static_cast<NativeSocket>(fd_), POLLIN, 0};
    return poll(&p, 1, timeoutMs) > 0;
#endif
}

bool UdpSocket::resolve(const std::string& address, uint16_t defaultPort, Endpoint& out) {
    ensureNetworkInit();
    std::string host = address;
    uint16_t port = defaultPort;
    size_t colon = address.rfind(':');
    if (colon != std::string::npos) {
        host = address.substr(0, colon);
        int parsed = std::atoi(address.c_str() + colon + 1);
        if (parsed <= 0 || parsed > 65535) return false;
        port = static_cast<uint16_t>(parsed);
    }
    if (host.empty()) return false;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) return false;
    out.ip = ntohl(reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr);
    out.port = port;
    freeaddrinfo(result);
    return true;
}

}  // namespace coop
