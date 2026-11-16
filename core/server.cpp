#include "server.h"

#include "characters.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <random>
#include <set>

namespace coop {

namespace {
constexpr size_t kMaxClients = 512;
constexpr uint16_t kMaxClientsPerIp = 16;    // a LAN party behind one address still fits two campaigns
constexpr int kMaxDatagramsPerPump = 4096;   // a flood must not keep pump() from serving the players
constexpr uint32_t kCookieWindowMs = 30000;  // a Challenge cookie stays good for 30-60 s
constexpr size_t kMaxCachedEntities = 4096;  // world copy per campaign (a session streams far fewer)
constexpr size_t kMaxUploadBytes = 256u << 20;  // all save uploads in progress together
constexpr uint32_t kMaxAuthFailures = 10;    // wrong passwords per address and minute
constexpr uint32_t kAuthWindowMs = 60000;
// Per player and kind of message; real play stays far below all of them. Nothing a player sends
// reliably that has a lasting effect is dropped for being over a budget:
//  * damage: one message per hit, or per frame of damage over time, routed to one player only.
//    Room for ten targets hurt every frame at 60 fps (clients also add up the damage per target).
//    Over the budget it is dropped: a hit that is not passed on is lost, but nothing else is;
constexpr float kDamagePerSecond = 600, kDamageBurst = 1200;
//  * story events and save requests are rare by construction and each one matters (a death, a
//    mission change, a download). Over the budget the sender's messages are read later: it is
//    slowed down, nothing is lost;
constexpr float kStoryPerSecond = 20, kStoryBurst = 60;
// Changing character respawns that player's character in every game: a few in a row, then one every
// two seconds at most.
constexpr float kCharacterPerSecond = 0.5f, kCharacterBurst = 3;
//  * waypoints and appearance go to everyone, and only the latest one counts: over the budget the
//    latest waits and goes out when the budget allows;
constexpr float kSharedPerSecond = 10, kSharedBurst = 20;
//  * bytes of everything that gets passed on.
constexpr float kBytesPerSecond = 256 * 1024, kBytesBurst = 512 * 1024;
// A Hello from the address of a player in the campaign, with that player's name, on another port:
// the same player again (its game restarted the mod on loading a save, or a NAT changed the port),
// if the old connection has gone quiet. Two players with the same name behind one address keep
// talking, so the newcomer waits a moment and then joins next to the other one.
constexpr uint32_t kTakeoverSilenceMs = 1000;
constexpr uint32_t kTakeoverWaitMs = 2000;
// Byes for Data that has no session here: all senders together (each one smaller than the Data).
constexpr float kResetsPerSecond = 100, kResetsBurst = 200;
constexpr uint32_t kQuietMs = 15000;         // no resets for an address just turned away (its own timeout is 10 s)
constexpr size_t kMaxQuiet = 4096;
constexpr uint32_t kForgetMs = 20000;        // a client that is gone this long is dropped
constexpr uint32_t kDeniedLingerMs = 1500;   // time for the "denied" message to get out
constexpr uint32_t kStoryTickMs = 50;
constexpr uint32_t kPersistMs = 30000;
constexpr uint32_t kJoinWaitMs = 45000;
constexpr uint32_t kSettleMs = 1500;
constexpr uint32_t kFailGuardMs = 3000;      // race guard: a death right after a shared failure is not passed on
constexpr size_t kDownloadWindow = 64;

// Length of the valid UTF-8 sequence at s[i], or 0 (overlong forms, surrogates and stray bytes too).
size_t utf8Sequence(const std::string& s, size_t i) {
    auto at = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    unsigned char c = at(i);
    if (c < 0x80) return 1;
    size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
    if (!n || c > 0xF4 || i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k)
        if ((at(i + k) & 0xC0) != 0x80) return 0;
    unsigned char c1 = at(i + 1);
    if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 > 0x9F) || (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 > 0x8F))
        return 0;
    return n;
}

// Names end up in the HUD, the journal and players.ini: printable, valid UTF-8, no '=', at most 24
// bytes (never cut inside a character), no spaces around.
std::string cleanName(const std::string& raw, size_t maxBytes = 24) {
    std::string out;
    for (size_t i = 0; i < raw.size();) {
        size_t n = utf8Sequence(raw, i);
        if (!n) {
            ++i;
            continue;
        }
        unsigned char c = static_cast<unsigned char>(raw[i]);
        bool c1Control = c == 0xC2 && static_cast<unsigned char>(raw[i + 1]) < 0xA0;  // U+0080..U+009F
        bool keep = (n > 1 && !c1Control) || (c > 32 && c != 127 && c != '=') || (c == ' ' && !out.empty());
        if (keep) {
            if (out.size() + n > maxBytes) break;
            out.append(raw, i, n);
        }
        i += n;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? "Player" : out;
}

// Avatars are ped model names.
std::string cleanAvatar(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_') out += c;
        if (out.size() >= 48) break;
    }
    return out;
}

// SipHash-2-4 of two words: Challenge cookies must not be predictable for another address.
uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
uint64_t siphash(const uint64_t key[2], uint64_t m0, uint64_t m1) {
    uint64_t v0 = key[0] ^ 0x736f6d6570736575ull, v1 = key[1] ^ 0x646f72616e646f6dull;
    uint64_t v2 = key[0] ^ 0x6c7967656e657261ull, v3 = key[1] ^ 0x7465646279746573ull;
    auto round = [&] {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    };
    for (uint64_t m : {m0, m1, uint64_t(16) << 56}) {
        v3 ^= m;
        round();
        round();
        v0 ^= m;
    }
    v2 ^= 0xff;
    for (int i = 0; i < 4; ++i) round();
    return v0 ^ v1 ^ v2 ^ v3;
}

}  // namespace

bool Server::Budget::take(float cost, float perSecond, float burst, uint32_t nowMs) {
    if (tokens < 0) tokens = burst;
    else tokens = std::min(burst, tokens + static_cast<float>(nowMs - lastMs) * perSecond / 1000.0f);
    lastMs = nowMs;
    if (tokens < cost) return false;
    tokens -= cost;
    return true;
}

// The real socket, seen by one client's link: sends go to that client. Its datagrams are handed to
// the link directly (Link::receive), so nothing is queued or copied per datagram.
class Server::ClientSocket final : public IDatagramSocket {
public:
    ClientSocket(IDatagramSocket& real, Endpoint ep) : real_(real), ep_(ep) {}
    bool sendTo(const Endpoint&, const uint8_t* data, size_t size) override { return real_.sendTo(ep_, data, size); }
    int recvFrom(Endpoint&, uint8_t*, size_t) override { return -1; }

private:
    IDatagramSocket& real_;
    Endpoint ep_;
};

struct Server::Client {
    Endpoint ep;
    std::unique_ptr<ClientSocket> socket;
    Link link;
    uint32_t linkEpoch = 0;
    uint32_t createdMs = 0, lostSinceMs = 0, deniedAtMs = 0;
    uint32_t lastHeardMs = 0;       // the last datagram from this address
    uint32_t admitWaitSinceMs = 0;  // connected, waiting to see whether it takes over an old connection
    bool admitted = false;
    bool replaced = false;          // a new connection of the same player took this one's place
    uint8_t id = 0;
    std::string campaign, name, avatar;
    std::string baseName;           // the name asked for, cleaned (`name` may have a " 2" added)
    bool wantsWorld = false;
    uint32_t joinedMs = 0;
    // latest state, for the story logic and for players who join later
    PlayerState state;
    bool haveState = false;
    std::vector<uint8_t> stateBytes, appearanceBytes;
    // story
    uint32_t missionScript = 0;
    uint16_t prevFlags = 0;
    bool holding = false, mismatch = false;
    uint32_t holdStartMs = 0, readySinceMs = 0, failGuardUntil = 0;
    // storyTick's own bookkeeping: the connection it last saw (a new one = a fresh player), when the
    // state last changed, since when the mission differs from the owner's, held at its last pass
    uint32_t storyLink = 0, storyStateTime = 0, storyFreshMs = 0, mismatchSinceMs = 0;
    bool storyHolding = false;
    // save transfer: an upload grows as its pieces arrive, a download shares the campaign's copy
    std::vector<uint8_t> upload;
    std::shared_ptr<const std::vector<uint8_t>> download;
    size_t uploadTotal = 0, downloadOffset = 0;
    uint32_t uploadCrc = 0, downloadCrc = 0;
    bool uploading = false, downloading = false;
    // limits, and the latest waypoint / appearance waiting for the budget
    Budget damage, story, shared, bytes, character;
    std::vector<uint8_t> laterWaypoint, laterAppearance;
    bool haveLaterWaypoint = false, haveLaterAppearance = false;
};

struct Server::Campaign {
    std::string name;
    std::vector<uint8_t> ids;           // members, in joining order
    uint8_t owner = 0;
    uint16_t epoch = 0;
    std::unordered_map<uint16_t, EntityState> cache;  // latest world state (fields = what is known)
    uint32_t cacheTimeMs = 0;
    std::vector<uint8_t> worldInfo, waypoint;
    uint8_t waypointFrom = 0;
    bool hasSave = false;
    std::shared_ptr<const std::vector<uint8_t>> save;  // the latest campaign save, read once
    uint32_t saveCrc = 0;
    uint32_t saveTime = 0;  // when it was stored, Unix seconds (0: not known)
    uint32_t ownerMissionStartMs = 0;
    bool ownerInMission = false;
    std::map<uint8_t, Client*> byId;
};

Server::Server(IDatagramSocket& socket, const ServerConfig& config, IServerStorage& storage)
    : socket_(socket), config_(config), storage_(storage) {
    std::random_device rd;
    for (uint64_t& k : cookieKey_) k = (uint64_t(rd()) << 32) ^ rd();
}
Server::~Server() = default;

ServerStats Server::stats() const {
    ServerStats s = stats_;
    s.uploadBytes = uploadBytes();
    return s;
}

size_t Server::uploadBytes() const {
    size_t n = 0;
    for (const auto& [ep, c] : clients_) n += c->upload.size();
    return n;
}

void Server::shutdown(uint32_t nowMs) {
    for (auto& [ep, c] : clients_) {
        if (c->admitted) {
            if (c->haveState) storage_.storePlayer(c->campaign, c->name, c->state.pos);
            storage_.journal(c->campaign, c->name + " left (server stopped)");
        }
        c->link.stop();  // a Bye: the player starts reconnecting at once instead of timing out
    }
    clients_.clear();
    clientsPerIp_.clear();
    campaigns_.clear();
    ++revision_;
    (void)nowMs;
}

size_t Server::playerCount() const {
    size_t n = 0;
    for (const auto& [ep, c] : clients_) n += c->admitted;
    return n;
}

std::vector<std::string> Server::describe() const {
    std::vector<std::string> out;
    for (const auto& [name, camp] : campaigns_) {
        std::string line = "campaign \"" + name + "\": ";
        for (size_t i = 0; i < camp->ids.size(); ++i) {
            auto it = camp->byId.find(camp->ids[i]);
            if (it == camp->byId.end()) continue;
            if (i) line += ", ";
            line += it->second->name;
            if (camp->ids[i] == camp->owner) line += " (world)";
        }
        out.push_back(line);
    }
    return out;
}

Server::Client* Server::member(Campaign& camp, uint8_t id) {
    auto it = camp.byId.find(id);
    return it == camp.byId.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------- the loop

void Server::pump(uint32_t nowMs) {
    uint8_t buffer[2048];
    Endpoint from;
    for (int n = 0; n < kMaxDatagramsPerPump; ++n) {
        int size = socket_.recvFrom(from, buffer, sizeof(buffer));
        if (size < 0) break;
        ++stats_.datagrams;
        dispatch(from, buffer, static_cast<size_t>(size), nowMs);
    }

    for (auto it = clients_.begin(); it != clients_.end();) {
        Client& c = *it->second;
        c.link.update(nowMs);
        if (c.link.connected()) {
            c.lostSinceMs = 0;
            if (c.link.epoch() != c.linkEpoch) {  // (re)connected: a fresh session for this player
                // The same player starting over from the same address (its mod restarted, or the
                // connection blipped) keeps its place, like one on a new port; anyone else rejoins.
                if (c.admitted && !sameSeat(c, nowMs)) leave(c, nowMs, "rejoined");
                c.linkEpoch = c.link.epoch();
                c.admitWaitSinceMs = 0;
                admit(c, nowMs);
            } else if (c.admitWaitSinceMs) {
                admit(c, nowMs);  // is it a player's new connection? (decided within kTakeoverWaitMs)
            }
            Msg type;
            size_t size = 0;
            if (c.admitted) {
                sendLater(c, nowMs);
                while (c.link.peek(type, size)) {
                    Verdict v = allow(c, type, size, nowMs);
                    if (v == Verdict::Wait) break;  // stays in the inbox: read on a later pump
                    c.link.poll(type, message_);
                    if (v == Verdict::Handle) handle(c, type, message_, nowMs);
                    else if (v == Verdict::Later) keepForLater(c, type, message_);
                    else ++stats_.rateLimited;
                }
                sendLater(c, nowMs);
            } else if (!c.admitWaitSinceMs) {
                while (c.link.poll(type, message_)) {}  // not in a campaign: nothing to do with it
            }
            // (What a waiting newcomer sends stays in its inbox and is handled once it is in.)
            if (c.downloading) pumpDownload(c);
        } else {
            if (c.admitted) leave(c, nowMs, "left");
            if (!c.lostSinceMs) c.lostSinceMs = nowMs ? nowMs : 1;
        }
        bool deniedDone = c.deniedAtMs && nowMs - c.deniedAtMs > kDeniedLingerMs;
        bool gone = c.lostSinceMs && nowMs - c.lostSinceMs > kForgetMs;
        bool neverJoined = !c.link.connected() && !c.linkEpoch && nowMs - c.createdMs > kForgetMs;
        if (deniedDone || gone || neverJoined || c.replaced) {
            if (c.admitted) leave(c, nowMs, "left");
            if (deniedDone && quiet_.size() < kMaxQuiet) quiet_[c.ep] = nowMs;
            auto ip = clientsPerIp_.find(c.ep.ip);
            if (ip != clientsPerIp_.end() && --ip->second == 0) clientsPerIp_.erase(ip);
            it = clients_.erase(it);
            continue;
        }
        ++it;
    }

    if (nowMs - lastStoryMs_ >= kStoryTickMs) {
        lastStoryMs_ = nowMs;
        for (auto& [name, camp] : campaigns_) storyTick(*camp, nowMs);
    }
    if (nowMs - lastPersistMs_ >= kPersistMs) {
        lastPersistMs_ = nowMs;
        for (auto f = authFailures_.begin(); f != authFailures_.end();)
            f = nowMs - f->second.sinceMs > kAuthWindowMs ? authFailures_.erase(f) : std::next(f);
        for (auto q = quiet_.begin(); q != quiet_.end();)
            q = nowMs - q->second > kQuietMs ? quiet_.erase(q) : std::next(q);
        for (auto& [ep, c] : clients_)
            if (c->admitted && c->haveState) storage_.storePlayer(c->campaign, c->name, c->state.pos);
    }
    for (auto& [ep, c] : clients_) c->link.flush(nowMs);
}

uint32_t Server::cookie(const Endpoint& ep, uint32_t window) const {
    uint32_t c = static_cast<uint32_t>(siphash(cookieKey_, (uint64_t(ep.ip) << 16) | ep.port, window));
    return c ? c : 1;
}

void Server::dispatch(const Endpoint& from, const uint8_t* data, size_t size, uint32_t nowMs) {
    ByteReader r(data, size);
    PacketType type;
    bool versionOk = false;
    if (size > kMaxDatagram + 64 || !readPacketHeader(r, type, versionOk)) {
        ++stats_.dropped;
        return;
    }
    auto it = clients_.find(from);
    if (type == PacketType::Hello) {
        // Nothing here may answer with more bytes than it got: the sender address is not proven yet.
        if (!versionOk) {
            // Our own Hello, so the player's mod shows "version mismatch" (older mods understand it too).
            ByteWriter w;
            writePacketHeader(w, PacketType::Hello);
            write(w, HelloInfo{});
            if (w.data.size() <= size) socket_.sendTo(from, w.data.data(), w.data.size());
            return;
        }
        HelloInfo hello;
        if (!read(r, hello) || hello.role != Role::Guest) {
            ++stats_.dropped;
            return;
        }
        // Only a Hello that echoes a cookie for its own address opens (or restarts) a session; any
        // other gets the cookie, in a datagram smaller than the Hello. A forged sender address never
        // sees it, so it can neither take a slot here nor make the server send someone else more.
        uint32_t window = nowMs / kCookieWindowMs;
        if (hello.cookie != cookie(from, window) && hello.cookie != cookie(from, window - 1)) {
            ByteWriter w;
            writePacketHeader(w, PacketType::Challenge);
            w.u32(cookie(from, window));
            socket_.sendTo(from, w.data.data(), w.data.size());
            ++stats_.challenges;
            return;
        }
        if (it == clients_.end()) {
            auto ip = clientsPerIp_.find(from.ip);
            if (clients_.size() >= kMaxClients || (ip != clientsPerIp_.end() && ip->second >= kMaxClientsPerIp)) {
                ++stats_.dropped;
                return;
            }
            auto c = std::make_unique<Client>();
            c->ep = from;
            c->socket = std::make_unique<ClientSocket>(socket_, from);
            c->createdMs = nowMs;
            LinkConfig lc;
            lc.listen = true;
            lc.local.role = Role::Host;
            lc.local.name = "server";
            c->link.start(c->socket.get(), lc, nowMs);
            it = clients_.emplace(from, std::move(c)).first;
            ++clientsPerIp_[from.ip];
            quiet_.erase(from);
        }
    } else if (it == clients_.end()) {
        if (type == PacketType::Data && versionOk) answerStaleData(from, r, nowMs);
        ++stats_.dropped;  // random traffic costs nothing
        return;
    }
    it->second->lastHeardMs = nowMs;
    it->second->link.receive(from, data, size, nowMs);
}

void Server::answerStaleData(const Endpoint& from, ByteReader& r, uint32_t nowMs) {
    // Data from an address without a session: most likely a player whose session this server does
    // not have, because it restarted or forgot the player after a long silence. Without a word from
    // us that player waits for its own timeout, ten seconds of a frozen world: a Bye with the key it
    // uses makes it connect again at once. The Bye is smaller than the Data it answers, and one
    // budget covers all of them, so a forged sender address gains nothing.
    uint16_t key = r.u16();
    if (!r.ok()) return;
    auto q = quiet_.find(from);
    if (q != quiet_.end()) {
        if (nowMs - q->second < kQuietMs) return;
        quiet_.erase(q);
    }
    if (!resets_.take(1, kResetsPerSecond, kResetsBurst, nowMs)) return;
    ByteWriter w;
    writePacketHeader(w, PacketType::Bye);
    w.u16(key);
    socket_.sendTo(from, w.data.data(), w.data.size());
    ++stats_.resets;
}

Server::Verdict Server::allow(Client& c, Msg type, size_t size, uint32_t nowMs) {
    auto bytes = [&] { return c.bytes.take(static_cast<float>(size), kBytesPerSecond, kBytesBurst, nowMs); };
    switch (type) {
        case Msg::SaveChunk:
            // Save uploads are bounded by size, are not passed on, and should go as fast as the line allows.
            return Verdict::Handle;
        case Msg::CharacterRequest:
            if (c.character.take(1, kCharacterPerSecond, kCharacterBurst, nowMs)) return Verdict::Handle;
            notice(c, "Too many character changes - wait a moment");
            return Verdict::Drop;
        case Msg::Story:
        case Msg::SaveRequest:
            return c.story.take(1, kStoryPerSecond, kStoryBurst, nowMs) ? Verdict::Handle : Verdict::Wait;
        case Msg::Waypoint:
        case Msg::Appearance: {
            // One already waits: this newer one takes its place (and its turn), never overtakes it.
            bool waiting = type == Msg::Waypoint ? c.haveLaterWaypoint : c.haveLaterAppearance;
            if (waiting) return Verdict::Later;
            return c.shared.take(1, kSharedPerSecond, kSharedBurst, nowMs) && bytes() ? Verdict::Handle : Verdict::Later;
        }
        case Msg::Damage:
            return c.damage.take(1, kDamagePerSecond, kDamageBurst, nowMs) && bytes() ? Verdict::Handle : Verdict::Drop;
        default:
            // State streams: a newer state follows soon.
            return bytes() ? Verdict::Handle : Verdict::Drop;
    }
}

void Server::keepForLater(Client& c, Msg type, const std::vector<uint8_t>& payload) {
    bool& have = type == Msg::Waypoint ? c.haveLaterWaypoint : c.haveLaterAppearance;
    std::vector<uint8_t>& later = type == Msg::Waypoint ? c.laterWaypoint : c.laterAppearance;
    if (have) ++stats_.rateLimited;  // superseded: only the latest one counts
    later = payload;
    have = true;
}

void Server::sendLater(Client& c, uint32_t nowMs) {
    for (Msg type : {Msg::Waypoint, Msg::Appearance}) {
        bool& have = type == Msg::Waypoint ? c.haveLaterWaypoint : c.haveLaterAppearance;
        std::vector<uint8_t>& later = type == Msg::Waypoint ? c.laterWaypoint : c.laterAppearance;
        if (!have || !c.shared.take(1, kSharedPerSecond, kSharedBurst, nowMs) ||
            !c.bytes.take(static_cast<float>(later.size()), kBytesPerSecond, kBytesBurst, nowMs))
            continue;
        have = false;
        std::vector<uint8_t> payload;
        payload.swap(later);
        handle(c, type, payload, nowMs);
    }
}

bool Server::authBlocked(uint32_t ip, uint32_t nowMs) {
    auto it = authFailures_.find(ip);
    if (it == authFailures_.end()) return false;
    if (nowMs - it->second.sinceMs > kAuthWindowMs) {
        authFailures_.erase(it);
        return false;
    }
    return it->second.count >= kMaxAuthFailures;
}

void Server::authFailed(uint32_t ip, uint32_t nowMs) {
    if (authFailures_.size() >= 4096 && !authFailures_.count(ip)) return;
    AuthFailures& f = authFailures_[ip];
    if (!f.count || nowMs - f.sinceMs > kAuthWindowMs) f = AuthFailures{0, nowMs};
    ++f.count;
}

void Server::dropUpload(Client& c) {
    c.uploading = false;
    std::vector<uint8_t>().swap(c.upload);
}

void Server::refuseUpload(Client& c, uint32_t crc, const std::string& why) {
    // Said at once: the player must not take the acknowledged pieces for a stored save.
    dropUpload(c);
    SaveResult result;
    result.crc = crc;
    result.stored = 0;
    result.reason = why;
    c.link.send(Msg::SaveResult, result, true);
}

// ---------------------------------------------------------------- joining and leaving

bool Server::sameSeat(Client& c, uint32_t nowMs) {
    // A new session on the link of an admitted player: is it that player again? Then admit() keeps
    // its place, without a password check that could turn it away (it would have to leave first).
    const HelloInfo& hello = c.link.peer();
    std::string campaignName = sanitizeCampaign(hello.campaign.empty() ? std::string("default") : hello.campaign);
    bool passwordOk = config_.password.empty() || (hello.password == config_.password && !authBlocked(c.ep.ip, nowMs));
    return c.admitted && campaignName == c.campaign && cleanName(hello.name) == c.baseName && passwordOk;
}

void Server::admit(Client& c, uint32_t nowMs) {
    const HelloInfo& hello = c.link.peer();
    std::string campaignName = sanitizeCampaign(hello.campaign.empty() ? std::string("default") : hello.campaign);
    auto deny = [&](const std::string& reason) {
        c.link.send(Msg::Denied, Denied{reason}, true);
        c.deniedAtMs = nowMs ? nowMs : 1;
        c.admitWaitSinceMs = 0;
    };
    if (campaignName.empty()) return deny("invalid campaign name");
    if (!config_.password.empty()) {
        // Guessing is slow: a few wrong passwords per address and minute, then not even checked.
        if (authBlocked(c.ep.ip, nowMs)) return deny("too many wrong passwords - wait a minute");
        if (hello.password != config_.password) {
            authFailed(c.ep.ip, nowMs);
            return deny("wrong password");
        }
    }
    auto& slot = campaigns_[campaignName];
    if (!slot) {
        slot = std::make_unique<Campaign>();
        slot->name = campaignName;
        // Read once and kept: every download of this campaign shares the copy.
        std::vector<uint8_t> data;
        slot->hasSave = storage_.loadSave(campaignName, data);
        if (slot->hasSave && !data.empty() && data.size() <= kMaxSaveBytes) {
            slot->saveCrc = crc32(data.data(), data.size());
            slot->saveTime = static_cast<uint32_t>(storage_.loadedSaveTime(campaignName));
            slot->save = std::make_shared<const std::vector<uint8_t>>(std::move(data));
        } else if (storage_.hasSaveFiles(campaignName)) {
            // Saves are there but none can be read (a broken disk, a folder without permissions).
            // This is not a new campaign: said to have no save, the world owner's game would start
            // it over from whatever local save it has. It stays "has a save" (with nothing to
            // download) until someone who may replace it uploads one.
            slot->hasSave = true;
            slot->save.reset();
            slot->saveCrc = 0;
            storage_.journal(campaignName, "the campaign's save files could not be read");
        }
    }
    Campaign& camp = *slot;

    // The same player again on a new connection: its old one has gone quiet. It takes that place,
    // with the same id and name, and the world if it owned it; nobody sees it leave and come back.
    // (Still admitted: a new session from the same address, see sameSeat().)
    const std::string baseName = cleanName(hello.name);
    Client* twin = c.admitted ? &c : nullptr;
    for (uint8_t other : camp.ids) {
        Client* m = twin == &c ? nullptr : member(camp, other);
        if (!m || m == &c || m->ep.ip != c.ep.ip || m->ep == c.ep || m->baseName != baseName) continue;
        if (!twin || nowMs - m->lastHeardMs > nowMs - twin->lastHeardMs) twin = m;
    }
    if (twin && twin != &c && nowMs - twin->lastHeardMs < kTakeoverSilenceMs) {
        if (!c.admitWaitSinceMs) c.admitWaitSinceMs = nowMs ? nowMs : 1;
        if (nowMs - c.admitWaitSinceMs < kTakeoverWaitMs) return;  // pump() asks again
        twin = nullptr;  // still talking: another player behind the same address, with the same name
    }
    c.admitWaitSinceMs = 0;
    if (!twin && static_cast<int>(camp.ids.size()) >= std::min<int>(config_.maxPlayers, kMaxPlayers))
        return deny("campaign is full");

    uint8_t id = 1;
    std::string name;
    bool ownedWorld = false;
    if (twin) {
        id = twin->id;
        name = twin->name;
        ownedWorld = camp.owner == id;
        if (twin->haveState) storage_.storePlayer(camp.name, twin->name, twin->state.pos);
        if (twin != &c) {
            twin->admitted = false;
            twin->replaced = true;  // pump() forgets it
            dropUpload(*twin);
            twin->downloading = false;
            twin->download.reset();
            twin->link.stop();  // a Bye, should the old game still be listening after all
        }
        ++stats_.takeovers;
    } else {
        while (camp.byId.count(id)) ++id;
        name = baseName;
        for (int n = 2; std::any_of(camp.byId.begin(), camp.byId.end(), [&](auto& kv) { return kv.second->name == name; }); ++n) {
            std::string suffix = " " + std::to_string(n);
            name = cleanName(hello.name, 24 - suffix.size()) + suffix;  // still within the 24 bytes
        }
    }

    c.admitted = true;
    c.baseName = baseName;
    c.haveLaterWaypoint = c.haveLaterAppearance = false;
    c.laterWaypoint.clear();
    c.laterAppearance.clear();
    c.deniedAtMs = 0;  // an earlier attempt on this address may have been turned away
    c.id = id;
    c.campaign = campaignName;
    c.name = name;
    // Every character once per campaign: whoever asks for one that is taken gets the first free one.
    std::string wantedAvatar = normalizeAvatar(cleanAvatar(hello.avatar));
    if (wantedAvatar.empty()) wantedAvatar = characterCatalog().front().model;
    std::vector<std::string> takenAvatars;
    for (uint8_t other : camp.ids)
        if (Client* m = member(camp, other); m && m != &c && m != twin) takenAvatars.push_back(m->avatar);
    c.avatar = freeCharacter(wantedAvatar, takenAvatars);
    c.wantsWorld = hello.wantsWorld != 0;
    c.joinedMs = nowMs;
    c.haveState = false;
    c.stateBytes.clear();
    c.appearanceBytes.clear();
    c.missionScript = 0;
    c.prevFlags = 0;
    c.holding = c.mismatch = false;
    dropUpload(c);
    c.downloading = false;
    c.download.reset();
    if (!twin) camp.ids.push_back(id);  // a returning player keeps its place in the joining order
    camp.byId[id] = &c;
    ++revision_;

    Welcome w;
    w.playerId = id;
    w.storyMode = config_.storyMode;
    w.hasCampaignSave = camp.hasSave;
    w.campaignSaveCrc = camp.hasSave ? camp.saveCrc : 0;
    w.campaignSaveTime = camp.hasSave ? camp.saveTime : 0;
    w.campaign = campaignName;
    c.link.send(Msg::Welcome, w, true);
    storage_.journal(campaignName, name + (twin ? " reconnected" : " joined"));

    // The world goes to a player who asked for it, or to the first one in. A returning owner gets it
    // back, as a new world: its restarted game numbers its entities afresh, so nothing of the old
    // world (cached state, hits on its ids) may be taken for the new one.
    Client* owner = member(camp, camp.owner);
    if (ownedWorld) newWorld(camp, id);
    else if (!owner || (c.wantsWorld && !owner->wantsWorld)) electOwner(camp, nowMs, c.wantsWorld || !owner ? id : 0);
    sendRoster(camp);

    // Bring the newcomer up to date at once: other players, the world, the map.
    for (uint8_t other : camp.ids) {
        Client* o = member(camp, other);
        if (!o || o == &c) continue;
        if (!o->stateBytes.empty()) c.link.sendFrom(o->id, Msg::PlayerState, o->stateBytes, false);
        if (!o->appearanceBytes.empty()) c.link.sendFrom(o->id, Msg::Appearance, o->appearanceBytes, true);
    }
    if (camp.owner != id) sendWorldTo(camp, c);
    if (!camp.waypoint.empty()) c.link.sendFrom(camp.waypointFrom, Msg::Waypoint, camp.waypoint, true);
    if (c.avatar != wantedAvatar)
        notice(c, characterName(wantedAvatar) + " is taken - you are " + characterName(c.avatar) +
                      " (choose another one with the character key)");
    if (twin) return;
    for (uint8_t other : camp.ids)
        if (Client* o = member(camp, other); o && o != &c) notice(*o, name + " joined the campaign");
}

void Server::leave(Client& c, uint32_t nowMs, const char* why) {
    if (!c.admitted) return;
    c.admitted = false;
    dropUpload(c);
    c.downloading = false;
    c.download.reset();
    c.haveLaterWaypoint = c.haveLaterAppearance = false;
    ++revision_;
    auto cit = campaigns_.find(c.campaign);
    if (cit == campaigns_.end()) return;
    Campaign& camp = *cit->second;
    if (c.haveState) storage_.storePlayer(camp.name, c.name, c.state.pos);
    storage_.journal(camp.name, c.name + " " + why);
    camp.ids.erase(std::remove(camp.ids.begin(), camp.ids.end(), c.id), camp.ids.end());
    camp.byId.erase(c.id);
    if (camp.waypointFrom == c.id) camp.waypointFrom = 0;
    if (camp.ids.empty()) {
        campaigns_.erase(cit);  // everything that matters is on disk
        return;
    }
    if (camp.owner == c.id) electOwner(camp, nowMs, 0);
    sendRoster(camp);
    for (uint8_t id : camp.ids)
        if (Client* o = member(camp, id)) notice(*o, c.name + " left the campaign");
}

void Server::electOwner(Campaign& camp, uint32_t nowMs, uint8_t preferred) {
    uint8_t chosen = preferred && camp.byId.count(preferred) ? preferred : 0;
    if (!chosen)
        for (uint8_t id : camp.ids)
            if (member(camp, id)->wantsWorld) {
                chosen = id;
                break;
            }
    if (!chosen && !camp.ids.empty()) chosen = camp.ids.front();
    if (chosen == camp.owner) return;
    newWorld(camp, chosen);
    (void)nowMs;
}

void Server::newWorld(Campaign& camp, uint8_t owner) {
    camp.owner = owner;
    // 0 never names a world: it is what a sender without a roster puts into Damage.
    if (++camp.epoch == 0) camp.epoch = 1;
    ++revision_;
    // A new world: whatever was cached belongs to the old one.
    camp.cache.clear();
    camp.worldInfo.clear();
    camp.ownerInMission = false;
    for (uint8_t id : camp.ids) {
        Client* m = member(camp, id);
        if (!m) continue;
        if (m->holding) release(*m, "");
        // Only the world owner may replace the campaign save: an upload under way from someone
        // else is refused now, not silently at its next piece.
        if (m->uploading && m->id != owner && camp.hasSave)
            refuseUpload(*m, m->uploadCrc, "The world now belongs to another player - your save was not uploaded");
    }
    if (Client* o = member(camp, owner)) storage_.journal(camp.name, o->name + " now owns the world");
}

void Server::sendRoster(Campaign& camp) {
    Roster r;
    r.worldOwner = camp.owner;
    r.worldEpoch = camp.epoch;
    r.hasCampaignSave = camp.hasSave ? 1 : 0;
    r.campaignSaveCrc = camp.hasSave ? camp.saveCrc : 0;
    r.campaignSaveTime = camp.hasSave ? camp.saveTime : 0;
    for (uint8_t id : camp.ids)
        if (Client* m = member(camp, id)) r.players.push_back({m->id, m->name, m->avatar});
    std::vector<uint8_t> bytes = encode(r);
    for (uint8_t id : camp.ids)
        if (Client* m = member(camp, id)) m->link.send(Msg::Roster, bytes, true);
}

// ---------------------------------------------------------------- the world

void Server::mergeSnapshot(Campaign& camp, const EntitySnapshot& snap) {
    camp.cacheTimeMs = snap.timeMs;
    for (const EntityState& e : snap.entities) {
        auto it = camp.cache.find(e.id);
        if (it == camp.cache.end()) {
            if (camp.cache.size() >= kMaxCachedEntities) continue;
            it = camp.cache.emplace(e.id, EntityState{}).first;
        }
        EntityState& c = it->second;
        if ((e.fields & EF_Static) && (c.fields & EF_Static) && (c.model != e.model || c.kind != e.kind)) c = EntityState{};
        c.id = e.id;
        if (e.fields & EF_Static) {
            c.kind = e.kind;
            c.model = e.model;
            c.outfit = e.outfit;
            c.script = e.script;
        }
        if (e.fields & EF_Pos) c.pos = e.pos;
        if (e.fields & EF_Heading) c.heading = e.heading;
        if (e.fields & EF_Tilt) {
            c.pitch = e.pitch;
            c.roll = e.roll;
        }
        if (e.fields & EF_Vel) c.vel = e.vel;
        if (e.fields & EF_Health) c.health = e.health;
        if (e.fields & EF_State) {
            c.flags = e.flags;
            c.weapon = e.weapon;
            c.mountRef = e.mountRef;
            c.vehicleRef = e.vehicleRef;
            c.seat = e.seat;
            c.targetRef = e.targetRef;
        }
        c.fields |= e.fields;
    }
    for (uint16_t id : snap.removed) camp.cache.erase(id);
}

void Server::sendWorldTo(Campaign& camp, Client& c) {
    if (!camp.worldInfo.empty()) c.link.send(Msg::WorldInfo, camp.worldInfo, true);
    if (camp.cache.empty()) return;
    std::vector<EntityState> all;
    all.reserve(camp.cache.size());
    Vec3 anchor;
    for (const auto& [id, e] : camp.cache) {
        if (!(e.fields & EF_Static) || !(e.fields & EF_Pos)) continue;
        if (all.empty()) anchor = e.pos;
        all.push_back(e);
        all.back().fields &= static_cast<uint8_t>(~EF_FarPos);
    }
    std::vector<std::vector<uint8_t>> packets;
    encodeSnapshots(camp.cacheTimeMs, anchor, all, {}, kMaxMessage, packets);
    for (const auto& p : packets) c.link.send(Msg::EntitySnapshot, p, true);
}

void Server::forward(Campaign& camp, const Client& from, Msg type, const std::vector<uint8_t>& payload, bool reliable,
                     bool prefix) {
    // Written straight into each player's outgoing buffer: no copy of the message per player.
    for (uint8_t id : camp.ids) {
        Client* m = member(camp, id);
        if (!m || m == &from) continue;
        if (prefix) m->link.sendFrom(from.id, type, payload, reliable);
        else m->link.send(type, payload, reliable);
    }
}

// ---------------------------------------------------------------- messages

void Server::handle(Client& c, Msg type, const std::vector<uint8_t>& payload, uint32_t nowMs) {
    if (!c.admitted) return;
    auto cit = campaigns_.find(c.campaign);
    if (cit == campaigns_.end()) return;
    Campaign& camp = *cit->second;
    bool isOwner = camp.owner == c.id;

    switch (type) {
        case Msg::PlayerState: {
            PlayerState s;
            if (!decode(payload, s)) return;
            if (c.haveState && static_cast<int32_t>(s.timeMs - c.state.timeMs) <= 0) return;
            c.state = s;
            c.haveState = true;
            c.stateBytes = payload;
            forward(camp, c, type, payload, false, true);
            return;
        }
        case Msg::EntitySnapshot: {
            EntitySnapshot& snap = snapshot_;
            if (!isOwner || !decode(payload, snap)) return;  // only the world owner describes the world
            mergeSnapshot(camp, snap);
            forward(camp, c, type, payload, false, false);
            return;
        }
        case Msg::WorldInfo:
            if (!isOwner) return;
            camp.worldInfo = payload;
            forward(camp, c, type, payload, false, false);
            return;
        case Msg::Damage: {
            Damage d;
            if (!decode(payload, d)) return;
            // World entities are numbered afresh in every world: a hit sent before its sender heard
            // of a new world owner names an entity of the old world, and the new owner would apply
            // it to whatever has that id in its own. Epoch 0 names no world: the sender had no
            // roster yet, so it cannot have seen any of this world's entities.
            if (!isPlayerRef(d.target) && d.worldEpoch != camp.epoch) {
                ++stats_.staleDamage;
                return;
            }
            // Players take their own damage; everything else lives in the owner's world.
            uint8_t to = isPlayerRef(d.target) ? playerOfRef(d.target) : camp.owner;
            Client* target = member(camp, to);
            if (target && target != &c) target->link.sendFrom(c.id, type, payload, true);
            return;
        }
        case Msg::CharacterRequest: {
            CharacterRequest req;
            if (!decode(payload, req)) return;
            std::string want = normalizeAvatar(cleanAvatar(req.avatar));
            if (want.empty() || want == c.avatar) return;
            const std::vector<CharacterInfo>& catalog = characterCatalog();
            if (std::none_of(catalog.begin(), catalog.end(), [&](const CharacterInfo& ci) { return want == ci.model; })) {
                notice(c, "Unknown character");
                return;
            }
            for (uint8_t other : camp.ids) {
                Client* m = member(camp, other);
                if (m && m != &c && normalizeAvatar(m->avatar) == want) {
                    notice(c, characterName(want) + " is already taken by " + m->name);
                    return;
                }
            }
            c.avatar = want;
            c.appearanceBytes.clear();  // those clothes were for the old character
            ++revision_;
            storage_.journal(camp.name, c.name + " is now " + characterName(want));
            sendRoster(camp);  // the others give the player its new character
            return;
        }
        case Msg::Appearance:
            c.appearanceBytes = payload;
            forward(camp, c, type, payload, true, true);
            return;
        case Msg::Camera:
            forward(camp, c, type, payload, false, true);
            return;
        case Msg::Waypoint:
            camp.waypoint = payload;
            camp.waypointFrom = c.id;
            forward(camp, c, type, payload, true, true);
            return;
        case Msg::Story: {
            StoryInfo info;
            if (!decode(payload, info)) return;
            if (info.event == StoryEvent::Mission) {
                if (info.missionScript != c.missionScript && info.missionScript && isOwner)
                    storage_.journal(camp.name, c.name + " is playing mission " + std::to_string(info.missionScript));
                c.missionScript = info.missionScript;
                return;
            }
            // A death in a mission.
            storage_.journal(camp.name, c.name + " died in a mission");
            // The guard is set on players we just failed: their own death is that failure, not a new one.
            bool guarded = c.failGuardUntil && static_cast<int32_t>(c.failGuardUntil - nowMs) > 0;
            if (config_.storyMode != StoryMode::Shared || !config_.sharedFail || guarded) return;
            // Mismatch flags compare a player with the world owner; the owner's own flag only says that
            // someone differs, so it says nothing about the owner's mission.
            Client* owner = member(camp, camp.owner);
            auto elsewhere = [&](const Client& p) { return p.mismatch && &p != owner; };
            for (uint8_t id : camp.ids) {
                Client* o = member(camp, id);
                if (!o || o == &c || !o->haveState) continue;
                bool inMission = o->state.flags & PF_InMission, dead = o->state.flags & PF_Dead;
                bool known = o->missionScript && c.missionScript;
                bool sameMission = known ? o->missionScript == c.missionScript : !elsewhere(*o) && !elsewhere(c);
                if (!inMission || dead || !sameMission) continue;
                StoryCommandMsg fail;
                fail.cmd = StoryCmd::Fail;
                fail.text = c.name + " died - the mission restarts for everyone";
                command(*o, fail);
                o->failGuardUntil = nowMs + kFailGuardMs;
            }
            return;
        }
        case Msg::SaveRequest: {
            if (c.downloading) return;
            if (!camp.save) {
                if (camp.hasSave) notice(c, "The server cannot read this campaign's save files - tell its admin");
                c.link.send(Msg::SaveChunk, SaveChunk{}, true);  // total 0: nothing to send
                return;
            }
            c.download = camp.save;
            c.downloadCrc = camp.saveCrc;
            c.downloadOffset = 0;
            c.downloading = true;
            pumpDownload(c);
            return;
        }
        case Msg::SaveChunk: {
            SaveChunk& chunk = uploadChunk_;
            if (!decode(payload, chunk) || chunk.total == 0) return;
            // Every refusal is told once per upload, at its first piece (or the piece that finds it
            // refused); the pieces after it are dropped quietly.
            // The campaign save comes from the world owner (or from anyone, to start a campaign).
            if (!isOwner && camp.hasSave) {
                if (chunk.offset == 0 || c.uploading)
                    refuseUpload(c, chunk.crc, "Only the world owner can update the campaign save");
                return;
            }
            if (chunk.offset == 0) {
                if (chunk.total > kMaxSaveBytes) return refuseUpload(c, chunk.crc, "Your save is too large for the server");
                // The buffer grows with what really arrives: announcing 64 MB costs the sender 64 MB.
                dropUpload(c);
                c.uploading = true;
                c.uploadTotal = chunk.total;
                c.uploadCrc = chunk.crc;
                c.upload.reserve(std::min<size_t>(chunk.total, 1u << 20));
            }
            if (!c.uploading) return;  // the rest of an upload that was refused
            if (chunk.offset != c.upload.size() || chunk.total != c.uploadTotal || chunk.crc != c.uploadCrc ||
                c.upload.size() + chunk.data.size() > c.uploadTotal)
                return refuseUpload(c, c.uploadCrc, "Your save upload was interrupted - upload it again");
            if (uploadBytes() + chunk.data.size() > kMaxUploadBytes)
                return refuseUpload(c, c.uploadCrc, "The server is busy - upload your save again later");
            c.upload.insert(c.upload.end(), chunk.data.begin(), chunk.data.end());
            if (c.upload.size() >= c.uploadTotal) finishUpload(c, camp, nowMs);
            return;
        }
        default:
            return;
    }
}

void Server::finishUpload(Client& c, Campaign& camp, uint32_t nowMs) {
    (void)nowMs;
    if (crc32(c.upload.data(), c.upload.size()) != c.uploadCrc)
        return refuseUpload(c, c.uploadCrc, "Your save arrived damaged (checksum mismatch) - upload it again");
    if (!storage_.storeSave(camp.name, c.upload, c.name))
        return refuseUpload(c, c.uploadCrc, "The server could not store your save");
    camp.saveCrc = c.uploadCrc;
    camp.saveTime = static_cast<uint32_t>(std::time(nullptr));
    camp.save = std::make_shared<const std::vector<uint8_t>>(std::move(c.upload));
    dropUpload(c);
    SaveResult result;
    result.crc = c.uploadCrc;
    result.stored = 1;
    c.link.send(Msg::SaveResult, result, true);
    camp.hasSave = true;
    sendRoster(camp);  // everyone learns the campaign's new save (its CRC)
    for (uint8_t id : camp.ids)
        if (Client* m = member(camp, id)) notice(*m, "Campaign saved on the server (by " + c.name + ")");
}

void Server::pumpDownload(Client& c) {
    while (c.downloading && c.link.pendingReliable() < kDownloadWindow) {
        const std::vector<uint8_t>& save = *c.download;
        SaveChunk& chunk = chunk_;
        chunk.total = static_cast<uint32_t>(save.size());
        chunk.crc = c.downloadCrc;
        chunk.offset = static_cast<uint32_t>(c.downloadOffset);
        size_t n = std::min(kSaveChunkBytes, save.size() - c.downloadOffset);
        chunk.data.assign(save.data() + c.downloadOffset, save.data() + c.downloadOffset + n);
        c.link.send(Msg::SaveChunk, chunk, true);
        c.downloadOffset += n;
        if (c.downloadOffset >= save.size()) {
            c.downloading = false;
            c.download.reset();
        }
    }
}

// ---------------------------------------------------------------- story coordination

void Server::command(Client& c, const StoryCommandMsg& cmd) { c.link.send(Msg::StoryCommand, cmd, true); }

void Server::notice(Client& c, const std::string& text) {
    StoryCommandMsg m;
    m.cmd = StoryCmd::Notice;
    m.text = text;
    command(c, m);
}

void Server::hold(Client& c, uint32_t nowMs, const std::string& why) {
    c.holding = true;
    c.holdStartMs = nowMs;
    c.readySinceMs = 0;
    StoryCommandMsg m;
    m.cmd = StoryCmd::Hold;
    m.text = why;
    command(c, m);
}

void Server::release(Client& c, const std::string& why) {
    if (!c.holding) return;
    c.holding = false;
    StoryCommandMsg m;
    m.cmd = StoryCmd::Release;
    m.text = why;
    command(c, m);
}

void Server::storyTick(Campaign& camp, uint32_t nowMs) {
    // Chained missions change scripts a moment apart on different machines: only a difference
    // that lasts is a mismatch.
    constexpr uint32_t kMismatchConfirmMs = 1500;
    // A game that has sent nothing for this long is loading, paused or hung: nobody waits for it.
    constexpr uint32_t kStaleMs = 5000;
    const bool shared = config_.storyMode == StoryMode::Shared;
    Client* owner = member(camp, camp.owner);
    auto names = [](const std::vector<const Client*>& list) {
        std::string out;
        for (size_t i = 0; i < list.size(); ++i) {
            if (i) out += i + 1 == list.size() ? " and " : ", ";
            out += list[i]->name;
        }
        return out;
    };

    // A player seen for the first time on this connection starts from the state they are in: a
    // mission or a scene that already runs when someone (re)joins is not an event.
    for (uint8_t id : camp.ids) {
        Client* m = member(camp, id);
        if (!m || !m->haveState) continue;
        if (m->storyLink != m->linkEpoch) {
            m->storyLink = m->linkEpoch;
            m->prevFlags = m->state.flags;
            m->storyStateTime = m->state.timeMs;
            m->storyFreshMs = nowMs;
            m->mismatchSinceMs = 0;
            m->storyHolding = m->holding;
        } else if (m->state.timeMs != m->storyStateTime) {
            m->storyStateTime = m->state.timeMs;
            m->storyFreshMs = nowMs;
        }
    }
    auto stale = [&](const Client& p) { return nowMs - p.storyFreshMs > kStaleMs; };

    // Which mission does the world owner play; who is in a different one?
    std::vector<const Client*> elsewhere;
    for (uint8_t id : camp.ids) {
        Client* m = member(camp, id);
        if (!m || !m->haveState || m == owner) continue;
        bool both = owner && owner->haveState && (m->state.flags & PF_InMission) && (owner->state.flags & PF_InMission);
        bool differs = shared && both && m->missionScript && owner->missionScript && m->missionScript != owner->missionScript;
        if (!differs) m->mismatchSinceMs = 0;
        else if (!m->mismatchSinceMs) m->mismatchSinceMs = nowMs ? nowMs : 1;
        bool mismatch = differs && nowMs - m->mismatchSinceMs >= kMismatchConfirmMs;
        if (mismatch != m->mismatch) {
            m->mismatch = mismatch;
            StoryCommandMsg cmd;
            cmd.cmd = StoryCmd::Mismatch;
            cmd.flag = mismatch;
            if (mismatch) cmd.text = "You and " + owner->name + " are in different missions - load the campaign save";
            command(*m, cmd);
            if (mismatch && m->holding) release(*m, "");
        }
        if (m->mismatch) elsewhere.push_back(m);
    }

    // The world owner hears about it too.
    bool anyMismatch = !elsewhere.empty();
    if (owner && anyMismatch != owner->mismatch) {
        owner->mismatch = anyMismatch;
        StoryCommandMsg cmd;
        cmd.cmd = StoryCmd::Mismatch;
        cmd.flag = anyMismatch;
        if (anyMismatch)
            cmd.text = names(elsewhere) + (elsewhere.size() == 1 ? " is" : " are") +
                       " in a different mission - they should load the campaign save";
        command(*owner, cmd);
    }

    // The world owner starts a mission: bring the others to the start.
    if (owner && owner->haveState) {
        bool inMission = owner->state.flags & PF_InMission, cut = owner->state.flags & PF_Cutscene;
        bool wasInMission = owner->prevFlags & PF_InMission, wasCut = owner->prevFlags & PF_Cutscene;
        if (inMission && !wasInMission) {
            camp.ownerMissionStartMs = nowMs ? nowMs : 1;
            storage_.journal(camp.name, owner->name + " started a mission");
            for (uint8_t id : camp.ids) {
                Client* m = member(camp, id);
                if (!m || m == owner) continue;
                bool already = m->haveState && (m->state.flags & PF_InMission);
                StoryCommandMsg cmd;
                cmd.cmd = StoryCmd::Regroup;
                cmd.pos = owner->state.pos;
                cmd.distance = shared && !already ? 15.0f : config_.regroupDistance;
                cmd.text = shared && !already ? owner->name + " started a mission - start it here too"
                                              : owner->name + " started a mission";
                command(*m, cmd);
            }
        }
        if (!inMission && wasInMission) storage_.journal(camp.name, owner->name + "'s mission ended");
        if (!shared && wasCut && !cut) {
            // Host world: after the owner's scene, bring far-away players back.
            for (uint8_t id : camp.ids) {
                Client* m = member(camp, id);
                if (!m || m == owner) continue;
                StoryCommandMsg cmd;
                cmd.cmd = StoryCmd::Regroup;
                cmd.pos = owner->state.pos;
                cmd.distance = config_.regroupDistance;
                command(*m, cmd);
            }
        }
        camp.ownerInMission = inMission;
    }

    // Cutscene barrier: whoever finishes a scene first waits for everybody in the same mission.
    if (shared && config_.cutsceneBarrier) {
        const bool joinWindow = owner && camp.ownerInMission && nowMs - camp.ownerMissionStartMs < kJoinWaitMs;
        auto excluded = [&](const Client& p) { return p.mismatch && &p != owner; };
        // Whom a player waits for: the others in the same mission who still watch a scene and, for a
        // while after the owner started the mission, those who have not started it yet. Of the ones it
        // would wait for, `away` have stopped answering; `late` are still not in the mission.
        struct Waits {
            std::vector<const Client*> watching, joining, away, late;
            bool busy() const { return !watching.empty() || !joining.empty(); }
        };
        auto waitsOf = [&](const Client& self) {
            Waits w;
            for (uint8_t id : camp.ids) {
                const Client* o = member(camp, id);
                if (!o || o == &self || !o->haveState || excluded(*o) || (o->state.flags & PF_Dead)) continue;
                bool inMission = o->state.flags & PF_InMission;
                if (inMission && self.missionScript && o->missionScript && self.missionScript != o->missionScript) continue;
                // A frozen player waits like us, whatever its game says about control.
                bool watching = inMission && (o->state.flags & PF_Cutscene) && !o->holding;
                std::vector<const Client*>* list = watching ? &w.watching : inMission ? nullptr : joinWindow ? &w.joining : &w.late;
                if (!list) continue;
                if (list != &w.late && stale(*o)) list = &w.away;
                list->push_back(o);
            }
            return w;
        };
        for (uint8_t id : camp.ids) {
            Client* m = member(camp, id);
            if (!m || !m->haveState) continue;
            uint16_t f = m->state.flags;
            bool inMission = f & PF_InMission, cut = f & PF_Cutscene, dead = f & PF_Dead;
            // A "scene" seen while this player's world was frozen is the freeze itself.
            bool sceneEnded = (m->prevFlags & PF_Cutscene) && !(m->prevFlags & PF_Holding) && !cut && inMission;
            // electOwner() lets everybody go when the world changes hands, but in the shared story each
            // game runs its own mission: whoever still has to wait keeps waiting.
            bool letGo = m->storyHolding && !m->holding;
            bool startWait = !m->holding && (sceneEnded || letGo) && inMission && !cut && !dead && !excluded(*m);
            if (!startWait && !m->holding) continue;
            Waits w = waitsOf(*m);
            if (startWait && w.busy()) {
                uint32_t since = m->holdStartMs;
                hold(*m, nowMs, w.watching.empty() ? "Waiting for " + names(w.joining) + " to start the mission..."
                                                   : "Waiting for " + names(w.watching) + " to finish the cutscene...");
                if (letGo) m->holdStartMs = since;  // the same wait goes on, and so does its timeout
            }
            if (!m->holding) continue;
            if (dead || !inMission) {
                release(*m, "");
            } else if (!w.busy()) {
                if (!m->readySinceMs) m->readySinceMs = nowMs ? nowMs : 1;
                if (nowMs - m->readySinceMs >= kSettleMs) {
                    // Say whom we go on without, if anyone.
                    std::vector<const Client*> without = w.away;
                    bool waitedForJoin = camp.ownerInMission && m->holdStartMs - camp.ownerMissionStartMs < kJoinWaitMs;
                    if (waitedForJoin) without.insert(without.end(), w.late.begin(), w.late.end());
                    release(*m, without.empty() ? "Continuing together" : "Continuing without " + names(without));
                }
            } else {
                m->readySinceMs = 0;
                if (nowMs - m->holdStartMs > config_.barrierTimeoutMs) {
                    std::vector<const Client*> waited = w.watching;
                    waited.insert(waited.end(), w.joining.begin(), w.joining.end());
                    release(*m, "Stopped waiting for " + names(waited));
                }
            }
        }
    }

    for (uint8_t id : camp.ids)
        if (Client* m = member(camp, id); m && m->haveState) {
            m->prevFlags = m->state.flags;
            m->storyHolding = m->holding;
        }
}

}  // namespace coop
