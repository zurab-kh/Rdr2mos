// Datagram socket abstraction. The real implementation is UdpSocket; tests plug in an in-memory one.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace coop {

struct Endpoint {
    uint32_t ip = 0;    // IPv4, host byte order
    uint16_t port = 0;

    bool valid() const { return port != 0; }
    bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const Endpoint& o) const { return !(*this == o); }
    bool operator<(const Endpoint& o) const { return ip != o.ip ? ip < o.ip : port < o.port; }
    std::string toString() const;
};

class IDatagramSocket {
public:
    virtual ~IDatagramSocket() = default;
    virtual bool sendTo(const Endpoint& to, const uint8_t* data, size_t size) = 0;
    // Non-blocking. Returns the datagram size, or -1 when nothing is waiting.
    virtual int recvFrom(Endpoint& from, uint8_t* buffer, size_t capacity) = 0;
};

class UdpSocket final : public IDatagramSocket {
public:
    UdpSocket();
    ~UdpSocket() override;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // port 0 picks an ephemeral port.
    bool open(uint16_t port, std::string* error = nullptr);
    void close();
    bool isOpen() const;
    uint16_t localPort() const;

    bool sendTo(const Endpoint& to, const uint8_t* data, size_t size) override;
    int recvFrom(Endpoint& from, uint8_t* buffer, size_t capacity) override;

    // Blocks up to timeoutMs until a datagram is readable. Used by the relay server.
    bool waitReadable(int timeoutMs);

    // Resolves "host" or "host:port" (IPv4). defaultPort is used when no port is given.
    static bool resolve(const std::string& address, uint16_t defaultPort, Endpoint& out);

private:
    intptr_t fd_;
};

}  // namespace coop
