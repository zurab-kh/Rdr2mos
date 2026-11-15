// Rendezvous relay: two players who cannot open ports both connect out to this server,
// register the same room name, and the relay forwards their datagrams to each other.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include "socket.h"

namespace coop {

class Relay {
public:
    explicit Relay(IDatagramSocket& socket) : socket_(socket) {}

    // Processes all waiting datagrams and expires idle clients.
    void pump(uint32_t nowMs);
    size_t roomCount() const { return rooms_.size(); }
    size_t clientCount() const { return clients_.size(); }

private:
    struct Room {
        Endpoint members[2];
        int count = 0;
    };
    struct Client {
        std::string room;
        uint32_t lastSeenMs = 0;
    };

    void handleRegister(const Endpoint& from, const std::string& room, uint32_t nowMs);
    void forward(const Endpoint& from, const uint8_t* data, size_t size, uint32_t nowMs);
    void removeClient(const Endpoint& who);
    void sendStatus(const Endpoint& to, uint8_t peers);

    IDatagramSocket& socket_;
    std::map<std::string, Room> rooms_;
    std::map<Endpoint, Client> clients_;
    uint32_t lastSweepMs_ = 0;
};

}  // namespace coop
