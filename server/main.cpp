// RDR2 Co-op relay server.
//
// Run it on any machine both players can reach (a cheap VPS is enough):
//     rdr2coop-relay [port]
// Then both players set  connection = relay  and the same  room  in RDR2Coop.ini.
// The relay only forwards packets between the two members of a room; it keeps no game state.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "protocol.h"
#include "relay.h"

int main(int argc, char** argv) {
    int port = coop::kDefaultPort;
    if (argc > 1) {
        port = std::atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            std::fprintf(stderr, "usage: %s [port]   (default %d)\n", argv[0], coop::kDefaultPort);
            return 2;
        }
    }

    coop::UdpSocket socket;
    std::string error;
    if (!socket.open(static_cast<uint16_t>(port), &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::printf("RDR2 Co-op relay listening on UDP port %d (protocol v%d)\n", port, coop::kProtocolVersion);
    std::fflush(stdout);

    coop::Relay relay(socket);
    auto start = std::chrono::steady_clock::now();
    size_t lastRooms = 0, lastClients = 0;
    for (;;) {
        socket.waitReadable(250);
        auto elapsed = std::chrono::steady_clock::now() - start;
        relay.pump(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
        if (relay.roomCount() != lastRooms || relay.clientCount() != lastClients) {
            lastRooms = relay.roomCount();
            lastClients = relay.clientCount();
            std::printf("rooms: %zu, players: %zu\n", lastRooms, lastClients);
            std::fflush(stdout);
        }
    }
}
