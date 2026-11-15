#include "relay.h"

#include <vector>
#include "protocol.h"

namespace coop {

namespace {
constexpr uint32_t kClientTimeoutMs = 15000;
constexpr size_t kMaxClients = 20000;
constexpr size_t kMaxRoomName = 64;
constexpr uint8_t kRoomFull = 255;
}  // namespace

void Relay::pump(uint32_t nowMs) {
    uint8_t buffer[2048];
    Endpoint from;
    int size;
    while ((size = socket_.recvFrom(from, buffer, sizeof(buffer))) >= 0) {
        if (static_cast<size_t>(size) > kMaxDatagram + 64) continue;
        ByteReader r(buffer, static_cast<size_t>(size));
        PacketType type;
        bool versionOk = false;
        if (!readPacketHeader(r, type, versionOk)) continue;
        if (type == PacketType::RelayRegister) {
            std::string room = r.str();
            // An outdated client still gets an answer: its link reports the version mismatch from our header.
            if (!versionOk) sendStatus(from, 0);
            else if (r.ok() && !room.empty() && room.size() <= kMaxRoomName) handleRegister(from, room, nowMs);
        } else if (type != PacketType::RelayStatus) {
            forward(from, buffer, static_cast<size_t>(size), nowMs);
        }
    }

    if (nowMs - lastSweepMs_ >= 1000) {
        lastSweepMs_ = nowMs;
        std::vector<Endpoint> expired;
        for (const auto& [endpoint, client] : clients_)
            if (nowMs - client.lastSeenMs > kClientTimeoutMs) expired.push_back(endpoint);
        for (const auto& endpoint : expired) removeClient(endpoint);
    }
}

void Relay::handleRegister(const Endpoint& from, const std::string& room, uint32_t nowMs) {
    auto existing = clients_.find(from);
    if (existing != clients_.end() && existing->second.room != room) {
        removeClient(from);
        existing = clients_.end();
    }
    if (existing == clients_.end()) {
        if (clients_.size() >= kMaxClients) return;
        Room& r = rooms_[room];
        if (r.count >= 2) {
            sendStatus(from, kRoomFull);
            return;
        }
        r.members[r.count++] = from;
        clients_[from] = Client{room, nowMs};
        // Let the player who was already waiting know their friend has arrived.
        for (int i = 0; i + 1 < r.count; ++i) sendStatus(r.members[i], static_cast<uint8_t>(r.count));
    } else {
        existing->second.lastSeenMs = nowMs;
    }
    sendStatus(from, static_cast<uint8_t>(rooms_[room].count));
}

void Relay::forward(const Endpoint& from, const uint8_t* data, size_t size, uint32_t nowMs) {
    auto client = clients_.find(from);
    if (client == clients_.end()) return;
    client->second.lastSeenMs = nowMs;
    const Room& room = rooms_[client->second.room];
    for (int i = 0; i < room.count; ++i)
        if (room.members[i] != from) socket_.sendTo(room.members[i], data, size);
}

void Relay::removeClient(const Endpoint& who) {
    auto client = clients_.find(who);
    if (client == clients_.end()) return;
    auto room = rooms_.find(client->second.room);
    if (room != rooms_.end()) {
        Room& r = room->second;
        for (int i = 0; i < r.count; ++i) {
            if (r.members[i] == who) {
                r.members[i] = r.members[r.count - 1];
                --r.count;
                break;
            }
        }
        if (r.count == 0) rooms_.erase(room);
    }
    clients_.erase(client);
}

void Relay::sendStatus(const Endpoint& to, uint8_t peers) {
    ByteWriter w;
    writePacketHeader(w, PacketType::RelayStatus);
    w.u8(peers);
    socket_.sendTo(to, w.data.data(), w.data.size());
}

}  // namespace coop
