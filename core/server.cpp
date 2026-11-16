#include "server.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace coop {

namespace {
constexpr size_t kMaxClients = 512;
constexpr uint32_t kForgetMs = 20000;        // a client that is gone this long is dropped
constexpr uint32_t kDeniedLingerMs = 1500;   // time for the "denied" message to get out
constexpr uint32_t kStoryTickMs = 50;
constexpr uint32_t kPersistMs = 30000;
constexpr uint32_t kJoinWaitMs = 45000;
constexpr uint32_t kSettleMs = 1500;
constexpr uint32_t kFailGuardMs = 3000;      // race guard: a death right after a shared failure is not passed on
constexpr size_t kDownloadWindow = 48;

std::string cleanName(const std::string& raw) {
    std::string out;
    for (char c : raw)
        if (static_cast<unsigned char>(c) >= 32 && c != '=' && c != '\n') out += c;
    if (out.size() > 24) out.resize(24);
    return out.empty() ? "Player" : out;
}
}  // namespace

// The real socket, seen by one client's link: only that client's datagrams, sends go to it.
class Server::ClientSocket final : public IDatagramSocket {
public:
    ClientSocket(IDatagramSocket& real, Endpoint ep) : real_(real), ep_(ep) {}
    bool sendTo(const Endpoint&, const uint8_t* data, size_t size) override { return real_.sendTo(ep_, data, size); }
    int recvFrom(Endpoint& from, uint8_t* buffer, size_t capacity) override {
        if (queue.empty()) return -1;
        std::vector<uint8_t>& d = queue.front();
        size_t n = std::min(capacity, d.size());
        std::copy(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(n), buffer);
        from = ep_;
        queue.pop_front();
        return static_cast<int>(n);
    }
    std::deque<std::vector<uint8_t>> queue;

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
    // save transfer
    std::vector<uint8_t> upload, download;
    size_t uploadOffset = 0, downloadOffset = 0;
    uint32_t uploadCrc = 0, downloadCrc = 0;
    bool downloading = false;
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
    uint32_t ownerMissionStartMs = 0;
    bool ownerInMission = false;
    std::map<uint8_t, Client*> byId;
};

Server::Server(IDatagramSocket& socket, const ServerConfig& config, IServerStorage& storage)
    : socket_(socket), config_(config), storage_(storage) {}
Server::~Server() = default;

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
    int size;
    while ((size = socket_.recvFrom(from, buffer, sizeof(buffer))) >= 0) dispatch(from, buffer, static_cast<size_t>(size), nowMs);

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
            std::vector<uint8_t> payload;
            while (c.link.poll(type, payload)) handle(c, type, payload, nowMs);
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
        for (auto& [ep, c] : clients_)
            if (c->admitted && c->haveState) storage_.storePlayer(c->campaign, c->name, c->state.pos);
    }
    for (auto& [ep, c] : clients_) c->link.flush(nowMs);
}

void Server::dispatch(const Endpoint& from, const uint8_t* data, size_t size, uint32_t nowMs) {
    if (size > kMaxDatagram + 64) return;
    auto it = clients_.find(from);
    if (it == clients_.end()) {
        // Only a proper Hello opens a new client: random traffic costs nothing.
        ByteReader r(data, size);
        PacketType type;
        bool versionOk;
        if (!readPacketHeader(r, type, versionOk) || type != PacketType::Hello || clients_.size() >= kMaxClients) return;
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
    }
    it->second->socket->queue.emplace_back(data, data + size);
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
    if (!config_.password.empty() && hello.password != config_.password) return deny("wrong password");
    auto& slot = campaigns_[campaignName];
    if (!slot) {
        slot = std::make_unique<Campaign>();
        slot->name = campaignName;
        std::vector<uint8_t> probe;
        slot->hasSave = storage_.loadSave(campaignName, probe);
    }
    Campaign& camp = *slot;
    if (static_cast<int>(camp.ids.size()) >= std::min<int>(config_.maxPlayers, kMaxPlayers)) return deny("campaign is full");

    uint8_t id = 1;
    while (camp.byId.count(id)) ++id;
    std::string name = cleanName(hello.name);
    for (int n = 2; std::any_of(camp.byId.begin(), camp.byId.end(), [&](auto& kv) { return kv.second->name == name; }); ++n)
        name = cleanName(hello.name) + " " + std::to_string(n);

    c.admitted = true;
    c.id = id;
    c.campaign = campaignName;
    c.name = name;
    c.avatar = hello.avatar.substr(0, 48);
    c.wantsWorld = hello.wantsWorld != 0;
    c.joinedMs = nowMs;
    c.haveState = false;
    c.stateBytes.clear();
    c.appearanceBytes.clear();
    c.missionScript = 0;
    c.prevFlags = 0;
    c.holding = c.mismatch = false;
    c.upload.clear();
    c.downloading = false;
    camp.ids.push_back(id);
    camp.byId[id] = &c;

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
        if (!o->stateBytes.empty()) c.link.send(Msg::PlayerState, withSource(o->id, o->stateBytes), false);
        if (!o->appearanceBytes.empty()) c.link.send(Msg::Appearance, withSource(o->id, o->appearanceBytes), true);
    }
    if (camp.owner != id) sendWorldTo(camp, c);
    if (!camp.waypoint.empty()) c.link.send(Msg::Waypoint, withSource(camp.waypointFrom, camp.waypoint), true);
    for (uint8_t other : camp.ids)
        if (Client* o = member(camp, other); o && o != &c) notice(*o, name + " joined the campaign");
}

void Server::leave(Client& c, uint32_t nowMs, const char* why) {
    if (!c.admitted) return;
    c.admitted = false;
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
        EntityState& c = camp.cache[e.id];
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
    std::vector<uint8_t> out = prefix ? withSource(from.id, payload) : payload;
    for (uint8_t id : camp.ids) {
        Client* m = member(camp, id);
        if (m && m != &from) m->link.send(type, out, reliable);
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
            EntitySnapshot snap;
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
            if (target && target != &c) target->link.send(type, withSource(c.id, payload), true);
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
            if (!storage_.loadSave(camp.name, c.download) || c.download.empty() || c.download.size() > kMaxSaveBytes) {
                c.download.clear();
                c.link.send(Msg::SaveChunk, SaveChunk{}, true);  // total 0: nothing stored yet
                return;
            }
            c.downloadCrc = crc32(c.download.data(), c.download.size());
            c.downloadOffset = 0;
            c.downloading = true;
            pumpDownload(c);
            return;
        }
        case Msg::SaveChunk: {
            SaveChunk chunk;
            if (!decode(payload, chunk) || chunk.total == 0) return;
            // The campaign save comes from the world owner (or from anyone, to start a campaign).
            if (!isOwner && camp.hasSave) return;
            if (chunk.offset == 0) {
                c.upload.assign(chunk.total, 0);
                c.uploadCrc = chunk.crc;
                c.uploadOffset = 0;
            }
            if (chunk.offset != c.uploadOffset || chunk.total != c.upload.size() || chunk.crc != c.uploadCrc) return;
            std::copy(chunk.data.begin(), chunk.data.end(), c.upload.begin() + static_cast<std::ptrdiff_t>(chunk.offset));
            c.uploadOffset += chunk.data.size();
            if (c.uploadOffset >= c.upload.size()) finishUpload(c, camp, nowMs);
            return;
        }
        default:
            return;
    }
}

void Server::finishUpload(Client& c, Campaign& camp, uint32_t nowMs) {
    (void)nowMs;
    bool ok = crc32(c.upload.data(), c.upload.size()) == c.uploadCrc && storage_.storeSave(camp.name, c.upload, c.name);
    c.upload.clear();
    c.upload.shrink_to_fit();
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
        SaveChunk chunk;
        chunk.total = static_cast<uint32_t>(c.download.size());
        chunk.crc = c.downloadCrc;
        chunk.offset = static_cast<uint32_t>(c.downloadOffset);
        size_t n = std::min(kSaveChunkBytes, c.download.size() - c.downloadOffset);
        chunk.data.assign(c.download.begin() + static_cast<std::ptrdiff_t>(c.downloadOffset),
                          c.download.begin() + static_cast<std::ptrdiff_t>(c.downloadOffset + n));
        c.link.send(Msg::SaveChunk, chunk, true);
        c.downloadOffset += n;
        if (c.downloadOffset >= c.download.size()) {
            c.downloading = false;
            c.download.clear();
            c.download.shrink_to_fit();
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
