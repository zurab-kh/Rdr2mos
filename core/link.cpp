#include "link.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <random>

namespace coop {

namespace {
constexpr uint32_t kHelloIntervalMs = 300;
constexpr uint32_t kKeepAliveMs = 250;
constexpr uint32_t kPingIntervalMs = 1000;
constexpr uint32_t kTimeoutMs = 10000;
// Resend timeout: srtt + max(4 * rttvar, slack), within [min, max]; doubled per repeated resend.
constexpr uint32_t kInitialRtoMs = 200;
constexpr uint32_t kMinRtoMs = 30;
constexpr uint32_t kMaxRtoMs = 1000;
constexpr uint32_t kRtoSlackMs = 20;
constexpr uint32_t kMaxBackoffMs = 2000;
// Bounds on what one peer can make us hold.
constexpr size_t kMaxQueuedReliable = 4096;
constexpr size_t kMaxQueuedReliableBytes = 2u << 20;
constexpr size_t kMaxUnreliableBytes = 128u << 10;
constexpr size_t kMaxInbox = 4096;
constexpr size_t kMaxInboxBytes = 2u << 20;
constexpr int kMaxDatagramsPerUpdate = 1024;  // a flood must not keep the caller in update() forever
constexpr size_t kSpareBuffers = 64;
// Record flags.
constexpr uint8_t kRecordReliable = 1;
constexpr uint8_t kRecordSack = 2;  // body: bitmap of the records after the cumulative ack that arrived
constexpr size_t kSackBytes = Link::kReliableWindow / 8;
constexpr size_t kWindowMask = Link::kReliableWindow - 1;
static_assert((Link::kReliableWindow & kWindowMask) == 0, "the window indexes a ring by id");
static_assert(4 + kSackBytes + 2 + 2 + kPacketHeaderBytes + 6 + Link::kMaxPayload <= kMaxDatagram,
              "a full record, the header and a selective ack fit one datagram");

uint32_t randomNonce() {
    // Per thread: the plugin's built-in server runs its links on a thread of its own.
    static thread_local std::mt19937 rng{std::random_device{}()};
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
    // The server's cookie is kept: it is tied to our address, not to the session.
    config_.local.nonce = randomNonce();
    remote_ = config_.listen ? Endpoint{} : config_.remote;
    lastReceiveMs_ = nowMs;
    helloSent_ = false;
}

void Link::resetSession() {
    outUnreliable_.clear();
    pongDue_ = false;
    outReliable_.clear();
    outReliableBytes_ = sackedCount_ = 0;
    haveSacked_ = false;
    for (Slot& s : rx_) s.have = false;
    rxBuffered_ = 0;
    inbox_.clear();
    inboxBytes_.clear();
    nextReliableId_ = 0;
    expectedReliableId_ = 0;
    ackDirty_ = false;
    pingMs_ = -1;
    srttMs_ = -1;
    rttVarMs_ = 0;
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

// ---------------------------------------------------------------- queuing

std::vector<uint8_t> Link::takeSpare() {
    if (spare_.empty()) return {};
    std::vector<uint8_t> v = std::move(spare_.back());
    spare_.pop_back();
    v.clear();
    return v;
}

void Link::queueReliable(Msg type, std::vector<uint8_t>&& payload) {
    size_t size = payload.size();
    if (state_ != State::Connected || size > kMaxPayload) return;
    // Peer is gone (or never reads): the timeout will handle it; do not grow without bound meanwhile.
    if (outReliable_.size() >= kMaxQueuedReliable || outReliableBytes_ + size > kMaxQueuedReliableBytes) return;
    Pending p;
    p.id = nextReliableId_++;
    p.type = type;
    p.payload = std::move(payload);
    outReliable_.push_back(std::move(p));
    outReliableBytes_ += size;
}

void Link::beginUnreliable(ByteWriter& w, Msg type) {
    w.data.swap(outUnreliable_);
    unreliableMark_ = w.data.size();
    const uint8_t header[4] = {static_cast<uint8_t>(type), 0, 0, 0};  // type, flags, length (filled in later)
    w.bytes(header, sizeof(header));
}

void Link::endUnreliable(ByteWriter& w) {
    size_t length = w.data.size() - unreliableMark_ - 4;
    if (length > kMaxPayload || w.data.size() > kMaxUnreliableBytes) {
        w.data.resize(unreliableMark_);  // too big for a datagram, or far more than a flush can carry
    } else {
        uint16_t l = static_cast<uint16_t>(length);
        std::memcpy(w.data.data() + unreliableMark_ + 2, &l, 2);
    }
    w.data.swap(outUnreliable_);
}

void Link::send(Msg type, const uint8_t* data, size_t size, bool reliable) {
    if (state_ != State::Connected || size > kMaxPayload) return;
    if (reliable) {
        std::vector<uint8_t> v = takeSpare();
        v.assign(data, data + size);
        queueReliable(type, std::move(v));
    } else {
        ByteWriter w;
        beginUnreliable(w, type);
        w.bytes(data, size);
        endUnreliable(w);
    }
}

void Link::send(Msg type, std::vector<uint8_t>&& payload, bool reliable) {
    if (reliable) queueReliable(type, std::move(payload));
    else send(type, payload.data(), payload.size(), false);
}

void Link::sendFrom(uint8_t source, Msg type, const std::vector<uint8_t>& payload, bool reliable) {
    if (state_ != State::Connected || payload.size() + 1 > kMaxPayload) return;
    if (reliable) {
        std::vector<uint8_t> v = takeSpare();
        v.reserve(payload.size() + 1);
        v.push_back(source);
        v.insert(v.end(), payload.begin(), payload.end());
        queueReliable(type, std::move(v));
    } else {
        ByteWriter w;
        beginUnreliable(w, type);
        w.u8(source);
        w.bytes(payload.data(), payload.size());
        endUnreliable(w);
    }
}

bool Link::poll(Msg& type, std::vector<uint8_t>& payload) {
    if (inbox_.empty()) return false;
    const Inbound& in = inbox_.front();
    type = in.type;
    const uint8_t* body = inboxBytes_.data() + in.offset;
    payload.assign(body, body + in.size);
    inbox_.pop_front();
    if (inbox_.empty()) inboxBytes_.clear();  // keeps its capacity for the next batch
    return true;
}

bool Link::deliver(Msg type, const uint8_t* body, size_t size) {
    if (inbox_.size() >= kMaxInbox || inboxBytes_.size() + size > kMaxInboxBytes) return false;
    inbox_.push_back(Inbound{type, static_cast<uint32_t>(inboxBytes_.size()), static_cast<uint32_t>(size)});
    inboxBytes_.insert(inboxBytes_.end(), body, body + size);
    return true;
}

// ---------------------------------------------------------------- receiving

void Link::sendHello(const Endpoint& to, uint32_t nowMs) {
    if (!to.valid()) return;
    ByteWriter w;
    writePacketHeader(w, PacketType::Hello);
    write(w, config_.local);
    transmit(to, w.data.data(), w.data.size());
    lastHelloMs_ = nowMs;
    helloSent_ = true;
}

void Link::update(uint32_t nowMs) {
    if (!socket_ || state_ == State::Idle || state_ == State::Failed) return;
    updateRates(nowMs);

    uint8_t buffer[2048];
    Endpoint from;
    for (int n = 0; n < kMaxDatagramsPerUpdate; ++n) {
        int size = socket_->recvFrom(from, buffer, sizeof(buffer));
        if (size < 0) break;
        bytesIn_ += static_cast<uint32_t>(size);
        handleDatagram(from, buffer, static_cast<size_t>(size), nowMs);
        if (state_ == State::Failed) return;
    }
    drainReliable();  // the caller may have made room in the inbox since the records came in

    if (state_ == State::Connecting) {
        if (!helloSent_ || nowMs - lastHelloMs_ >= kHelloIntervalMs) sendHello(remote_, nowMs);
    } else if (state_ == State::Connected && nowMs - lastReceiveMs_ > kTimeoutMs) {
        beginConnecting(nowMs);  // lost the peer: fall back to waiting / reconnecting
    }
}

void Link::receive(const Endpoint& from, const uint8_t* data, size_t size, uint32_t nowMs) {
    if (!socket_ || state_ == State::Idle || state_ == State::Failed) return;
    bytesIn_ += static_cast<uint32_t>(size);
    handleDatagram(from, data, size, nowMs);
}

void Link::handleDatagram(const Endpoint& from, const uint8_t* data, size_t size, uint32_t nowMs) {
    ByteReader r(data, size);
    PacketType type;
    bool versionOk = false;
    if (!readPacketHeader(r, type, versionOk)) return;

    // Until a direct host has locked on to its friend it accepts a Hello from anywhere.
    bool acceptAnySource = config_.listen && state_ == State::Connecting;
    if (!acceptAnySource && from != remote_) return;

    if (!versionOk) {
        if (type == PacketType::Hello || type == PacketType::Challenge) {
            const char* why = "mod version mismatch: players and server need the same version";
            // A listening side keeps listening: a stranger must not be able to shut it down.
            if (config_.listen) error_ = why;
            else fail(why);
        }
        return;
    }

    switch (type) {
        case PacketType::Hello:
            handleHello(from, r, nowMs);
            break;
        case PacketType::Challenge:
            if (!config_.listen && state_ == State::Connecting) handleChallenge(r, nowMs);
            break;
        case PacketType::Data:
            if (state_ == State::Connected) handleData(r, nowMs);
            break;
        case PacketType::Bye:
            if (state_ == State::Connected && r.u16() == sessionKey() && r.ok()) beginConnecting(nowMs);
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
        if (config_.listen) error_ = why;
        else fail(why);
        return;
    }
    bool newSession = state_ != State::Connected || hello.nonce != peer_.nonce;
    if (newSession && !config_.listen && state_ == State::Connected &&
        std::find(pastPeerNonces_.begin(), pastPeerNonces_.end(), hello.nonce) != pastPeerNonces_.end())
        return;  // a replay of a session the server has already left behind
    if (newSession) {
        if (config_.listen) {
            // A fresh nonce for every session we accept, so the peer always notices that this side
            // started over. Otherwise a repeated (or forged) Hello could restart our numbering while
            // the peer keeps its own: same session key on both sides, reliable ids that never match.
            config_.local.nonce = randomNonce();
        } else if (state_ == State::Connected) {
            pastPeerNonces_[pastPeerIndex_++ % pastPeerNonces_.size()] = peer_.nonce;
        }
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

void Link::handleChallenge(ByteReader& r, uint32_t nowMs) {
    uint32_t cookie = r.u32();
    if (!r.ok() || cookie == 0) return;
    bool changed = cookie != config_.local.cookie;
    config_.local.cookie = cookie;
    if (changed || nowMs - lastHelloMs_ >= kHelloIntervalMs / 4) sendHello(remote_, nowMs);
}

void Link::handleData(ByteReader& r, uint32_t nowMs) {
    uint16_t key = r.u16();
    uint16_t ack = r.u16();
    if (!r.ok() || key != sessionKey()) return;
    lastReceiveMs_ = nowMs;

    // An ack for records we never queued is nonsense: ignore it rather than drop real ones.
    bool ackValid = !seqBefore(nextReliableId_, ack);
    if (ackValid) acknowledge(ack, nowMs);

    while (r.remaining() > 0) {
        uint8_t rawType = r.u8();
        uint8_t flags = r.u8();
        uint16_t reliableId = (flags & kRecordReliable) ? r.u16() : 0;
        uint16_t length = r.u16();
        const uint8_t* body = r.take(length);
        if (!r.ok()) break;

        Msg type = static_cast<Msg>(rawType);
        if (flags & kRecordSack) {
            if (ackValid) selectiveAck(ack, body, length, nowMs);
        } else if (flags & kRecordReliable) {
            ackDirty_ = true;
            receiveReliable(reliableId, type, body, length);
        } else if (type == Msg::Ping) {
            // Only the latest one is answered: a burst of pings cannot pile up answers.
            if (length <= pong_.size()) {
                std::memcpy(pong_.data(), body, length);
                pongSize_ = static_cast<uint8_t>(length);
                pongDue_ = true;
            }
        } else if (type == Msg::Pong) {
            ByteReader pr(body, length);
            uint32_t sentAt = pr.u32();
            if (pr.ok() && nowMs - sentAt < 60000) {
                pingMs_ = static_cast<int>(nowMs - sentAt);
                // Pings are never resent, so they measure the round trip even when every reliable
                // record is being resent (a path slower than the first guess, where Karn gives nothing).
                rttSample(pingMs_);
            }
        } else {
            deliver(type, body, length);  // a full inbox drops state updates; newer ones follow
        }
    }
    drainReliable();
}

void Link::rttSample(int sampleMs) {
    if (sampleMs < 0 || sampleMs >= 60000) return;
    if (srttMs_ < 0) {
        srttMs_ = sampleMs;
        rttVarMs_ = sampleMs / 2;
    } else {
        int err = sampleMs - srttMs_;
        rttVarMs_ += (std::abs(err) - rttVarMs_) / 4;
        srttMs_ += err / 8;
    }
}

void Link::ackRecord(Pending& p, uint32_t nowMs) {
    // Karn: only a record sent once tells how long a round trip takes.
    if (p.sends == 1) rttSample(static_cast<int>(nowMs - p.lastSentMs));
    outReliableBytes_ -= p.payload.size();
    if (spare_.size() < kSpareBuffers && p.payload.capacity() <= 2048) spare_.push_back(std::move(p.payload));
    else std::vector<uint8_t>().swap(p.payload);
}

void Link::acknowledge(uint16_t ack, uint32_t nowMs) {
    while (!outReliable_.empty() && seqBefore(outReliable_.front().id, ack)) {
        Pending& p = outReliable_.front();
        if (p.acked) --sackedCount_;
        else ackRecord(p, nowMs);
        outReliable_.pop_front();
    }
    if (haveSacked_ && (outReliable_.empty() || !seqBefore(outReliable_.front().id, highestSacked_)))
        haveSacked_ = false;
}

void Link::selectiveAck(uint16_t ack, const uint8_t* bits, size_t size, uint32_t nowMs) {
    if (size != kSackBytes || outReliable_.empty()) return;
    const uint16_t front = outReliable_.front().id;
    for (size_t i = 0; i < kReliableWindow - 1; ++i) {
        if (!(bits[i / 8] >> (i % 8) & 1)) continue;
        uint16_t id = static_cast<uint16_t>(ack + 1 + i);
        uint16_t index = static_cast<uint16_t>(id - front);
        if (index >= outReliable_.size()) continue;
        Pending& p = outReliable_[index];
        if (p.acked || p.sends == 0) continue;
        p.acked = true;
        ++sackedCount_;
        ackRecord(p, nowMs);
        if (!haveSacked_ || seqBefore(highestSacked_, id)) highestSacked_ = id;
        haveSacked_ = true;
    }
}

void Link::receiveReliable(uint16_t id, Msg type, const uint8_t* body, size_t size) {
    uint16_t offset = static_cast<uint16_t>(id - expectedReliableId_);
    if (offset >= kReliableWindow) return;  // a duplicate, or beyond what the sender may have in flight
    Slot& s = rx_[id & kWindowMask];
    if (s.have) return;
    s.have = true;
    s.type = type;
    s.payload.assign(body, body + size);
    ++rxBuffered_;
}

void Link::drainReliable() {
    while (rxBuffered_ > 0) {
        Slot& s = rx_[expectedReliableId_ & kWindowMask];
        // A full inbox leaves the record here unacknowledged: it is not lost, only delayed.
        if (!s.have || !deliver(s.type, s.payload.data(), s.payload.size())) break;
        s.have = false;
        --rxBuffered_;
        ++expectedReliableId_;
    }
}

// ---------------------------------------------------------------- sending

uint32_t Link::resendTimeout(const Pending& p, uint32_t nowMs) const {
    uint32_t rto = kInitialRtoMs;
    if (srttMs_ >= 0) {
        rto = static_cast<uint32_t>(srttMs_) + std::max<uint32_t>(4u * static_cast<uint32_t>(rttVarMs_), kRtoSlackMs);
        rto = std::clamp(rto, kMinRtoMs, kMaxRtoMs);
        // Records sent after this one have arrived: it is most likely lost, do not wait for the variance.
        if (haveSacked_ && seqBefore(p.id, highestSacked_)) {
            uint32_t srtt = static_cast<uint32_t>(srttMs_);
            rto = std::min(rto, std::max(kMinRtoMs, srtt + srtt / 4 + 5));
        }
    }
    // Back off only while the peer is silent (an outage). While its packets keep coming the path
    // works and a loss is just a loss: waiting longer for each repeat would only stall the channel.
    if (nowMs - lastReceiveMs_ <= rto) return rto;
    uint32_t shift = std::min<uint32_t>(p.sends > 0 ? p.sends - 1u : 0u, 4u);
    return std::min(rto << shift, kMaxBackoffMs);
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
        ByteWriter w;
        beginUnreliable(w, Msg::Ping);
        w.u32(nowMs);
        endUnreliable(w);
        lastPingMs_ = nowMs;
    }

    // Which records after the first missing one have arrived (only while there is a gap).
    uint8_t sack[kSackBytes] = {};
    bool haveSack = false;
    if (rxBuffered_ > 0) {
        for (size_t i = 0; i < kReliableWindow - 1; ++i) {
            uint16_t id = static_cast<uint16_t>(expectedReliableId_ + 1 + i);
            if (rx_[id & kWindowMask].have) {
                sack[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
                haveSack = true;
            }
        }
    }

    ByteWriter& w = packet_;
    size_t records = 0;
    auto begin = [&] {
        w.data.clear();
        writePacketHeader(w, PacketType::Data);
        w.u16(sessionKey());
        w.u16(expectedReliableId_);
        if (haveSack) {
            w.u8(0);
            w.u8(kRecordSack);
            w.u16(static_cast<uint16_t>(kSackBytes));
            w.bytes(sack, kSackBytes);
        }
        records = 0;
    };
    auto sendPacket = [&] {
        transmit(remote_, w.data.data(), w.data.size());
        lastSendMs_ = nowMs;
        ackDirty_ = false;
    };
    auto room = [&](size_t need) {
        if (records > 0 && w.data.size() + need > kMaxDatagram) {
            sendPacket();
            begin();
        }
        ++records;
    };

    begin();
    size_t window = 0;
    for (auto& p : outReliable_) {
        if (window++ >= kReliableWindow) break;
        if (p.acked) continue;
        if (p.sends > 0) {
            if (nowMs - p.lastSentMs < resendTimeout(p, nowMs)) continue;
            ++resends_;
        }
        room(p.payload.size() + 6);
        w.u8(static_cast<uint8_t>(p.type));
        w.u8(kRecordReliable);
        w.u16(p.id);
        w.u16(static_cast<uint16_t>(p.payload.size()));
        w.bytes(p.payload.data(), p.payload.size());
        if (p.sends < 255) ++p.sends;
        p.lastSentMs = nowMs;
    }
    if (pongDue_) {
        room(pongSize_ + 4u);
        w.u8(static_cast<uint8_t>(Msg::Pong));
        w.u8(0);
        w.u16(pongSize_);
        w.bytes(pong_.data(), pongSize_);
        pongDue_ = false;
    }
    // Unreliable records are framed already: copy as many as fit the datagram in one go.
    const uint8_t* staged = outUnreliable_.data();
    for (size_t at = 0, end = outUnreliable_.size(); at < end;) {
        size_t run = at, count = 0;
        while (run + 4 <= end) {
            uint16_t length;
            std::memcpy(&length, staged + run + 2, 2);
            size_t size = 4u + length;
            if (records + count > 0 && w.data.size() + (run - at) + size > kMaxDatagram) break;
            run += size;
            ++count;
        }
        w.bytes(staged + at, run - at);
        records += count;
        at = run;
        if (at < end) {
            sendPacket();
            begin();
        }
    }
    outUnreliable_.clear();

    if (records > 0 || ackDirty_ || nowMs - lastSendMs_ >= kKeepAliveMs) sendPacket();
}

}  // namespace coop
