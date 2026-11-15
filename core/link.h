// Two-player connection over UDP: handshake, reconnect, a reliable ordered channel for events
// and an unreliable channel for state, plus optional rendezvous through the relay server.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include "protocol.h"
#include "socket.h"

namespace coop {

struct LinkConfig {
    bool listen = false;     // direct mode, waiting for the friend to connect to our port
    bool viaRelay = false;   // `remote` is a relay server; peers meet in `room`
    Endpoint remote;         // friend (direct join) or relay address; unused when listening
    std::string room;
    HelloInfo local;         // nonce is filled in by the link
};

class Link {
public:
    enum class State { Idle, Connecting, Connected, Failed };

    void start(IDatagramSocket* socket, const LinkConfig& config, uint32_t nowMs);
    void stop();  // tells the peer we are leaving

    // Receives and processes everything waiting on the socket. Call once per frame, before poll().
    void update(uint32_t nowMs);
    // Sends queued messages. Call once per frame, after queuing.
    void flush(uint32_t nowMs);

    // Unreliable payloads must fit one datagram (about 1100 bytes); reliable ones are delivered in order.
    void send(Msg type, const std::vector<uint8_t>& payload, bool reliable);
    template <typename T>
    void send(Msg type, const T& message, bool reliable) { send(type, encode(message), reliable); }
    bool poll(Msg& type, std::vector<uint8_t>& payload);

    State state() const { return state_; }
    bool connected() const { return state_ == State::Connected; }
    const HelloInfo& peer() const { return peer_; }
    const HelloInfo& local() const { return config_.local; }
    // Why the link failed, or (while a direct host keeps waiting) why the last attempt was refused.
    const std::string& error() const { return error_; }
    int pingMs() const { return pingMs_; }
    // Bumped every time a (new) session with the peer is established.
    uint32_t epoch() const { return epoch_; }
    // Relay mode: players currently in the room (0 = relay not answering yet).
    int relayPeers() const { return relayPeers_; }
    // Traffic over the last second, in bytes per second (UDP payload, without IP/UDP headers).
    uint32_t sendRate() const { return sendRate_; }
    uint32_t receiveRate() const { return receiveRate_; }

private:
    struct Record {
        Msg type;
        std::vector<uint8_t> payload;
    };
    struct Pending {
        uint16_t id;
        Record record;
        uint32_t lastSentMs;
        bool sent;
    };

    void handleDatagram(const Endpoint& from, const uint8_t* data, size_t size, uint32_t nowMs);
    void handleHello(const Endpoint& from, ByteReader& r, uint32_t nowMs);
    void handleData(ByteReader& r, uint32_t nowMs);
    void sendHello(const Endpoint& to, uint32_t nowMs);
    void sendRegister(uint32_t nowMs);
    void beginConnecting(uint32_t nowMs);
    void resetSession();
    void fail(const std::string& why);
    void transmit(const Endpoint& to, const uint8_t* data, size_t size);
    void updateRates(uint32_t nowMs);
    uint16_t sessionKey() const;

    IDatagramSocket* socket_ = nullptr;
    LinkConfig config_;
    State state_ = State::Idle;
    std::string error_;
    HelloInfo peer_;
    Endpoint remote_;
    uint32_t epoch_ = 0;
    int pingMs_ = -1;
    int relayPeers_ = 0;
    uint32_t bytesOut_ = 0, bytesIn_ = 0, sendRate_ = 0, receiveRate_ = 0, rateStartMs_ = 0;
    ByteWriter packet_;  // reused for every outgoing data packet

    uint32_t lastReceiveMs_ = 0;
    uint32_t lastSendMs_ = 0;
    uint32_t lastHelloMs_ = 0;
    uint32_t lastRegisterMs_ = 0;
    uint32_t lastPingMs_ = 0;
    bool helloSent_ = false;
    bool registerSent_ = false;
    bool ackDirty_ = false;

    std::vector<Record> outUnreliable_;
    std::deque<Pending> outReliable_;
    uint16_t nextReliableId_ = 0;
    uint16_t expectedReliableId_ = 0;
    std::deque<Record> inbox_;
};

}  // namespace coop
