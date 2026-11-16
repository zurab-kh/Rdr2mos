// Connection between a player and the server over UDP: handshake, reconnect, a reliable ordered
// channel for events and an unreliable channel for state.
//
// Reliable channel: records carry 16-bit ids. The receiver keeps records that arrive ahead of a gap
// (up to kReliableWindow) and tells the sender which ones it holds (selective acknowledgement), so a
// lost or reordered datagram costs one resend instead of everything sent after it. Resends wait for a
// timeout derived from the measured round trip, and come sooner for a record that later ones have
// already overtaken.
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include "protocol.h"
#include "socket.h"

namespace coop {

struct LinkConfig {
    bool listen = false;     // server side: wait for a player to connect
    Endpoint remote;         // server address (player side); unused when listening
    HelloInfo local;         // nonce is filled in by the link
};

class Link {
public:
    enum class State { Idle, Connecting, Connected, Failed };

    // Largest payload one message may have: every record must fit one datagram, with the packet
    // header and a selective acknowledgement next to it.
    static constexpr size_t kMaxPayload = kMaxDatagram - 8 - 20 - 6;
    static constexpr size_t kReliableWindow = 128;  // records in flight; a power of two

    void start(IDatagramSocket* socket, const LinkConfig& config, uint32_t nowMs);
    void stop();  // tells the peer we are leaving

    // Receives and processes everything waiting on the socket. Call once per frame, before poll().
    void update(uint32_t nowMs);
    // Hands the link one datagram that arrived from `from` (for a server that reads the socket itself
    // and passes each client's datagrams on); update() still has to run for timeouts.
    void receive(const Endpoint& from, const uint8_t* data, size_t size, uint32_t nowMs);
    // Sends queued messages. Call once per frame, after queuing.
    void flush(uint32_t nowMs);

    // Payloads above kMaxPayload are refused. Reliable messages are delivered in order.
    void send(Msg type, const uint8_t* data, size_t size, bool reliable);
    void send(Msg type, const std::vector<uint8_t>& payload, bool reliable) {
        send(type, payload.data(), payload.size(), reliable);
    }
    void send(Msg type, std::vector<uint8_t>&& payload, bool reliable);
    // The same with a leading byte: the id of the player a relayed message came from.
    void sendFrom(uint8_t source, Msg type, const std::vector<uint8_t>& payload, bool reliable);
    template <typename T>
    void send(Msg type, const T& message, bool reliable) {
        // Encoded straight into the outgoing buffers: no allocation per message once warmed up.
        if (state_ != State::Connected) return;
        ByteWriter w;
        if (reliable) w.data = takeSpare();
        else beginUnreliable(w, type);
        write(w, message);
        if (reliable) queueReliable(type, std::move(w.data));
        else endUnreliable(w);
    }
    // payload's storage is reused, so polling into the same vector in a loop does not allocate.
    bool poll(Msg& type, std::vector<uint8_t>& payload);

    State state() const { return state_; }
    bool connected() const { return state_ == State::Connected; }
    const HelloInfo& peer() const { return peer_; }
    const HelloInfo& local() const { return config_.local; }
    // Why the link failed, or (while a direct host keeps waiting) why the last attempt was refused.
    const std::string& error() const { return error_; }
    int pingMs() const { return pingMs_; }
    // Smoothed round trip of reliable records (includes the peer's frame time), -1 before the first.
    int rttMs() const { return srttMs_; }
    // Bumped every time a (new) session with the peer is established.
    uint32_t epoch() const { return epoch_; }
    // Traffic over the last second, in bytes per second (UDP payload, without IP/UDP headers).
    uint32_t sendRate() const { return sendRate_; }
    // Reliable messages queued and not yet acknowledged (bulk transfers throttle on this).
    size_t pendingReliable() const { return outReliable_.size() - sackedCount_; }
    uint32_t receiveRate() const { return receiveRate_; }
    // Reliable records sent again since start() (for tests and the HUD).
    uint32_t resends() const { return resends_; }

private:
    struct Pending {
        uint16_t id = 0;
        Msg type = Msg::Ping;
        std::vector<uint8_t> payload;
        uint32_t lastSentMs = 0;
        uint8_t sends = 0;     // 0 = not sent yet
        bool acked = false;    // selectively acknowledged, kept until the cumulative ack passes it
    };
    struct Slot {
        bool have = false;
        Msg type = Msg::Ping;
        std::vector<uint8_t> payload;
    };
    struct Inbound {
        Msg type;
        uint32_t offset, size;  // in inboxBytes_
    };

    void handleDatagram(const Endpoint& from, const uint8_t* data, size_t size, uint32_t nowMs);
    void handleHello(const Endpoint& from, ByteReader& r, uint32_t nowMs);
    void handleChallenge(ByteReader& r, uint32_t nowMs);
    void handleData(ByteReader& r, uint32_t nowMs);
    void acknowledge(uint16_t ack, uint32_t nowMs);
    void selectiveAck(uint16_t ack, const uint8_t* bits, size_t size, uint32_t nowMs);
    void ackRecord(Pending& p, uint32_t nowMs);
    void receiveReliable(uint16_t id, Msg type, const uint8_t* body, size_t size);
    void drainReliable();
    bool deliver(Msg type, const uint8_t* body, size_t size);
    uint32_t resendTimeout(const Pending& p, uint32_t nowMs) const;
    void rttSample(int sampleMs);
    void sendHello(const Endpoint& to, uint32_t nowMs);
    void beginConnecting(uint32_t nowMs);
    void resetSession();
    void fail(const std::string& why);
    void transmit(const Endpoint& to, const uint8_t* data, size_t size);
    void updateRates(uint32_t nowMs);
    uint16_t sessionKey() const;
    void beginUnreliable(ByteWriter& w, Msg type);
    void endUnreliable(ByteWriter& w);
    void queueReliable(Msg type, std::vector<uint8_t>&& payload);
    std::vector<uint8_t> takeSpare();

    IDatagramSocket* socket_ = nullptr;
    LinkConfig config_;
    State state_ = State::Idle;
    std::string error_;
    HelloInfo peer_;
    Endpoint remote_;
    uint32_t epoch_ = 0;
    int pingMs_ = -1;
    uint32_t bytesOut_ = 0, bytesIn_ = 0, sendRate_ = 0, receiveRate_ = 0, rateStartMs_ = 0;
    ByteWriter packet_;  // reused for every outgoing data packet

    uint32_t lastReceiveMs_ = 0;
    uint32_t lastSendMs_ = 0;
    uint32_t lastHelloMs_ = 0;
    uint32_t lastPingMs_ = 0;
    bool helloSent_ = false;
    bool ackDirty_ = false;
    std::array<uint32_t, 4> pastPeerNonces_{};  // server sessions we have moved on from
    size_t pastPeerIndex_ = 0;

    // Unreliable records, already framed (type, flags, length, body), waiting for the next flush.
    std::vector<uint8_t> outUnreliable_;
    size_t unreliableMark_ = 0;
    bool pongDue_ = false;
    std::array<uint8_t, 8> pong_{};
    uint8_t pongSize_ = 0;

    // Reliable, outgoing: consecutive ids, oldest first.
    std::deque<Pending> outReliable_;
    size_t outReliableBytes_ = 0, sackedCount_ = 0;
    std::vector<std::vector<uint8_t>> spare_;  // payload buffers of acknowledged records, for reuse
    uint16_t nextReliableId_ = 0;
    bool haveSacked_ = false;
    uint16_t highestSacked_ = 0;
    int srttMs_ = -1, rttVarMs_ = 0;
    uint32_t resends_ = 0;

    // Reliable, incoming: records ahead of a gap wait here (indexed by id % window).
    std::array<Slot, kReliableWindow> rx_;
    size_t rxBuffered_ = 0;
    uint16_t expectedReliableId_ = 0;

    // Delivered messages, their bodies packed into one buffer.
    std::deque<Inbound> inbox_;
    std::vector<uint8_t> inboxBytes_;
};

}  // namespace coop
