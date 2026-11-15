#include "link.h"

#include <random>

namespace coop {

namespace {
constexpr uint32_t kHelloIntervalMs = 300;
constexpr uint32_t kResendMs = 150;
constexpr uint32_t kKeepAliveMs = 250;
constexpr uint32_t kPingIntervalMs = 1000;
constexpr uint32_t kTimeoutMs = 10000;
constexpr size_t kReliableWindow = 32;
constexpr size_t kMaxQueuedReliable = 4096;
constexpr size_t kMaxInbox = 4096;
constexpr uint8_t kRecordReliable = 1;
constexpr uint8_t kRelayRoomFull = 255;

uint32_t randomNonce() {
    static std::mt19937 rng{std::random_device{}()};
    uint32_t n = rng();
    return n ? n : 1;
}

// True when a is before b on the 16-bit sequence circle.
bool seqBefore(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b) < 0; }
}  // namespace

void Link::start(IDatagramSocket* socket, const LinkConfig& config, uint32_t nowMs) {
    socket_ = socket;
    config_ = config;
    error_.clear();
    relayPeers_ = 0;
    beginConnecting(nowMs);
}

void Link::stop() {
    if (socket_ && state_ == State::Connected) {
        ByteWriter w;
        writePacketHeader(w, PacketType::Bye);
        w.u16(sessionKey());
        transmit(remote_, w.data.data(), w.data.size());
    }
    state_ = State::Idle;
    resetSession();
}

void Link::beginConnecting(uint32_t nowMs) {
    state_ = State::Connecting;
    resetSession();
    // A fresh nonce makes the peer drop whatever it remembers about our previous session.
    config_.local.nonce = randomNonce();
    remote_ = config_.listen && !config_.viaRelay ? Endpoint{} : config_.remote;
    lastReceiveMs_ = nowMs;
    helloSent_ = false;
    registerSent_ = false;
}

void Link::resetSession() {
    outUnreliable_.clear();
    outReliable_.clear();
    inbox_.clear();
    nextReliableId_ = 0;
    expectedReliableId_ = 0;
    ackDirty_ = false;
    pingMs_ = -1;
    peer_ = HelloInfo{};
}

void Link::fail(const std::string& why) {
    state_ = State::Failed;
    error_ = why;
    resetSession();
}

uint16_t Link::sessionKey() const {
    uint32_t mixed = config_.local.nonce ^ peer_.nonce;
    return static_cast<uint16_t>(mixed ^ (mixed >> 16));
}

void Link::send(Msg type, const std::vector<uint8_t>& payload, bool reliable) {
    if (state_ != State::Connected) return;
    if (reliable) {
        if (outReliable_.size() >= kMaxQueuedReliable) return;  // peer is gone; the timeout will handle it
        outReliable_.push_back(Pending{nextReliableId_++, Record{type, payload}, 0, false});
    } else {
        outUnreliable_.push_back(Record{type, payload});
    }
}

bool Link::poll(Msg& type, std::vector<uint8_t>& payload) {
    if (inbox_.empty()) return false;
    type = inbox_.front().type;
    payload = std::move(inbox_.front().payload);
    inbox_.pop_front();
    return true;
}

void Link::sendHello(const Endpoint& to, uint32_t nowMs) {
    if (!to.valid()) return;
    ByteWriter w;
    writePacketHeader(w, PacketType::Hello);
    write(w, config_.local);
    transmit(to, w.data.data(), w.data.size());
    lastHelloMs_ = nowMs;
    helloSent_ = true;
}

void Link::sendRegister(uint32_t nowMs) {
    ByteWriter w;
    writePacketHeader(w, PacketType::RelayRegister);
    w.str(config_.room);
    transmit(config_.remote, w.data.data(), w.data.size());
    lastRegisterMs_ = nowMs;
    registerSent_ = true;
}

void Link::update(uint32_t nowMs) {
    if (!socket_ || state_ == State::Idle || state_ == State::Failed) return;
    updateRates(nowMs);

    uint8_t buffer[2048];
    Endpoint from;
    int size;
    while ((size = socket_->recvFrom(from, buffer, sizeof(buffer))) >= 0) {
        bytesIn_ += static_cast<uint32_t>(size);
        handleDatagram(from, buffer, static_cast<size_t>(size), nowMs);
        if (state_ == State::Failed) return;
    }

    if (config_.viaRelay) {
        uint32_t interval = state_ == State::Connected ? 5000u : 1000u;
        if (!registerSent_ || nowMs - lastRegisterMs_ >= interval) sendRegister(nowMs);
    }

    if (state_ == State::Connecting) {
        if (!helloSent_ || nowMs - lastHelloMs_ >= kHelloIntervalMs) sendHello(remote_, nowMs);
    } else if (state_ == State::Connected && nowMs - lastReceiveMs_ > kTimeoutMs) {
        beginConnecting(nowMs);  // lost the peer: fall back to waiting / reconnecting
    }
}

void Link::handleDatagram(const Endpoint& from, const uint8_t* data, size_t size, uint32_t nowMs) {
    ByteReader r(data, size);
    PacketType type;
    bool versionOk = false;
    if (!readPacketHeader(r, type, versionOk)) return;

    // Until a direct host has locked on to its friend it accepts a Hello from anywhere.
    bool acceptAnySource = config_.listen && !config_.viaRelay && state_ == State::Connecting;
    if (!acceptAnySource && from != remote_) return;

    if (!versionOk) {
        if (type == PacketType::Hello || type == PacketType::RelayStatus)
            fail("mod version mismatch: both players (and the relay) need the same version");
        return;
    }

    switch (type) {
        case PacketType::Hello:
            handleHello(from, r, nowMs);
            break;
        case PacketType::Data:
            if (state_ == State::Connected) handleData(r, nowMs);
            break;
        case PacketType::Bye:
            if (state_ == State::Connected && r.u16() == sessionKey() && r.ok()) beginConnecting(nowMs);
            break;
        case PacketType::RelayStatus:
            if (config_.viaRelay) {
                uint8_t peers = r.u8();
                if (!r.ok()) break;
                if (peers == kRelayRoomFull) fail("relay room is full: pick another room name");
                else relayPeers_ = peers;
            }
            break;
        default:
            break;
    }
}

void Link::handleHello(const Endpoint& from, ByteReader& r, uint32_t nowMs) {
    HelloInfo hello;
    if (!read(r, hello)) return;
    if (hello.role == config_.local.role) {
        const char* why = "both players are set to the same role: one must be host, the other guest";
        // Answer first so the other side sees the same problem.
        if (nowMs - lastHelloMs_ >= kHelloIntervalMs / 2) sendHello(from, nowMs);
        // A direct host keeps listening (a stranger must not be able to shut it down); it only shows the reason.
        if (config_.listen && !config_.viaRelay) error_ = why;
        else fail(why);
        return;
    }
    bool newSession = state_ != State::Connected || hello.nonce != peer_.nonce;
    if (newSession) {
        resetSession();
        peer_ = hello;
        remote_ = from;
        error_.clear();
        state_ = State::Connected;
        ++epoch_;
        lastPingMs_ = nowMs - kPingIntervalMs;
    }
    lastReceiveMs_ = nowMs;
    // Answer so the peer learns about us even if our earlier Hello was lost (rate limited).
    if (newSession || nowMs - lastHelloMs_ >= kHelloIntervalMs / 2) sendHello(remote_, nowMs);
}

void Link::handleData(ByteReader& r, uint32_t nowMs) {
    uint16_t key = r.u16();
    uint16_t ack = r.u16();
    if (!r.ok() || key != sessionKey()) return;
    lastReceiveMs_ = nowMs;

    while (!outReliable_.empty() && seqBefore(outReliable_.front().id, ack)) outReliable_.pop_front();

    while (r.remaining() > 0) {
        Msg type = static_cast<Msg>(r.u8());
        uint8_t flags = r.u8();
        uint16_t reliableId = (flags & kRecordReliable) ? r.u16() : 0;
        uint16_t length = r.u16();
        const uint8_t* body = r.take(length);
        if (!r.ok()) return;

        if (flags & kRecordReliable) {
            ackDirty_ = true;
            if (reliableId != expectedReliableId_) continue;  // duplicate, or a gap the sender will refill
            ++expectedReliableId_;
        }
        if (type == Msg::Ping) {
            outUnreliable_.push_back(Record{Msg::Pong, std::vector<uint8_t>(body, body + length)});
        } else if (type == Msg::Pong) {
            ByteReader pr(body, length);
            uint32_t sentAt = pr.u32();
            if (pr.ok()) pingMs_ = static_cast<int>(nowMs - sentAt);
        } else if (inbox_.size() < kMaxInbox) {
            inbox_.push_back(Record{type, std::vector<uint8_t>(body, body + length)});
        }
    }
}

void Link::transmit(const Endpoint& to, const uint8_t* data, size_t size) {
    if (socket_->sendTo(to, data, size)) bytesOut_ += static_cast<uint32_t>(size);
}

void Link::updateRates(uint32_t nowMs) {
    uint32_t elapsed = nowMs - rateStartMs_;
    if (elapsed < 1000) return;
    if (elapsed < 3000) {
        sendRate_ = static_cast<uint32_t>(uint64_t(bytesOut_) * 1000 / elapsed);
        receiveRate_ = static_cast<uint32_t>(uint64_t(bytesIn_) * 1000 / elapsed);
    }
    bytesOut_ = bytesIn_ = 0;
    rateStartMs_ = nowMs;
}

void Link::flush(uint32_t nowMs) {
    if (!socket_ || state_ != State::Connected) return;

    if (nowMs - lastPingMs_ >= kPingIntervalMs) {
        ByteWriter ping;
        ping.u32(nowMs);
        outUnreliable_.push_back(Record{Msg::Ping, std::move(ping.data)});
        lastPingMs_ = nowMs;
    }

    ByteWriter& w = packet_;
    size_t records = 0;
    auto begin = [&] {
        w.data.clear();
        writePacketHeader(w, PacketType::Data);
        w.u16(sessionKey());
        w.u16(expectedReliableId_);
        records = 0;
    };
    auto sendPacket = [&] {
        transmit(remote_, w.data.data(), w.data.size());
        lastSendMs_ = nowMs;
        ackDirty_ = false;
    };
    auto append = [&](const Record& rec, bool reliable, uint16_t id) {
        size_t need = rec.payload.size() + 6;
        if (records > 0 && w.data.size() + need > kMaxDatagram) {
            sendPacket();
            begin();
        }
        w.u8(static_cast<uint8_t>(rec.type));
        w.u8(reliable ? kRecordReliable : 0);
        if (reliable) w.u16(id);
        w.u16(static_cast<uint16_t>(rec.payload.size()));
        w.bytes(rec.payload.data(), rec.payload.size());
        ++records;
    };

    begin();
    size_t window = 0;
    for (auto& pending : outReliable_) {
        if (window++ >= kReliableWindow) break;
        if (pending.sent && nowMs - pending.lastSentMs < kResendMs) continue;
        append(pending.record, true, pending.id);
        pending.sent = true;
        pending.lastSentMs = nowMs;
    }
    for (const auto& rec : outUnreliable_) append(rec, false, 0);
    outUnreliable_.clear();

    if (records > 0 || ackDirty_ || nowMs - lastSendMs_ >= kKeepAliveMs) sendPacket();
}

}  // namespace coop
