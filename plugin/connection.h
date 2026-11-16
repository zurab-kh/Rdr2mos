// The player's connection to the server, kept apart from the script instance that uses it.
//
// Script Hook restarts the mod's script after every save load: scriptMain() runs again and a new
// App replaces the old one, whose fiber is simply abandoned. A socket and link owned by the old App
// went with it without a word, so the server kept that session for the whole link timeout: a
// frozen puppet for the others, the world held by a silent owner, the player's name taken (the
// reconnect came back as "A 2") and, in a full campaign, no slot for the reconnect. Kept in a
// static instead (plugin/main.cpp), the old session ends with a Bye as soon as the new script runs,
// and the reconnect comes from the same address and port, which the server takes as this player
// starting over (it replaces the old session even when the Bye is lost).
//
// Portable (no Windows calls), so the tests can drive it over the simulated network.
#pragma once
#include <cstdint>

#include "link.h"

namespace coop {

class ServerConnection {
public:
    Link& link() { return link_; }
    const Link& link() const { return link_; }
    // From start() to stop() or retire(): a session is under way or being set up.
    bool active() const { return link_.state() != Link::State::Idle; }

    // A new script instance took over: the session the old one had ends now (a Bye when it was
    // connected), and whatever the server sent it is dropped with it. The socket stays open, so
    // the next start() comes from the same port.
    void retire(IDatagramSocket* socket) {
        if (active()) link_.stop();
        drain(socket);
    }

    // Starts a session with config.remote over `socket`.
    void start(IDatagramSocket* socket, LinkConfig config, uint32_t nowMs) {
        if (active()) link_.stop();
        // The server's cookie belongs to our address, not to a session: on the same socket it still
        // opens the door, which saves the reconnect a round trip (an expired one only costs that).
        if (socket && socket == socket_ && config.remote == remote_) config.local.cookie = link_.local().cookie;
        // Datagrams still waiting belong to the session before: a late Hello from the server would
        // otherwise be taken for the answer to this session's Hello.
        drain(socket);
        socket_ = socket;
        remote_ = config.remote;
        link_.start(socket, config, nowMs);
    }

    // The player disconnects: a Bye, and the caller closes the socket.
    void stop() {
        link_.stop();
        socket_ = nullptr;
    }

private:
    static void drain(IDatagramSocket* socket) {
        if (!socket) return;
        uint8_t buffer[2048];
        Endpoint from;
        for (int n = 0; n < 4096 && socket->recvFrom(from, buffer, sizeof buffer) >= 0; ++n) {
        }
    }

    Link link_;
    IDatagramSocket* socket_ = nullptr;  // the socket the link last ran on (only compared)
    Endpoint remote_;
};

}  // namespace coop
