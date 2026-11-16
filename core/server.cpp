#include "server.h"

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
// Per player: messages with side effects (damage, story, waypoints, save requests, ...) and bytes of
// everything that gets passed on. Real play stays far below both.
constexpr float kControlPerSecond = 60, kControlBurst = 120;
constexpr float kBytesPerSecond = 256 * 1024, kBytesBurst = 512 * 1024;
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

// A per-player allowance that refills over time.
struct Budget {
    float tokens = -1;
    uint32_t lastMs = 0;
    bool take(float cost, float perSecond, float burst, uint32_t nowMs) {
        if (tokens < 0) tokens = burst;
        else tokens = std::min(burst, tokens + static_cast<float>(nowMs - lastMs) * perSecond / 1000.0f);
        lastMs = nowMs;
        if (tokens < cost) return false;
        tokens -= cost;
        return true;
    }
};
}  // namespace

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
    bool admitted = false;
    uint8_t id = 0;
    std::string campaign, name, avatar;
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
    // save transfer: an upload grows as its pieces arrive, a download shares the campaign's copy
    std::vector<uint8_t> upload;
    std::shared_ptr<const std::vector<uint8_t>> download;
    size_t uploadTotal = 0, downloadOffset = 0;
    uint32_t uploadCrc = 0, downloadCrc = 0;
    bool uploading = false, downloading = false;
    // limits
    Budget control, bytes;
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
                if (c.admitted) leave(c, nowMs, "rejoined");
                c.linkEpoch = c.link.epoch();
                admit(c, nowMs);
            }
            Msg type;
            while (c.link.poll(type, message_)) {
                if (!c.admitted) continue;
                if (allow(c, type, message_.size(), nowMs)) handle(c, type, message_, nowMs);
                else ++stats_.rateLimited;
            }
            if (c.downloading) pumpDownload(c);
        } else {
            if (c.admitted) leave(c, nowMs, "left");
            if (!c.lostSinceMs) c.lostSinceMs = nowMs ? nowMs : 1;
        }
        bool deniedDone = c.deniedAtMs && nowMs - c.deniedAtMs > kDeniedLingerMs;
        bool gone = c.lostSinceMs && nowMs - c.lostSinceMs > kForgetMs;
        bool neverJoined = !c.link.connected() && !c.linkEpoch && nowMs - c.createdMs > kForgetMs;
        if (deniedDone || gone || neverJoined) {
            if (c.admitted) leave(c, nowMs, "left");
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
        }
    } else if (it == clients_.end()) {
        ++stats_.dropped;  // random traffic costs nothing
        return;
    }
    it->second->link.receive(from, data, size, nowMs);
}

bool Server::allow(Client& c, Msg type, size_t size, uint32_t nowMs) {
    // Save uploads are bounded by size, are not passed on, and should go as fast as the line allows.
    if (type == Msg::SaveChunk) return true;
    if (!c.bytes.take(static_cast<float>(size), kBytesPerSecond, kBytesBurst, nowMs)) return false;
    bool stream = type == Msg::PlayerState || type == Msg::EntitySnapshot || type == Msg::Camera || type == Msg::WorldInfo;
    return stream || c.control.take(1, kControlPerSecond, kControlBurst, nowMs);
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

// ---------------------------------------------------------------- joining and leaving

void Server::admit(Client& c, uint32_t nowMs) {
    const HelloInfo& hello = c.link.peer();
    std::string campaignName = sanitizeCampaign(hello.campaign.empty() ? std::string("default") : hello.campaign);
    auto deny = [&](const std::string& reason) {
        c.link.send(Msg::Denied, Denied{reason}, true);
        c.deniedAtMs = nowMs ? nowMs : 1;
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
            slot->save = std::make_shared<const std::vector<uint8_t>>(std::move(data));
        }
    }
    Campaign& camp = *slot;
    if (static_cast<int>(camp.ids.size()) >= std::min<int>(config_.maxPlayers, kMaxPlayers)) return deny("campaign is full");

    uint8_t id = 1;
    while (camp.byId.count(id)) ++id;
    std::string name = cleanName(hello.name);
    for (int n = 2; std::any_of(camp.byId.begin(), camp.byId.end(), [&](auto& kv) { return kv.second->name == name; }); ++n) {
        std::string suffix = " " + std::to_string(n);
        name = cleanName(hello.name, 24 - suffix.size()) + suffix;  // still within the 24 bytes
    }

    c.admitted = true;
    c.deniedAtMs = 0;  // an earlier attempt on this address may have been turned away
    c.id = id;
    c.campaign = campaignName;
    c.name = name;
    c.avatar = cleanAvatar(hello.avatar);
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
    camp.ids.push_back(id);
    camp.byId[id] = &c;
    ++revision_;

    Welcome w;
    w.playerId = id;
    w.storyMode = config_.storyMode;
    w.hasCampaignSave = camp.hasSave;
    w.campaign = campaignName;
    c.link.send(Msg::Welcome, w, true);
    storage_.journal(campaignName, name + " joined");

    // The world goes to a player who asked for it, or to the first one in.
    Client* owner = member(camp, camp.owner);
    if (!owner || (c.wantsWorld && !owner->wantsWorld)) electOwner(camp, nowMs, c.wantsWorld || !owner ? id : 0);
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
    for (uint8_t other : camp.ids)
        if (Client* o = member(camp, other); o && o != &c) notice(*o, name + " joined the campaign");
}

void Server::leave(Client& c, uint32_t nowMs, const char* why) {
    if (!c.admitted) return;
    c.admitted = false;
    dropUpload(c);
    c.downloading = false;
    c.download.reset();
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
    camp.owner = chosen;
    ++camp.epoch;
    ++revision_;
    // A new world: whatever was cached belongs to the old one.
    camp.cache.clear();
    camp.worldInfo.clear();
    camp.ownerInMission = false;
    for (uint8_t id : camp.ids)
        if (Client* m = member(camp, id); m && m->holding) release(*m, "");
    if (Client* o = member(camp, chosen)) storage_.journal(camp.name, o->name + " now owns the world");
    (void)nowMs;
}

void Server::sendRoster(Campaign& camp) {
    Roster r;
    r.worldOwner = camp.owner;
    r.worldEpoch = camp.epoch;
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
            // Players take their own damage; everything else lives in the owner's world.
            uint8_t to = isPlayerRef(d.target) ? playerOfRef(d.target) : camp.owner;
            Client* target = member(camp, to);
            if (target && target != &c) target->link.sendFrom(c.id, type, payload, true);
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
            if (config_.storyMode != StoryMode::Shared || !config_.sharedFail || nowMs < c.failGuardUntil) return;
            for (uint8_t id : camp.ids) {
                Client* o = member(camp, id);
                if (!o || o == &c || !o->haveState || o->mismatch) continue;
                bool inMission = o->state.flags & PF_InMission, dead = o->state.flags & PF_Dead;
                bool sameMission = !o->missionScript || !c.missionScript || o->missionScript == c.missionScript;
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
                c.link.send(Msg::SaveChunk, SaveChunk{}, true);  // total 0: nothing stored yet
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
            // The campaign save comes from the world owner (or from anyone, to start a campaign).
            if (!isOwner && camp.hasSave) return;
            if (chunk.offset == 0) {
                // The buffer grows with what really arrives: announcing 64 MB costs the sender 64 MB.
                dropUpload(c);
                c.uploading = true;
                c.uploadTotal = chunk.total;
                c.uploadCrc = chunk.crc;
                c.upload.reserve(std::min<size_t>(chunk.total, 1u << 20));
            }
            if (!c.uploading || chunk.offset != c.upload.size() || chunk.total != c.uploadTotal ||
                chunk.crc != c.uploadCrc)
                return;
            if (uploadBytes() + chunk.data.size() > kMaxUploadBytes) {
                dropUpload(c);
                notice(c, "The server is busy - upload your save again later");
                return;
            }
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
    bool ok = crc32(c.upload.data(), c.upload.size()) == c.uploadCrc && storage_.storeSave(camp.name, c.upload, c.name);
    if (ok) {
        camp.saveCrc = c.uploadCrc;
        camp.save = std::make_shared<const std::vector<uint8_t>>(std::move(c.upload));
    }
    dropUpload(c);
    if (!ok) {
        notice(c, "The server could not store your save");
        return;
    }
    camp.hasSave = true;
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
    const bool shared = config_.storyMode == StoryMode::Shared;
    Client* owner = member(camp, camp.owner);

    // Which mission does the world owner play; who is in a different one?
    for (uint8_t id : camp.ids) {
        Client* m = member(camp, id);
        if (!m || !m->haveState || m == owner) continue;
        bool both = owner && owner->haveState && (m->state.flags & PF_InMission) && (owner->state.flags & PF_InMission);
        bool mismatch = shared && both && m->missionScript && owner->missionScript && m->missionScript != owner->missionScript;
        if (mismatch != m->mismatch) {
            m->mismatch = mismatch;
            StoryCommandMsg cmd;
            cmd.cmd = StoryCmd::Mismatch;
            cmd.flag = mismatch;
            if (mismatch) cmd.text = "You and " + owner->name + " are in different missions - load the campaign save";
            command(*m, cmd);
            if (mismatch && m->holding) release(*m, "");
        }
    }

    // The world owner hears about it too.
    bool anyMismatch = false;
    for (uint8_t id : camp.ids)
        if (Client* m = member(camp, id); m && m != owner && m->mismatch) anyMismatch = true;
    if (owner && anyMismatch != owner->mismatch) {
        owner->mismatch = anyMismatch;
        StoryCommandMsg cmd;
        cmd.cmd = StoryCmd::Mismatch;
        cmd.flag = anyMismatch;
        if (anyMismatch) cmd.text = "Someone is in a different mission - they should load the campaign save";
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
        bool joinWindow = owner && camp.ownerInMission && nowMs - camp.ownerMissionStartMs < kJoinWaitMs;
        auto busyOthers = [&](const Client& self) {
            for (uint8_t id : camp.ids) {
                Client* o = member(camp, id);
                if (!o || o == &self || !o->haveState || (o->mismatch && o != owner)) continue;
                bool inMission = o->state.flags & PF_InMission, cut = o->state.flags & PF_Cutscene;
                if (o->state.flags & PF_Dead) continue;
                if (cut) return true;                     // still watching
                if (!inMission && joinWindow) return true; // has not started the mission yet
            }
            return false;
        };
        for (uint8_t id : camp.ids) {
            Client* m = member(camp, id);
            if (!m || !m->haveState) continue;
            uint16_t f = m->state.flags;
            bool inMission = f & PF_InMission, cut = f & PF_Cutscene, dead = f & PF_Dead;
            bool sceneEnded = (m->prevFlags & PF_Cutscene) && !cut && inMission;
            bool excluded = m->mismatch && m != owner;
            if (!m->holding && sceneEnded && !dead && !excluded && busyOthers(*m)) {
                bool watching = false;
                for (uint8_t oid : camp.ids)
                    if (Client* o = member(camp, oid); o && o != m && o->haveState && (o->state.flags & PF_Cutscene)) watching = true;
                hold(*m, nowMs, watching ? "Waiting for the others to finish the cutscene..."
                                         : "Waiting for the others to start the mission...");
            }
            if (!m->holding) continue;
            if (dead || !inMission) {
                release(*m, "");
            } else if (!busyOthers(*m)) {
                if (!m->readySinceMs) m->readySinceMs = nowMs ? nowMs : 1;
                if (nowMs - m->readySinceMs >= kSettleMs) release(*m, "Continuing together");
            } else {
                m->readySinceMs = 0;
                if (nowMs - m->holdStartMs > config_.barrierTimeoutMs) release(*m, "Stopped waiting for the others");
            }
        }
    }

    for (uint8_t id : camp.ids)
        if (Client* m = member(camp, id); m && m->haveState) m->prevFlags = m->state.flags;
}

}  // namespace coop
