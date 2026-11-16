// Tests for the portable core: protocol, link, server, interpolation and the co-op session,
// driven against a simulated game world and an in-memory network with latency, jitter and loss.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "config.h"
#include "interp.h"
#include "link.h"
#include "server.h"
#include "session.h"

using namespace coop;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

// ---------------------------------------------------------------- in-memory network
class MemoryNetwork {
public:
    double loss = 0.0;
    uint32_t latencyMs = 0, jitterMs = 0;
    uint32_t now = 0;
    std::mt19937 rng{1234};

    class Socket : public IDatagramSocket {
    public:
        Socket(MemoryNetwork& net, Endpoint self) : net_(net), self_(self) {}
        bool sendTo(const Endpoint& to, const uint8_t* data, size_t size) override {
            if (std::uniform_real_distribution<double>(0, 1)(net_.rng) < net_.loss) return true;
            if (!net_.sockets_.count(self_)) return true;  // unplugged: nothing gets out either
            auto it = net_.sockets_.find(to);
            if (it == net_.sockets_.end()) return true;
            uint32_t delay = net_.latencyMs;
            if (net_.jitterMs) delay += net_.rng() % (net_.jitterMs + 1);
            it->second->queue_.push_back({net_.now + delay, self_, std::vector<uint8_t>(data, data + size)});
            return true;
        }
        int recvFrom(Endpoint& from, uint8_t* buffer, size_t capacity) override {
            // Deliver the earliest datagram that has arrived (jitter can reorder them).
            int best = -1;
            for (size_t i = 0; i < queue_.size(); ++i)
                if (static_cast<int32_t>(net_.now - queue_[i].at) >= 0 &&
                    (best < 0 || static_cast<int32_t>(queue_[i].at - queue_[best].at) < 0))
                    best = static_cast<int>(i);
            if (best < 0) return -1;
            Datagram d = std::move(queue_[best]);
            queue_.erase(queue_.begin() + best);
            size_t n = std::min(capacity, d.data.size());
            std::copy(d.data.begin(), d.data.begin() + n, buffer);
            from = d.from;
            return static_cast<int>(n);
        }
        Endpoint self() const { return self_; }

    private:
        friend class MemoryNetwork;
        struct Datagram {
            uint32_t at;
            Endpoint from;
            std::vector<uint8_t> data;
        };
        MemoryNetwork& net_;
        Endpoint self_;
        std::vector<Datagram> queue_;
    };

    Socket* open(uint32_t ip, uint16_t port) {
        Endpoint e{ip, port};
        owned_.push_back(std::make_unique<Socket>(*this, e));
        sockets_[e] = owned_.back().get();
        return owned_.back().get();
    }
    void unplug(Socket* s) { sockets_.erase(s->self()); }
    void plug(Socket* s) { sockets_[s->self()] = s; }

private:
    std::map<Endpoint, Socket*> sockets_;
    std::vector<std::unique_ptr<Socket>> owned_;
};

// ---------------------------------------------------------------- simulated game
class FakeGame : public IGame {
public:
    struct Ent {
        EntityKind kind = EntityKind::Ped;
        uint32_t model = 0;
        Vec3 pos;
        Vec3 vel;
        float heading = 0;
        int health = 100;
        bool ours = false;  // created through spawn()
        SpawnRole role = SpawnRole::Mirror;
        bool mission = false;  // script-owned: only hidden, never deleted
        bool hidden = false;
        bool hitByPlayer = false;
        EntityHandle mount = 0, vehicle = 0, target = 0, draftOf = 0;
        int seat = -1;
        uint32_t weapon = 0;
        uint8_t flags = 0;
        bool crouch = false;
        int shots = 0, aims = 0, ragdolls = 0, teleports = 0, tasks = 0, nudges = 0;
        std::string label;
        bool hasGoal = false, steered = false;
        bool parked = false;
        bool harmless = false;
        uint32_t outfit = 0, script = 0;
        int faces = 0, updates = 0;  // updates: any driving call (task, nudge, face, steer)
        mutable int queries = 0;     // position / health reads
        Vec3 goal;
        float goalSpeed = 0;
        Vec3 aimPoint;
    };

    std::map<EntityHandle, Ent> ents;
    EntityHandle next = 100;
    EntityHandle playerPed = 0;
    PlayerState player;
    int playerHealth = 200;
    EntityHandle playerMount = 0, playerVehicle = 0;
    int playerSeat = -1;
    EntityHandle ownHorse = 0;
    bool isReady = true;
    bool populationSuppressed = false;
    WorldInfo world{12, 0, 0, 0, 0};
    int modelLoadDelay = 0;  // spawn() returns 0 this many times first
    int readCalls = 0;
    int spawns = 0, removals = 0, prefetches = 0;
    uint32_t playerOutfit = 0;
    bool playerProtected = false;
    bool held = false;
    uint32_t missionScript = 0;
    bool cameraValid = false, spectating = false;
    CameraState camera, spectateCam;
    bool waypoint = false, friendWaypoint = false;
    Vec3 waypointPos, friendWaypointPos;

    FakeGame() {
        playerPed = add(EntityKind::Ped, joaat("player"), {});
    }

    EntityHandle add(EntityKind kind, uint32_t model, Vec3 pos, int health = 100) {
        EntityHandle h = next++;
        Ent e;
        e.kind = kind;
        e.model = model;
        e.pos = pos;
        e.health = health;
        ents[h] = e;
        return h;
    }
    EntityHandle addPed(uint32_t model, Vec3 pos, int health = 100) { return add(EntityKind::Ped, model, pos, health); }
    void playerShoots(EntityHandle h, int damage) {
        Ent& e = ents.at(h);
        e.health = std::max(0, e.health - damage);
        e.hitByPlayer = true;
    }
    int count(SpawnRole role, EntityKind kind = EntityKind::Ped) const {
        int n = 0;
        for (auto& [h, e] : ents) n += e.ours && !e.parked && e.role == role && e.kind == kind;
        return n;
    }
    EntityHandle find(SpawnRole role, uint32_t model = 0, EntityKind kind = EntityKind::Ped) const {
        for (auto& [h, e] : ents)
            if (e.ours && !e.parked && e.role == role && e.kind == kind && (!model || e.model == model)) return h;
        return 0;
    }

    // One frame of "physics": tasks move peds, steering moves vehicles, riders follow mounts.
    void simulate(float dt) {
        for (auto& [h, e] : ents) {
            if (e.kind == EntityKind::Ped && e.hasGoal && !e.mount && !e.vehicle) {
                Vec3 d = e.goal - e.pos;
                d.z = 0;
                float len = length(d), step = e.goalSpeed * dt;
                if (len <= step || len < 1e-4f) {
                    e.pos.x = e.goal.x;
                    e.pos.y = e.goal.y;
                } else {
                    e.pos = e.pos + d * (step / len);
                }
            }
            if (e.steered) e.pos = e.pos + e.vel * dt;  // physics keeps the last velocity
        }
        for (auto& [h, e] : ents) {
            if (e.mount && ents.count(e.mount)) e.pos = ents[e.mount].pos;
            if (e.vehicle && ents.count(e.vehicle)) e.pos = ents[e.vehicle].pos;
        }
    }

    bool ready() override { return isReady; }
    LocalPlayer readLocalPlayer() override {
        LocalPlayer lp;
        lp.state = player;
        lp.state.health = static_cast<uint16_t>(playerHealth);
        lp.state.maxHealth = 200;
        if (playerHealth <= 0) lp.state.flags |= PF_Dead;
        lp.mount = playerMount;
        lp.vehicle = playerVehicle;
        if (playerMount) {
            lp.state.flags |= PF_OnMount;
            lp.state.mountModel = ents.at(playerMount).model;
        }
        if (playerVehicle) {
            lp.state.flags |= PF_InVehicle;
            lp.state.vehicleModel = ents.at(playerVehicle).model;
            lp.state.seat = static_cast<int8_t>(playerSeat);
        }
        ents.at(playerPed).pos = player.pos;
        return lp;
    }
    EntityHandle localPlayerPed() override { return playerPed; }
    void damageLocalPlayer(int amount) override { playerHealth = std::max(0, playerHealth - amount); }
    void teleportLocalPlayer(const Vec3& pos) override { player.pos = pos; }
    uint32_t localOutfit() override { return playerOutfit; }
    void setLocalProtected(bool on) override { playerProtected = on; }
    void killLocalPlayer() override { playerHealth = 0; }
    void holdWorld(bool on) override { held = on; }
    uint32_t localMissionScript() override { return missionScript; }
    void listMissionEntities(std::vector<MissionEntity>& out) override {
        for (auto& [h, e] : ents)
            if (e.mission && !e.ours && !belongsToLocalPlayer(h)) out.push_back({h, e.kind, e.model, e.script, e.pos});
    }
    void setHarmless(EntityHandle h, bool on) override { ents.at(h).harmless = on; }
    void revive(EntityHandle h, int health) override { ents.at(h).health = health; }
    bool readCamera(CameraState& out) override {
        out = camera;
        return cameraValid;
    }
    void spectate(bool on, const CameraState& cam) override {
        spectating = on;
        spectateCam = cam;
    }
    bool readWaypoint(Vec3& out) override {
        out = waypointPos;
        return waypoint;
    }
    void showFriendWaypoint(bool on, const Vec3& pos) override {
        friendWaypoint = on;
        friendWaypointPos = pos;
    }
    bool belongsToLocalPlayer(EntityHandle h) override {
        return h == playerPed || h == ownHorse || (h && (h == playerMount || h == playerVehicle));
    }

    void listNearby(const Vec3& a, const Vec3& b, float radius, std::vector<NearbyEntity>& out) override {
        for (auto& [h, e] : ents) {
            if (belongsToLocalPlayer(h) || e.draftOf || e.parked) continue;
            if (distance(e.pos, a) > radius && distance(e.pos, b) > radius) continue;
            out.push_back({h, e.kind, e.pos});
        }
    }
    void readEntity(EntityHandle h, EntityKind, EntityRead& out) override {
        ++readCalls;
        const Ent& e = ents.at(h);
        out.state.kind = e.kind;
        out.state.model = e.model;
        out.state.outfit = e.outfit;
        out.state.script = e.mission ? e.script : 0;
        out.state.pos = e.pos;
        out.state.vel = e.vel;
        out.state.heading = e.heading;
        out.state.health = static_cast<int16_t>(e.health);
        out.state.flags = e.flags | (e.health <= 0 ? NF_Dead : 0);
        out.state.weapon = e.weapon;
        out.state.seat = static_cast<int8_t>(e.seat);
        out.mount = e.mount;
        out.vehicle = e.vehicle;
        out.target = e.target;
    }
    void listForeign(std::vector<ForeignEntity>& out) override {
        for (auto& [h, e] : ents)
            if (h != playerPed) out.push_back({h, e.draftOf, e.mission});
    }
    WorldInfo readWorldInfo() override { return world; }
    void applyWorldInfo(const WorldInfo& info, bool clock, bool weather) override {
        if (clock) {
            world.hour = info.hour;
            world.minute = info.minute;
        }
        if (weather) world.weather = info.weather;
    }

    EntityHandle spawn(EntityKind kind, uint32_t model, const Vec3& pos, float heading, SpawnRole role) override {
        if (modelLoadDelay > 0) {
            --modelLoadDelay;
            return 0;
        }
        EntityHandle h = add(kind, model, pos);
        Ent& e = ents[h];
        e.heading = heading;
        e.ours = true;
        e.role = role;
        ++spawns;
        return h;
    }
    void prefetchModel(uint32_t) override { ++prefetches; }
    void park(EntityHandle h) override {
        Ent& e = ents.at(h);
        e.parked = e.hidden = true;
        e.hasGoal = false;
        e.mount = e.vehicle = 0;
    }
    void unpark(EntityHandle h, const Vec3& pos, float heading) override {
        Ent& e = ents.at(h);
        e.parked = e.hidden = false;
        e.pos = pos;
        e.heading = heading;
    }
    void remove(EntityHandle h) override {
        ++removals;
        ents.erase(h);
        for (auto& [k, e] : ents) {
            if (e.mount == h) e.mount = 0;
            if (e.vehicle == h) e.vehicle = 0;
        }
    }
    bool exists(EntityHandle h) override { return ents.count(h) != 0; }
    bool isDead(EntityHandle h) override { return ents.at(h).health <= 0; }
    Vec3 position(EntityHandle h) override {
        ++ents.at(h).queries;
        return ents.at(h).pos;
    }
    float heading(EntityHandle h) override { return ents.at(h).heading; }
    int health(EntityHandle h) override {
        ++ents.at(h).queries;
        return ents.at(h).health;
    }
    void setHealth(EntityHandle h, int health, int) override { ents.at(h).health = health; }
    void kill(EntityHandle h) override { ents.at(h).health = 0; }
    void teleport(EntityHandle h, const Vec3& pos, float heading) override {
        Ent& e = ents.at(h);
        e.pos = pos;
        e.heading = heading;
        e.hasGoal = false;
        ++e.teleports;
        ++e.updates;
    }
    void nudge(EntityHandle h, const Vec3& delta) override {
        ents.at(h).pos = ents.at(h).pos + delta;
        ++ents.at(h).nudges;
        ++ents.at(h).updates;
    }
    void moveTo(EntityHandle h, const Vec3& pos, float speed, float heading) override {
        Ent& e = ents.at(h);
        e.hasGoal = true;
        e.goal = pos;
        e.goalSpeed = speed;
        e.heading = heading;
        ++e.tasks;
        ++e.updates;
    }
    void standStill(EntityHandle h, float heading) override {
        ents.at(h).hasGoal = false;
        ents.at(h).heading = heading;
        ++ents.at(h).tasks;
    }
    void face(EntityHandle h, float heading) override {
        Ent& e = ents.at(h);
        e.heading = lerpHeading(e.heading, heading, 0.5f);
        ++e.faces;
        ++e.updates;
    }
    void setOutfit(EntityHandle h, uint32_t outfit) override { ents.at(h).outfit = outfit; }
    void aimAt(EntityHandle h, const Vec3& target) override {
        ++ents.at(h).aims;
        ents.at(h).aimPoint = target;
    }
    void shootAt(EntityHandle h, const Vec3& target) override {
        ++ents.at(h).shots;
        ents.at(h).aimPoint = target;
    }
    void setWeapon(EntityHandle h, uint32_t weapon) override { ents.at(h).weapon = weapon; }
    void setMount(EntityHandle rider, EntityHandle mount) override { ents.at(rider).mount = mount; }
    void setVehicle(EntityHandle ped, EntityHandle vehicle, int seat) override {
        ents.at(ped).vehicle = vehicle;
        ents.at(ped).seat = vehicle ? seat : -1;
    }
    void setCrouch(EntityHandle h, bool crouch) override { ents.at(h).crouch = crouch; }
    void ragdoll(EntityHandle h) override { ++ents.at(h).ragdolls; }
    void setLabel(EntityHandle h, const char* name) override { ents.at(h).label = name; }
    void steerVehicle(EntityHandle h, const Vec3& velocity, float heading, float, float, float) override {
        Ent& e = ents.at(h);
        ++e.updates;
        e.vel = velocity;
        e.heading = heading;
        e.steered = true;
    }
    void damageNpc(EntityHandle npc, int amount, EntityHandle) override {
        ents.at(npc).health = std::max(0, ents.at(npc).health - amount);
    }
    bool wasDamagedByLocalPlayer(EntityHandle h) override {
        bool hit = ents.at(h).hitByPlayer;
        ents.at(h).hitByPlayer = false;
        return hit;
    }
    void suppressAmbientPopulation() override { populationSuppressed = true; }
    ForeignAction removeForeign(EntityHandle h) override {
        if (ents.at(h).mission) {
            ents.at(h).hidden = true;
            return ForeignAction::Hidden;
        }
        remove(h);
        return ForeignAction::Deleted;
    }
    void restoreForeign(EntityHandle h) override { ents.at(h).hidden = false; }
};

// ---------------------------------------------------------------- helpers
static const Endpoint kHostAddr{0x0A000001, 27960};
static const Endpoint kGuestAddr{0x0A000002, 40000};

static LinkConfig hostConfig() {
    LinkConfig c;
    c.listen = true;
    c.local.role = Role::Host;
    c.local.name = "Arthur";
    c.local.avatar = "player_zero";
    return c;
}
static LinkConfig guestConfig() {
    LinkConfig c;
    c.remote = kHostAddr;
    c.local.role = Role::Guest;
    c.local.name = "John";
    c.local.avatar = "player_three";
    return c;
}

class MemorySaveStore : public ISaveStore {
public:
    std::vector<uint8_t> latest;
    uint64_t stamp = 0;
    std::vector<std::vector<uint8_t>> written;
    uint64_t latestStamp() override { return latest.empty() ? 0 : stamp; }
    bool readLatest(std::vector<uint8_t>& data, std::string& name) override {
        if (latest.empty()) return false;
        data = latest;
        name = "SRDR30003";
        return true;
    }
    bool writeCopy(const std::vector<uint8_t>& data, std::string& placedAs, std::string&) override {
        written.push_back(data);
        placedAs = "SRDR30007";
        return true;
    }
};

static const Endpoint kServerAddr{0x0A000009, 27960};

// One player: game, link to the server, session.
struct Player {
    MemoryNetwork::Socket* sock = nullptr;
    Link link;
    FakeGame game;
    MemorySaveStore saves;
    std::unique_ptr<CoopSession> session;
    LinkConfig linkConfig;

    void connect(MemoryNetwork& net, uint32_t ip, const std::string& name, const std::string& avatar, bool wantsWorld,
                 const SessionConfig& config, uint32_t now, const std::string& campaign = "default",
                 const std::string& password = "") {
        if (!sock) sock = net.open(ip, 40000);
        linkConfig = LinkConfig{};
        linkConfig.remote = kServerAddr;
        linkConfig.local.role = Role::Guest;
        linkConfig.local.name = name;
        linkConfig.local.avatar = avatar;
        linkConfig.local.campaign = campaign;
        linkConfig.local.password = password;
        linkConfig.local.wantsWorld = wantsWorld ? 1 : 0;
        link.start(sock, linkConfig, now);
        session = std::make_unique<CoopSession>(game, link, config);
        session->setSaveStore(&saves);
    }
};

// A server with any number of players on a simulated network.
struct Cluster {
    MemoryNetwork net;
    MemoryNetwork::Socket* serverSock = net.open(kServerAddr.ip, kServerAddr.port);
    MemoryStorage storage;
    ServerConfig serverConfig;
    std::unique_ptr<Server> server;
    SessionConfig config;
    std::vector<std::unique_ptr<Player>> extra;
    std::vector<Player*> players;
    uint32_t now = 1000;
    std::function<void()> perFrame;

    void startServer() {
        net.now = now;
        if (!server) server = std::make_unique<Server>(*serverSock, serverConfig, storage);
    }
    Player& join(const std::string& name, bool wantsWorld = false, const std::string& campaign = "default",
                 const std::string& password = "") {
        startServer();
        extra.push_back(std::make_unique<Player>());
        Player& p = *extra.back();
        p.connect(net, 0x0A000100u + static_cast<uint32_t>(extra.size()), name, "player_" + name, wantsWorld, config, now,
                  campaign, password);
        players.push_back(&p);
        return p;
    }
    void run(uint32_t ms) {
        for (uint32_t end = now + ms; now < end; now += 16) {
            net.now = now;
            if (perFrame) perFrame();
            if (server) server->pump(now);
            for (Player* p : players)
                if (p->session) p->session->tick(now);
            for (Player* p : players) p->game.simulate(0.016f);
        }
    }
};

// The classic two-player setup: the host owns the world, the guest joins it.
struct Pair : Cluster {
    Player hostP, guestP;
    FakeGame& hostGame = hostP.game;
    FakeGame& guestGame = guestP.game;
    Link& hostLink = hostP.link;
    Link& guestLink = guestP.link;
    std::unique_ptr<CoopSession>& host = hostP.session;
    std::unique_ptr<CoopSession>& guest = guestP.session;

    void start() {
        startServer();
        hostP.connect(net, 0x0A000001, "Arthur", "player_zero", true, config, now);
        players.push_back(&hostP);
        run(100);  // the host is in first and owns the world
        guestP.connect(net, 0x0A000002, "John", "player_three", false, config, now);
        players.push_back(&guestP);
    }
};

static EntityHandle mirrorOf(FakeGame& g, uint32_t model, EntityKind kind = EntityKind::Ped) {
    return g.find(SpawnRole::Mirror, model, kind);
}

// ---------------------------------------------------------------- tests
static void testProtocol() {
    PlayerState s;
    s.timeMs = 42;
    s.pos = {1500.5f, -2.5f, 3.25f};
    s.vel = {4.2f, -1.0f, 0.3f};
    s.heading = 271.0f;
    s.aim = {1510.0f, 0.0f, 4.0f};
    s.health = 180;
    s.weapon = joaat("weapon_revolver_cattleman");
    s.flags = PF_InVehicle | PF_Shooting;
    s.vehicleRef = 77;
    s.vehicleModel = joaat("wagon02x");
    s.seat = 1;
    PlayerState t;
    std::vector<uint8_t> bytes = encode(s);
    CHECK(bytes.size() <= 48);
    CHECK(decode(bytes, t));
    CHECK(t.timeMs == 42 && t.pos.x == 1500.5f && t.health == 180 && t.flags == s.flags);
    CHECK(std::abs(t.vel.x - 4.2f) < 0.01f && std::abs(angleDelta(t.heading, 271.0f)) < 0.01f);
    CHECK(distance(t.aim, s.aim) < 0.05f);
    CHECK(t.vehicleRef == 77 && t.seat == 1 && t.vehicleModel == s.vehicleModel);

    // Entities: a near one costs a handful of bytes, a far one falls back to floats.
    EntitySnapshot snap;
    snap.timeMs = 7;
    snap.anchor = {100, 100, 10};
    EntityState near;
    near.id = 5;
    near.fields = EF_Pos | EF_Heading | EF_Vel;
    near.pos = {123.45f, 90.0f, 11.0f};
    near.heading = 33.0f;
    near.vel = {3.0f, -2.5f, 0.0f};
    EntityState far = near;
    far.id = 6;
    far.fields = EF_Static | EF_Pos | EF_State | EF_Health | EF_Tilt;
    far.kind = EntityKind::Vehicle;
    far.model = 999;
    far.pos = {-2000.0f, 3000.0f, 50.0f};
    far.health = 321;
    far.flags = NF_Shooting;
    far.vehicleRef = 6;
    far.seat = 2;
    far.targetRef = playerRef(2);
    far.pitch = 9.0f;
    far.roll = -4.5f;
    snap.entities = {near, far};
    snap.removed = {9, 10};
    CHECK(encodedSize(near, snap.anchor) == 14);
    bytes = encode(snap);
    EntitySnapshot back;
    CHECK(decode(bytes, back));
    CHECK(back.entities.size() == 2 && back.removed.size() == 2 && back.removed[1] == 10);
    CHECK(distance(back.entities[0].pos, near.pos) < 0.011f);
    CHECK(std::abs(back.entities[0].vel.y + 2.5f) < 0.01f);
    CHECK(back.entities[0].fields == near.fields);
    CHECK(back.entities[1].pos.x == -2000.0f && back.entities[1].kind == EntityKind::Vehicle);
    CHECK(back.entities[1].targetRef == playerRef(2) && back.entities[1].seat == 2 && back.entities[1].health == 321);
    CHECK(std::abs(back.entities[1].pitch - 9.0f) < 0.01f && std::abs(back.entities[1].roll + 4.5f) < 0.01f);

    // Splitting respects the size budget and loses nothing.
    std::vector<EntityState> many;
    for (int i = 1; i <= 300; ++i) {
        EntityState e = far;
        e.id = static_cast<uint16_t>(i);
        e.pos = {float(i), 0, 0};
        many.push_back(e);
    }
    std::vector<uint16_t> removed(400, 3);
    std::vector<std::vector<uint8_t>> packets;
    encodeSnapshots(1, {}, many, removed, kMaxMessage, packets);
    size_t total = 0, totalRemoved = 0;
    for (const auto& p : packets) {
        CHECK(p.size() <= kMaxMessage);
        EntitySnapshot part;
        CHECK(decode(p, part));
        total += part.entities.size();
        totalRemoved += part.removed.size();
    }
    CHECK(total == 300 && totalRemoved == 400);

    bytes = encode(snap);
    bytes.resize(bytes.size() - 3);  // truncated input must be rejected, not read out of bounds
    CHECK(!decode(bytes, back));
    CHECK(!decode(std::vector<uint8_t>{1, 2}, t));

    CHECK(joaat("player_zero") == 0x0D7114C9u);
    CHECK(joaat("PLAYER_THREE") == 0x00B69710u);
}

static void testInterpolationSmooth() {
    // A rider circles at 8 m/s; 30 Hz updates arrive with 60-140 ms latency, reordering and 10% loss.
    std::mt19937 rng(7);
    MotionBuffer buf;
    const float radius = 20.0f, speed = 8.0f, omega = speed / radius;
    auto truth = [&](float t) { return Vec3{radius * std::cos(omega * t), radius * std::sin(omega * t), 0}; };
    auto truthVel = [&](float t) {
        return Vec3{-radius * omega * std::sin(omega * t), radius * omega * std::cos(omega * t), 0};
    };
    struct InFlight { uint32_t at; MotionSample s; };
    std::vector<InFlight> flight;
    const uint32_t clockOffset = 123456;  // sender and receiver clocks are unrelated
    float maxError = 0, maxStepJump = 0, maxAccel = 0, sumError = 0;
    int frames = 0, backwards = 0;
    Vec3 prevPos, prevVel;
    bool havePrev = false;
    for (uint32_t local = 0; local < 20000; local += 16) {
        uint32_t sender = local + clockOffset;
        if (local % 33 < 16) {
            float t = sender / 1000.0f;
            MotionSample s;
            s.timeMs = sender;
            s.pos = truth(t);
            s.vel = truthVel(t);
            s.hasVel = true;
            if (rng() % 10 != 0) flight.push_back({local + 60 + static_cast<uint32_t>(rng() % 81), s});
        }
        for (size_t i = 0; i < flight.size();) {
            if (flight[i].at <= local) {
                buf.push(flight[i].s, local);
                flight.erase(flight.begin() + i);
            } else {
                ++i;
            }
        }
        if (buf.empty() || local < 2000) continue;
        MotionSample out = buf.sample(local);
        // Compare with the true path at the time being shown.
        float shown = out.timeMs / 1000.0f;
        float err = distance(out.pos, truth(shown));
        maxError = std::max(maxError, err);
        sumError += err;
        ++frames;
        if (havePrev) {
            Vec3 step = out.pos - prevPos;
            maxStepJump = std::max(maxStepJump, length(step));
            if (step.x * prevVel.x + step.y * prevVel.y < 0) ++backwards;
            maxAccel = std::max(maxAccel, length(out.vel - prevVel) / 0.016f);
        }
        prevPos = out.pos;
        prevVel = out.vel;
        havePrev = true;
    }
    std::printf("  interpolation: max error %.3f m, mean %.3f m, max frame step %.3f m, max accel %.1f m/s2, delay %u ms\n",
                maxError, sumError / frames, maxStepJump, maxAccel, buf.delayMs());
    CHECK(maxError < 0.15f);               // on the true curve, not cutting corners
    CHECK(maxStepJump < speed * 0.016f * 1.25f);  // playback never jumps
    CHECK(backwards == 0);
    CHECK(maxAccel < 20.0f);               // true centripetal accel is 3.2 m/s2; no jerky velocity
    CHECK(buf.delayMs() >= 100 && buf.delayMs() <= 250);  // covers the jitter, no more

    // On a clean connection the buffer shrinks.
    MotionBuffer clean;
    for (uint32_t local = 0; local < 10000; local += 16) {
        if (local % 33 < 16) {
            MotionSample s;
            s.timeMs = local;
            s.pos = {local / 100.0f, 0, 0};
            clean.push(s, local + 20);
        }
        clean.sample(local + 20);
    }
    CHECK(clean.delayMs() <= 60);

    // Linear fallback when velocities cannot explain a jump (teleport), and out-of-order samples ignored.
    MotionBuffer tp;
    MotionSample a;
    a.timeMs = 1000;
    a.hasVel = true;
    tp.push(a, 1000);
    MotionSample b = a;
    b.timeMs = 1100;
    b.pos = {100, 0, 0};
    tp.push(b, 1100);
    MotionSample old = a;
    old.timeMs = 1050;
    old.pos = {999, 0, 0};
    tp.push(old, 1150);
    CHECK(tp.latest().pos.x == 100.0f);
}

static void testIni() {
    IniFile ini;
    ini.parse("\xEF\xBB\xBF; comment\n[Network]\nRole = guest\naddress=1.2.3.4:555 ; friend\n[sync]\nworld_sync=0\nradius=80.5\n");
    CHECK(ini.get("network", "role") == "guest");
    CHECK(ini.get("network", "ADDRESS") == "1.2.3.4:555");
    CHECK(!ini.getBool("sync", "world_sync", true));
    CHECK(ini.getFloat("sync", "radius", 1) == 80.5f);
    CHECK(ini.getInt("sync", "missing", 7) == 7);
}

static void testLinkReliableUnderLoss() {
    MemoryNetwork net;
    net.loss = 0.35;
    net.latencyMs = 40;
    net.jitterMs = 40;
    auto* a = net.open(kHostAddr.ip, kHostAddr.port);
    auto* b = net.open(kGuestAddr.ip, kGuestAddr.port);
    Link host, guest;
    host.start(a, hostConfig(), 0);
    guest.start(b, guestConfig(), 0);

    std::vector<uint32_t> received;
    uint32_t sent = 0;
    for (uint32_t now = 0; now < 40000; now += 16) {
        net.now = now;
        host.update(now);
        guest.update(now);
        if (guest.connected() && sent < 300) {
            ByteWriter w;
            w.u32(sent++);
            guest.send(Msg::Damage, w.data, true);
        }
        Msg type;
        std::vector<uint8_t> payload;
        while (host.poll(type, payload)) {
            ByteReader r(payload.data(), payload.size());
            received.push_back(r.u32());
        }
        host.flush(now);
        guest.flush(now);
    }
    CHECK(host.connected() && guest.connected());
    CHECK(host.peer().name == "John" && guest.peer().avatar == "player_zero");
    CHECK(received.size() == 300);
    bool ordered = true;
    for (size_t i = 0; i < received.size(); ++i) ordered = ordered && received[i] == i;
    CHECK(ordered);
    CHECK(host.pingMs() >= 80);
    CHECK(host.sendRate() > 0 && host.receiveRate() > 0);
}

static void testLinkReconnectAndRoleClash() {
    MemoryNetwork net;
    auto* a = net.open(kHostAddr.ip, kHostAddr.port);
    auto* b = net.open(kGuestAddr.ip, kGuestAddr.port);
    Link host, guest;
    host.start(a, hostConfig(), 0);
    guest.start(b, guestConfig(), 0);
    uint32_t now = 0;
    auto run = [&](uint32_t ms) {
        for (uint32_t end = now + ms; now < end; now += 16) {
            net.now = now;
            host.update(now);
            guest.update(now);
            host.flush(now);
            guest.flush(now);
        }
    };
    run(1000);
    CHECK(host.connected() && guest.connected());
    uint32_t firstEpoch = host.epoch();

    net.unplug(b);  // guest drops off the network
    run(12000);
    CHECK(host.state() == Link::State::Connecting);
    net.plug(b);
    run(3000);
    CHECK(host.connected() && guest.connected());
    CHECK(host.epoch() > firstEpoch);

    guest.send(Msg::Damage, Damage{5, 9}, true);
    run(500);
    Msg type;
    std::vector<uint8_t> payload;
    bool got = false;
    while (host.poll(type, payload)) got = got || type == Msg::Damage;
    CHECK(got);

    guest.stop();
    run(200);
    CHECK(host.state() == Link::State::Connecting);

    Link other;
    LinkConfig clash = guestConfig();
    clash.local.role = Role::Host;
    other.start(b, clash, now);
    for (uint32_t end = now + 1000; now < end; now += 16) {
        net.now = now;
        host.update(now);
        other.update(now);
    }
    CHECK(other.state() == Link::State::Failed && !other.error().empty());
    CHECK(host.state() == Link::State::Connecting && !host.error().empty());
}

static void testSessionSharedWorld() {
    Pair p;  // host-world story mode: the guest's own story entities are hidden while visiting
    p.serverConfig.storyMode = StoryMode::HostWorld;
    const uint32_t kOutlaw = joaat("g_m_m_uniduster_01");
    const uint32_t kHorse = joaat("a_c_horse_morgan_bay");
    const uint32_t kWagon = joaat("wagon02x");
    p.hostGame.player.pos = {100, 100, 50};
    p.guestGame.player.pos = {110, 100, 50};
    EntityHandle outlaw = p.hostGame.addPed(kOutlaw, {105, 105, 50});
    EntityHandle horse = p.hostGame.addPed(kHorse, {120, 100, 50});
    EntityHandle rider = p.hostGame.addPed(kOutlaw, {120, 100, 51});
    p.hostGame.ents[rider].mount = horse;
    EntityHandle wagon = p.hostGame.add(EntityKind::Vehicle, kWagon, {90, 120, 50});
    EntityHandle draft = p.hostGame.addPed(kHorse, {90, 124, 50});
    p.hostGame.ents[draft].draftOf = wagon;  // pulled with the wagon, never streamed on its own
    EntityHandle driver = p.hostGame.addPed(joaat("dutch"), {90, 120, 51});
    p.hostGame.ents[driver].vehicle = wagon;
    p.hostGame.ents[driver].seat = -1;
    p.hostGame.addPed(kOutlaw, {5000, 5000, 0});  // far away: must not be streamed
    // The guest's own world: an ambient ped (deleted), a story ped (hidden), the guest's own horse.
    EntityHandle stray = p.guestGame.addPed(kOutlaw, {111, 101, 50});
    EntityHandle storyPed = p.guestGame.addPed(kOutlaw, {112, 101, 50});
    p.guestGame.ents[storyPed].mission = true;
    p.guestGame.ownHorse = p.guestGame.addPed(kHorse, {112, 100, 50});
    p.guestGame.modelLoadDelay = 3;
    p.hostGame.world = WorldInfo{21, 30, 0, 0, joaat("RAIN")};

    p.start();
    p.run(1500);

    EntityHandle guestPuppet = p.hostGame.find(SpawnRole::PlayerPuppet);
    EntityHandle hostPuppet = p.guestGame.find(SpawnRole::PlayerPuppet);
    CHECK(guestPuppet && hostPuppet);
    CHECK(p.hostGame.ents[guestPuppet].model == joaat("player_three"));
    CHECK(p.guestGame.ents[hostPuppet].model == joaat("player_zero"));
    CHECK(p.hostGame.ents[guestPuppet].label == "John");
    CHECK(distance(p.hostGame.ents[guestPuppet].pos, p.guestGame.player.pos) < 0.5f);
    CHECK(p.host->status().peerVisible && std::abs(p.host->status().peerDistance - 10.0f) < 0.5f);

    // Mirrors: 4 nearby peds (outlaw, horse, rider, driver) + the wagon. Not the draft horse, not the far ped.
    CHECK(p.guestGame.count(SpawnRole::Mirror) == 4);
    CHECK(p.guestGame.count(SpawnRole::Mirror, EntityKind::Vehicle) == 1);
    CHECK(p.guest->status().syncedEntities == 5);
    EntityHandle mirrorHorse = mirrorOf(p.guestGame, kHorse);
    EntityHandle mirrorWagon = mirrorOf(p.guestGame, kWagon, EntityKind::Vehicle);
    EntityHandle mirrorDutch = mirrorOf(p.guestGame, joaat("dutch"));
    CHECK(mirrorHorse && mirrorWagon && mirrorDutch);
    int mounted = 0;
    for (auto& [h, e] : p.guestGame.ents) mounted += e.ours && e.mount == mirrorHorse;
    CHECK(mounted == 1);
    CHECK(p.guestGame.ents[mirrorDutch].vehicle == mirrorWagon && p.guestGame.ents[mirrorDutch].seat == -1);

    // Guest world: population off, ambient ped deleted, story ped hidden, own horse untouched.
    CHECK(p.guestGame.populationSuppressed && !p.hostGame.populationSuppressed);
    CHECK(!p.guestGame.ents.count(stray));
    CHECK(p.guestGame.ents.count(storyPed) && p.guestGame.ents[storyPed].hidden);
    CHECK(!p.guestGame.ents[p.guestGame.ownHorse].hidden);
    CHECK(p.guestGame.world.hour == 21 && p.guestGame.world.minute == 30 && p.guestGame.world.weather == joaat("RAIN"));

    // Movement on the host is followed: the wagon drives off, the outlaw walks.
    p.perFrame = [&] {
        p.hostGame.ents[wagon].vel = {0, 6, 0};
        p.hostGame.ents[wagon].pos.y += 6 * 0.016f;
        p.hostGame.ents[outlaw].vel = {1.5f, 0, 0};
        p.hostGame.ents[outlaw].pos.x += 1.5f * 0.016f;
    };
    p.run(2000);
    p.perFrame = nullptr;
    p.hostGame.ents[wagon].vel = {};
    p.hostGame.ents[outlaw].vel = {};
    p.run(800);
    EntityHandle mirrorOutlaw = 0;
    for (auto& [h, e] : p.guestGame.ents)
        if (e.ours && e.role == SpawnRole::Mirror && e.model == kOutlaw && !e.mount) mirrorOutlaw = h;
    CHECK(mirrorOutlaw != 0);
    CHECK(distance(p.guestGame.ents[mirrorOutlaw].pos, p.hostGame.ents[outlaw].pos) < 0.5f);
    CHECK(distance(p.guestGame.ents[mirrorWagon].pos, p.hostGame.ents[wagon].pos) < 0.5f);
    CHECK(distance(p.guestGame.ents[mirrorDutch].pos, p.hostGame.ents[wagon].pos) < 0.5f);  // rides along
    CHECK(p.guestGame.ents[mirrorWagon].teleports <= 1);  // driven smoothly, not warped

    // The guest shoots the mirror: the damage lands on the host's real NPC, exactly once.
    p.guestGame.playerShoots(mirrorOutlaw, 40);
    p.run(500);
    CHECK(p.hostGame.ents[outlaw].health == 60);
    CHECK(p.guestGame.ents[mirrorOutlaw].health == 60);

    // A host NPC hurts the guest's puppet: the guest player loses that health.
    p.hostGame.ents[guestPuppet].health -= 25;
    p.run(500);
    CHECK(p.guestGame.playerHealth == 175);
    CHECK(p.hostGame.ents[guestPuppet].health == 175);

    // Friendly fire is off: the host shooting the puppet changes nothing for the guest.
    p.hostGame.playerShoots(guestPuppet, 50);
    p.run(500);
    CHECK(p.guestGame.playerHealth == 175);

    // NPC firing at the guest: the mirror fires (blanks) towards the guest's character.
    p.hostGame.ents[rider].flags = NF_Shooting | NF_InCombat;
    p.hostGame.ents[rider].target = guestPuppet;
    p.run(700);
    EntityHandle mirrorRider = 0;
    for (auto& [h, e] : p.guestGame.ents)
        if (e.ours && e.role == SpawnRole::Mirror && e.mount == mirrorHorse) mirrorRider = h;
    CHECK(mirrorRider && p.guestGame.ents[mirrorRider].shots >= 1);
    Vec3 aim = p.guestGame.ents[mirrorRider].aimPoint;
    CHECK(std::abs(aim.x - p.guestGame.player.pos.x) < 0.5f && std::abs(aim.y - p.guestGame.player.pos.y) < 0.5f);
    // ... and at the host: the mirror aims at the host's puppet.
    p.hostGame.ents[rider].target = p.hostGame.playerPed;
    p.run(700);
    aim = p.guestGame.ents[mirrorRider].aimPoint;
    Vec3 hp = p.guestGame.ents[hostPuppet].pos;
    CHECK(std::abs(aim.x - hp.x) < 0.5f && std::abs(aim.y - hp.y) < 0.5f);

    // The guest finishes the outlaw; both worlds agree it is dead and it stays a corpse.
    p.guestGame.playerShoots(mirrorOutlaw, 500);
    p.run(500);
    CHECK(p.hostGame.ents[outlaw].health == 0);
    p.run(3000);
    CHECK(p.guestGame.ents.count(mirrorOutlaw) && p.guestGame.ents[mirrorOutlaw].health == 0);

    // Removed on the host -> removed on the guest right away (explicit removal, no timeout wait).
    p.hostGame.remove(rider);
    p.hostGame.remove(horse);
    p.run(400);
    // Both live: parked in the pool for reuse, invisible, not counted as mirrors.
    CHECK(p.guestGame.ents.count(mirrorRider) && p.guestGame.ents[mirrorRider].parked);
    CHECK(p.guestGame.ents.count(mirrorHorse) && p.guestGame.ents[mirrorHorse].parked);
    CHECK(p.guestGame.find(SpawnRole::Mirror, kHorse) == 0);

    // Disconnect: everything the session created disappears and hidden entities come back.
    p.hostLink.stop();
    p.run(300);
    CHECK(p.guestGame.count(SpawnRole::Mirror) == 0);
    CHECK(p.guestGame.count(SpawnRole::Mirror, EntityKind::Vehicle) == 0);
    CHECK(p.guestGame.count(SpawnRole::PlayerPuppet) == 0);
    CHECK(!p.guestGame.ents[storyPed].hidden);
    int leftovers = 0;  // the pool is emptied too
    for (auto& [h, e] : p.guestGame.ents) leftovers += e.ours;
    CHECK(leftovers == 0);
    p.host->cleanup();
    CHECK(p.hostGame.count(SpawnRole::PlayerPuppet) == 0);
}

static void testSessionRidesAndControl() {
    Pair p;
    p.net.loss = 0.1;
    p.net.latencyMs = 30;
    p.net.jitterMs = 30;
    p.config.friendlyFire = true;
    const uint32_t kHorse = joaat("a_c_horse_morgan_bay");
    const uint32_t kWagon = joaat("wagon02x");
    const uint32_t kCart = joaat("cart01");
    p.hostGame.player.pos = {0, 0, 10};
    p.guestGame.player.pos = {3, 0, 10};
    EntityHandle hostWagon = p.hostGame.add(EntityKind::Vehicle, kWagon, {10, 10, 10});
    p.start();
    p.run(1500);
    EntityHandle guestPuppet = p.hostGame.find(SpawnRole::PlayerPuppet);
    CHECK(guestPuppet != 0);

    // The guest mounts their own horse, rides off and shoots from the saddle.
    EntityHandle guestHorse = p.guestGame.addPed(kHorse, p.guestGame.player.pos);
    p.guestGame.ownHorse = guestHorse;
    p.guestGame.playerMount = guestHorse;
    p.guestGame.player.flags = PF_Shooting;
    p.guestGame.player.weapon = joaat("weapon_repeater_carbine");
    p.perFrame = [&] {
        p.guestGame.player.pos.x += 12 * 0.016f;
        p.guestGame.player.vel = {12, 0, 0};
        p.guestGame.ents[guestHorse].pos = p.guestGame.player.pos;
    };
    p.run(1500);
    p.perFrame = nullptr;
    p.guestGame.player.vel = {};
    p.run(800);
    EntityHandle puppetHorse = p.hostGame.find(SpawnRole::PuppetMount);
    CHECK(puppetHorse != 0);
    CHECK(p.hostGame.ents[guestPuppet].mount == puppetHorse);
    CHECK(distance(p.hostGame.ents[puppetHorse].pos, p.guestGame.player.pos) < 1.0f);
    CHECK(p.hostGame.ents[guestPuppet].weapon == joaat("weapon_repeater_carbine"));
    CHECK(p.hostGame.ents[guestPuppet].shots > 0);

    // Dismount and crouch.
    p.guestGame.playerMount = 0;
    p.guestGame.player.flags = PF_Crouching;
    p.run(600);
    CHECK(p.hostGame.ents[guestPuppet].mount == 0 && p.hostGame.ents[guestPuppet].crouch);
    CHECK(distance(p.hostGame.ents[guestPuppet].pos, p.guestGame.player.pos) < 1.0f);
    p.guestGame.player.flags = 0;

    // The guest climbs onto the host's wagon (its mirror) and drives: the host's real wagon follows.
    EntityHandle mirrorWagon = mirrorOf(p.guestGame, kWagon, EntityKind::Vehicle);
    CHECK(mirrorWagon != 0);
    p.guestGame.player.pos = p.guestGame.ents[mirrorWagon].pos;
    p.guestGame.playerVehicle = mirrorWagon;
    p.guestGame.playerSeat = -1;
    p.perFrame = [&] {
        Vec3& wp = p.guestGame.ents[mirrorWagon].pos;  // the guest's game moves the wagon they drive
        wp.y += 5 * 0.016f;
        p.guestGame.player.pos = wp;
        p.guestGame.player.vel = {0, 5, 0};
    };
    p.run(2500);
    p.perFrame = nullptr;
    p.guestGame.player.vel = {};
    p.run(800);
    CHECK(p.hostGame.ents[guestPuppet].vehicle == hostWagon && p.hostGame.ents[guestPuppet].seat == -1);
    CHECK(distance(p.hostGame.ents[hostWagon].pos, p.guestGame.player.pos) < 1.0f);
    CHECK(p.hostGame.ents[hostWagon].pos.y > 20.0f);

    // Out of the wagon, into a cart of the guest's own: the host shows a puppet cart.
    p.guestGame.playerVehicle = 0;
    p.run(400);
    CHECK(p.hostGame.ents[guestPuppet].vehicle == 0);
    EntityHandle ownCart = p.guestGame.add(EntityKind::Vehicle, kCart, p.guestGame.player.pos);
    p.guestGame.playerVehicle = ownCart;
    p.guestGame.playerSeat = -1;
    p.perFrame = [&] {
        p.guestGame.player.pos.x -= 4 * 0.016f;
        p.guestGame.player.vel = {-4, 0, 0};
        p.guestGame.ents[ownCart].pos = p.guestGame.player.pos;
    };
    p.run(1500);
    p.perFrame = nullptr;
    p.guestGame.player.vel = {};
    p.run(800);
    EntityHandle puppetCart = p.hostGame.find(SpawnRole::PuppetVehicle, kCart, EntityKind::Vehicle);
    CHECK(puppetCart != 0);
    CHECK(p.hostGame.ents[guestPuppet].vehicle == puppetCart);
    CHECK(distance(p.hostGame.ents[puppetCart].pos, p.guestGame.player.pos) < 1.0f);
    p.guestGame.playerVehicle = 0;
    p.run(400);

    // Friendly fire enabled: host shoots the puppet, guest takes the damage.
    p.hostGame.playerShoots(guestPuppet, 30);
    p.run(800);
    CHECK(p.guestGame.playerHealth == 170);

    // Guest dies -> puppet dies; guest respawns -> a live puppet is back.
    p.guestGame.playerHealth = 0;
    p.run(800);
    CHECK(p.hostGame.ents[guestPuppet].health == 0);
    CHECK(p.host->status().peerDead);
    p.guestGame.playerHealth = 200;
    p.run(3000);
    EntityHandle reborn = p.hostGame.find(SpawnRole::PlayerPuppet);
    CHECK(reborn != 0 && p.hostGame.ents[reborn].health == 200);
    CHECK(p.hostGame.count(SpawnRole::PlayerPuppet) == 1);

    // Teleport to friend.
    p.hostGame.player.pos = {900, 900, 10};
    p.run(500);
    CHECK(p.guest->teleportToPeer());
    CHECK(distance(p.guestGame.player.pos, Vec3{900, 900, 10}) < 3.0f);

    // World sync switched off on the host: the guest keeps its own world.
    Pair q;
    q.config.worldSync = false;
    q.hostGame.addPed(1, {1, 1, 1});
    EntityHandle local = q.guestGame.addPed(2, {1, 1, 1});
    q.start();
    q.run(1500);
    CHECK(q.guestGame.count(SpawnRole::Mirror) == 0);
    CHECK(q.guestGame.ents.count(local) && !q.guestGame.populationSuppressed);
    CHECK(q.guestGame.count(SpawnRole::PlayerPuppet) == 1);
}

static void testTrackingAccuracy() {
    // The guest runs a figure-eight at 5 m/s over a mediocre connection. The host's puppet,
    // moved only through game tasks (plus nudges), must stay close without warping.
    Pair p;
    p.net.latencyMs = 50;
    p.net.jitterMs = 40;
    p.net.loss = 0.05;
    p.hostGame.player.pos = {0, 0, 0};
    p.guestGame.player.pos = {0, 0, 0};
    p.start();
    p.run(1000);
    float t = 0, maxErr = 0, sumErr = 0;
    int samples = 0;
    std::vector<Vec3> history;
    p.perFrame = [&] {
        t += 0.016f;
        float w = 5.0f / 15.0f;
        Vec3 pos{15 * std::sin(w * t), 7.5f * std::sin(2 * w * t), 0};
        Vec3 vel{15 * w * std::cos(w * t), 15 * w * std::cos(2 * w * t), 0};
        p.guestGame.player.pos = pos;
        p.guestGame.player.vel = vel;
        p.guestGame.player.heading = std::atan2(-vel.x, vel.y) * 180.0f / 3.14159265f;
        history.push_back(pos);
    };
    p.run(2000);
    EntityHandle puppet = p.hostGame.find(SpawnRole::PlayerPuppet);
    CHECK(puppet != 0);
    int teleportsBefore = p.hostGame.ents[puppet].teleports;
    int tasksBefore = p.hostGame.ents[puppet].tasks;
    for (int i = 0; i < 600; ++i) {
        p.run(16);
        // Compare against where the guest was one smoothing delay + latency ago.
        int lag = (p.host->status().bufferMs + 100) / 16;  // + network and server hop
        if (static_cast<int>(history.size()) <= lag) continue;
        float err = distance(p.hostGame.ents[puppet].pos, history[history.size() - 1 - lag]);
        maxErr = std::max(maxErr, err);
        sumErr += err;
        ++samples;
    }
    int warps = p.hostGame.ents[puppet].teleports - teleportsBefore;
    int tasks = p.hostGame.ents[puppet].tasks - tasksBefore;
    std::printf("  tracking: max error %.2f m, mean %.2f m, warps %d, buffer %d ms, tasks %d over 10 s\n", maxErr,
                sumErr / samples, warps, p.host->status().bufferMs, tasks);
    CHECK(maxErr < 1.2f);
    CHECK(sumErr / samples < 0.5f);
    CHECK(warps == 0);
    CHECK(tasks < 10 * 8);  // re-tasked a few times per second, not every frame (60 fps)
}

static void testBandwidthAndLod() {
    // 20 NPCs walking next to the guest, 60 idle ones further out.
    Pair p;
    p.config.maxPeds = 80;
    p.config.npcRadius = 200;
    p.hostGame.player.pos = {0, 0, 0};
    p.guestGame.player.pos = {5, 0, 0};
    std::vector<EntityHandle> walkers, idle;
    for (int i = 0; i < 20; ++i) walkers.push_back(p.hostGame.addPed(joaat("walker"), {float(i % 5) * 3, float(i / 5) * 3, 0}));
    for (int i = 0; i < 60; ++i) idle.push_back(p.hostGame.addPed(joaat("idle"), {100.0f + i, 50, 0}));
    p.perFrame = [&] {
        for (EntityHandle h : walkers) {
            p.hostGame.ents[h].pos.x += 1.4f * 0.016f;
            p.hostGame.ents[h].vel = {1.4f, 0, 0};
        }
    };
    p.start();
    p.run(4000);
    int readsBefore = p.hostGame.readCalls;
    p.run(3000);
    float readsPerSecond = (p.hostGame.readCalls - readsBefore) / 3.0f;
    uint32_t rate = p.hostLink.sendRate();
    // Old v1 protocol: 80 NPCs x 30 bytes x 10 Hz = 24 KB/s for the world alone.
    std::printf("  bandwidth: host sends %.1f KB/s for 80 NPCs (+ player), %.0f entity reads/s\n", rate / 1024.0f,
                readsPerSecond);
    CHECK(p.guestGame.count(SpawnRole::Mirror) == 80);
    CHECK(rate < 9000);
    // Near movers refresh at the full world rate; idle far ones are rarely read from the game.
    CHECK(readsPerSecond < 20 * 15.5f + 60 * 4.0f);
    for (EntityHandle h : walkers) {
        EntityHandle m = 0;
        float best = 1e9f;
        for (auto& [k, e] : p.guestGame.ents)
            if (e.ours && e.model == joaat("walker") && distance(e.pos, p.hostGame.ents[h].pos) < best) {
                best = distance(e.pos, p.hostGame.ents[h].pos);
                m = k;
            }
        CHECK(m && best < 0.6f);
    }
}


static void testPoolHysteresisAndLod() {
    Pair p;
    p.config.npcRadius = 60;
    p.hostGame.player.pos = {0, 0, 0};
    p.guestGame.player.pos = {0, 0, 0};
    const uint32_t kCow = joaat("a_c_cow");
    // A cow wandering back and forth across the stream edge must not pop in and out.
    EntityHandle edge = p.hostGame.addPed(kCow, {58, 0, 0});
    float phase = 0;
    p.perFrame = [&] {
        phase += 0.016f;
        p.hostGame.ents[edge].pos.x = 54.0f + 12.0f * std::sin(phase * 2.0f);  // 42..66 m, radius 60
    };
    p.start();
    p.run(1500);
    CHECK(p.guestGame.find(SpawnRole::Mirror, kCow) != 0);
    int spawnsBefore = p.guestGame.spawns;
    p.run(6000);
    CHECK(p.guestGame.spawns == spawnsBefore);  // no churn at the boundary
    CHECK(p.guestGame.find(SpawnRole::Mirror, kCow) != 0);
    p.perFrame = nullptr;

    // Leaves for real -> parked; a cow appears elsewhere -> the parked one is reused (no spawn).
    p.hostGame.ents[edge].pos = {500, 0, 0};
    p.run(500);
    CHECK(p.guestGame.find(SpawnRole::Mirror, kCow) == 0);
    int spawns = p.guestGame.spawns;
    EntityHandle other = p.hostGame.addPed(kCow, {10, 10, 0});
    p.hostGame.ents[other].outfit = 0xABCDu;
    p.run(500);
    EntityHandle reused = p.guestGame.find(SpawnRole::Mirror, kCow);
    CHECK(reused != 0 && p.guestGame.spawns == spawns);
    CHECK(distance(p.guestGame.ents[reused].pos, Vec3{10, 10, 0}) < 0.5f);
    CHECK(p.guestGame.ents[reused].outfit == 0xABCDu);  // the host's look, not a random one
    // The outfit changes on the host (story script dresses an NPC) -> follows.
    p.hostGame.ents[other].outfit = 0x1234u;
    p.run(1500);
    CHECK(p.guestGame.ents[reused].outfit == 0x1234u);
    // Unused pool entries are freed after a while.
    p.hostGame.remove(other);
    p.run(12000);
    int parked = 0;
    for (auto& [h, e] : p.guestGame.ents) parked += e.parked;
    CHECK(parked == 0);

    // CPU level of detail: a far walker is driven far less often than a near one, yet both stay on track.
    Pair q;
    q.config.npcRadius = 200;
    q.hostGame.player.pos = {0, 0, 0};
    q.guestGame.player.pos = {0, 0, 0};
    EntityHandle nearWalker = q.hostGame.addPed(joaat("near"), {5, 0, 0});
    EntityHandle farWalker = q.hostGame.addPed(joaat("far"), {120, 0, 0});
    q.perFrame = [&] {
        for (EntityHandle h : {nearWalker, farWalker}) {
            q.hostGame.ents[h].pos.y += 1.5f * 0.016f;
            q.hostGame.ents[h].vel = {0, 1.5f, 0};
        }
    };
    q.start();
    q.run(2000);
    EntityHandle mn = q.guestGame.find(SpawnRole::Mirror, joaat("near"));
    EntityHandle mf = q.guestGame.find(SpawnRole::Mirror, joaat("far"));
    CHECK(mn && mf);
    int queriesNear = q.guestGame.ents[mn].queries, queriesFar = q.guestGame.ents[mf].queries;
    q.run(4000);
    int workNear = q.guestGame.ents[mn].queries - queriesNear;
    int workFar = q.guestGame.ents[mf].queries - queriesFar;
    std::printf("  cpu lod: near mirror %d game queries, far mirror %d (4 s)\n", workNear, workFar);
    CHECK(workFar * 3 <= workNear);
    CHECK(distance(q.guestGame.ents[mf].pos, q.hostGame.ents[farWalker].pos) < 1.0f);
    CHECK(distance(q.guestGame.ents[mn].pos, q.hostGame.ents[nearWalker].pos) < 1.0f);
}

static void testCutsceneRegroupAndAppearance() {
    Pair p;
    p.serverConfig.storyMode = StoryMode::HostWorld;
    p.hostGame.player.pos = {0, 0, 0};
    p.guestGame.player.pos = {800, 0, 0};
    p.guestGame.playerOutfit = 0x5555u;
    p.start();
    p.run(1500);
    EntityHandle guestPuppet = p.hostGame.find(SpawnRole::PlayerPuppet);
    CHECK(guestPuppet && p.hostGame.ents[guestPuppet].outfit == 0x5555u);  // the guest's look
    p.guestGame.playerOutfit = 0x6666u;  // changes clothes
    p.run(2500);
    CHECK(p.hostGame.ents[guestPuppet].outfit == 0x6666u);

    // The host starts a mission far away: the guest is brought over and told why.
    p.hostGame.player.flags = PF_InMission;
    p.run(500);
    CHECK(distance(p.guestGame.player.pos, p.hostGame.player.pos) < 3.0f);
    std::string text;
    bool sawMission = false;
    while (p.guest->popNotice(text)) sawMission = sawMission || text.find("mission") != std::string::npos;
    CHECK(sawMission);

    // Cutscene: guest protected meanwhile; host rides off during it; regroup when it ends.
    p.hostGame.player.flags = PF_InMission | PF_Cutscene;
    p.run(400);
    CHECK(p.guestGame.playerProtected && p.guest->status().peerInCutscene);
    p.hostGame.player.pos = {0, 600, 0};
    p.run(400);
    CHECK(distance(p.guestGame.player.pos, p.hostGame.player.pos) > 100.0f);  // no teleport mid-scene
    p.hostGame.player.flags = PF_InMission;
    p.run(500);
    CHECK(!p.guestGame.playerProtected);
    CHECK(distance(p.guestGame.player.pos, p.hostGame.player.pos) < 3.0f);

    // A standing player is sent at 5 Hz, a moving one at 30 Hz.
    p.hostGame.player.flags = 0;
    p.run(1500);
    uint32_t idleRate = p.hostLink.sendRate();
    p.perFrame = [&] {
        p.hostGame.player.pos.x += 3 * 0.016f;
        p.hostGame.player.vel = {3, 0, 0};
    };
    p.run(2500);
    uint32_t movingRate = p.hostLink.sendRate();
    p.perFrame = nullptr;
    std::printf("  idle player: host sends %u B/s standing, %u B/s moving\n", idleRate, movingRate);
    CHECK(idleRate * 2 < movingRate);
}




static bool sawNotice(CoopSession& s, const char* needle) {
    std::string text;
    bool seen = false;
    while (s.popNotice(text)) seen = seen || text.find(needle) != std::string::npos;
    return seen;
}

static void testSharedStoryMissionEntities() {
    // Both run the same mission: each game has its own copy of the gang and the enemies.
    Pair p;
    const uint32_t kScript = joaat("mission_script_x");
    const uint32_t kOutlaw = joaat("g_m_m_uniduster_01"), kDutch = joaat("cs_dutch");
    p.hostGame.player.pos = {0, 0, 0};
    p.guestGame.player.pos = {2, 0, 0};
    auto missionPed = [&](FakeGame& g, uint32_t model, Vec3 pos) {
        EntityHandle h = g.addPed(model, pos);
        g.ents[h].mission = true;
        g.ents[h].script = kScript;
        return h;
    };
    EntityHandle hostEnemy = missionPed(p.hostGame, kOutlaw, {20, 0, 0});
    EntityHandle hostEnemy2 = missionPed(p.hostGame, kOutlaw, {22, 5, 0});
    EntityHandle hostDutch = missionPed(p.hostGame, kDutch, {3, 3, 0});
    EntityHandle hostOnly = missionPed(p.hostGame, joaat("cs_hosea"), {6, 6, 0});
    // The guest's copies spawned by its own mission script, slightly elsewhere (their AI differs a bit).
    EntityHandle guestEnemy = missionPed(p.guestGame, kOutlaw, {21, 1, 0});
    EntityHandle guestEnemy2 = missionPed(p.guestGame, kOutlaw, {23, 6, 0});
    EntityHandle guestDutch = missionPed(p.guestGame, kDutch, {4, 2, 0});
    EntityHandle guestOnly = missionPed(p.guestGame, joaat("cs_bill"), {9, 9, 0});
    p.guestGame.ents[guestOnly].script = joaat("other_script");
    p.start();
    p.run(1200);

    // Linked, not duplicated: no stand-ins for the enemies or Dutch, the guest's own copies are used.
    CHECK(p.guestGame.find(SpawnRole::Mirror, kOutlaw) == 0 && p.guestGame.find(SpawnRole::Mirror, kDutch) == 0);
    CHECK(p.guestGame.ents[guestEnemy].harmless && p.guestGame.ents[guestDutch].harmless);
    CHECK(p.guestGame.find(SpawnRole::Mirror, joaat("cs_hosea")) == 0);  // still waiting for a twin
    p.run(2500);
    CHECK(p.guestGame.find(SpawnRole::Mirror, joaat("cs_hosea")) != 0);  // none came: a stand-in is shown
    // Copies are kept where the host's are; the guest-only ped is left alone.
    CHECK(distance(p.guestGame.ents[guestEnemy].pos, p.hostGame.ents[hostEnemy].pos) < 0.8f);
    CHECK(p.guestGame.ents.count(guestOnly) && !p.guestGame.ents[guestOnly].hidden && !p.guestGame.ents[guestOnly].harmless);

    // The host kills an enemy: the guest's copy dies too (the guest's mission counts the kill).
    p.hostGame.playerShoots(hostEnemy, 500);
    p.run(500);
    CHECK(p.guestGame.ents.count(guestEnemy) && p.guestGame.ents[guestEnemy].health == 0);
    // The guest shoots its copy of the second enemy: the host's real one takes the hit.
    p.guestGame.playerShoots(guestEnemy2, 35);
    p.run(500);
    CHECK(p.hostGame.ents[hostEnemy2].health == 65);
    CHECK(p.guestGame.ents[guestEnemy2].health == 65);
    // A copy that dies only locally (killed by local AI, not by the guest) is not counted: it comes back.
    p.guestGame.ents[guestDutch].health = 0;
    p.run(2500);
    CHECK(p.guestGame.ents[guestDutch].health > 0);
    (void)hostDutch;
    (void)hostOnly;

    // The mission ends on the host: its entities go away -> the guest's copies are released, not deleted.
    p.hostGame.remove(hostEnemy2);
    p.run(500);
    CHECK(p.guestGame.ents.count(guestEnemy2) && !p.guestGame.ents[guestEnemy2].harmless);
}

static void testSharedStoryFlow() {
    Pair p;
    MemorySaveStore hostSaves;
    std::mt19937 rng(3);
    for (int i = 0; i < 300000; ++i) hostSaves.latest.push_back(static_cast<uint8_t>(rng()));
    p.hostGame.player.pos = {0, 0, 0};
    p.guestGame.player.pos = {100, 0, 0};
    p.net.loss = 0.05;
    p.net.latencyMs = 40;
    p.hostP.saves = hostSaves;
    p.hostP.saves.stamp = 1;
    p.start();
    p.run(5000);

    // The host's save is put on the server; one key and the guest has it.
    CHECK(p.storage.saves["default"].size() == 1);
    CHECK(p.guest->requestCampaignSave());
    p.run(200);
    CHECK(p.guest->status().saveProgress >= 0);
    p.run(8000);
    CHECK(p.guestP.saves.written.size() == 1 && p.guestP.saves.written[0] == hostSaves.latest);
    CHECK(sawNotice(*p.guest, "SRDR30007"));
    CHECK(p.hostGame.find(SpawnRole::PlayerPuppet) != 0);  // the game kept running meanwhile

    // The host starts a mission: the guest is moved to the start and told to start it too.
    p.hostGame.player.flags = PF_InMission | PF_Cutscene;
    p.hostGame.missionScript = joaat("mission_a");
    p.run(400);
    CHECK(distance(p.guestGame.player.pos, Vec3{0, 0, 0}) < 3.0f);
    CHECK(sawNotice(*p.guest, "start it here too"));
    // The host's scene ends before the guest has even started: the host waits.
    p.run(1000);
    p.hostGame.player.flags = PF_InMission;
    p.run(300);
    CHECK(p.hostGame.held && p.host->status().holding);
    // The guest starts the mission and watches its own cutscene; the host keeps waiting.
    p.guestGame.player.flags = PF_InMission | PF_Cutscene;
    p.guestGame.missionScript = joaat("mission_a");
    p.run(3000);
    CHECK(p.hostGame.held);
    CHECK((p.guest->status().peerInCutscene) == false);
    // The guest's scene ends: both continue together (after a short settle).
    p.guestGame.player.flags = PF_InMission;
    p.run(2500);
    CHECK(!p.hostGame.held && !p.guestGame.held);
    CHECK(!p.host->status().missionMismatch);

    // Both in a scene again; the guest finishes first this time and waits for the host.
    p.hostGame.player.flags = p.guestGame.player.flags = PF_InMission | PF_Cutscene;
    p.run(1000);
    p.guestGame.player.flags = PF_InMission;
    p.run(1000);
    CHECK(p.guestGame.held && !p.hostGame.held);
    p.hostGame.player.flags = PF_InMission;
    p.run(2500);
    CHECK(!p.guestGame.held);

    // Shared failure: the guest dies in the mission -> the host's mission fails too (no echo back).
    p.guestGame.playerHealth = 0;
    p.run(800);
    CHECK(p.hostGame.playerHealth == 0);
    CHECK(sawNotice(*p.host, "restarts"));
    p.hostGame.playerHealth = 200;
    p.guestGame.playerHealth = 200;
    p.run(1000);
    CHECK(p.guestGame.playerHealth == 200 && p.hostGame.playerHealth == 200);

    // The host dies while the guest watches a scene: the guest's mission fails once the scene is over.
    p.guestGame.player.flags = PF_InMission | PF_Cutscene;
    p.run(2500);
    p.hostGame.playerHealth = 0;
    p.run(1500);
    CHECK(p.guestGame.playerHealth == 200);  // not in the middle of the scene
    p.hostGame.playerHealth = 200;
    p.guestGame.player.flags = PF_InMission;
    p.run(500);
    CHECK(p.guestGame.playerHealth == 0);
    p.guestGame.playerHealth = 200;
    p.run(1000);
    CHECK(p.hostGame.playerHealth == 200);  // no echo back to the host

    // Different missions are reported.
    p.guestGame.missionScript = joaat("mission_b");
    p.run(2500);
    CHECK(p.host->status().missionMismatch && p.guest->status().missionMismatch);
    CHECK(sawNotice(*p.guest, "different missions"));
    // ... and nobody waits for a scene of another mission.
    p.hostGame.player.flags = p.guestGame.player.flags = PF_InMission | PF_Cutscene;
    p.run(1500);
    p.hostGame.player.flags = PF_InMission;
    p.run(600);
    CHECK(!p.hostGame.held);
    p.guestGame.player.flags = PF_InMission;
    p.guestGame.missionScript = joaat("mission_a");
    p.run(1500);

    // Waypoints travel both ways.
    p.hostGame.player.flags = p.guestGame.player.flags = 0;
    p.hostGame.waypoint = true;
    p.hostGame.waypointPos = {1234, -567, 0};
    p.run(1200);
    CHECK(p.guestGame.friendWaypoint && distance(p.guestGame.friendWaypointPos, Vec3{1234, -567, 0}) < 0.01f);
    p.hostGame.waypoint = false;
    p.run(1200);
    CHECK(!p.guestGame.friendWaypoint);
    p.guestGame.waypoint = true;
    p.guestGame.waypointPos = {10, 20, 0};
    p.run(1200);
    CHECK(p.hostGame.friendWaypoint && distance(p.hostGame.friendWaypointPos, Vec3{10, 20, 0}) < 0.01f);

    // Disconnect while waiting: nothing stays frozen.
    p.hostGame.player.flags = p.guestGame.player.flags = PF_InMission | PF_Cutscene;
    p.run(500);
    p.guestGame.player.flags = PF_InMission;
    p.run(500);
    CHECK(p.guestGame.held);
    p.hostLink.stop();
    p.run(300);
    CHECK(!p.guestGame.held && !p.guestGame.playerProtected && !p.guestGame.friendWaypoint);

}

static void testSpectatorCamera() {
    // Host world: the guest watches the host's scene through the host's camera.
    Pair p;
    p.serverConfig.storyMode = StoryMode::HostWorld;
    p.hostGame.player.pos = {0, 0, 0};
    p.guestGame.player.pos = {50, 0, 0};
    p.start();
    p.run(1500);
    p.hostGame.player.flags = PF_InMission | PF_Cutscene;
    p.hostGame.cameraValid = true;
    float t = 0;
    p.perFrame = [&] {
        t += 0.016f;
        p.hostGame.camera.pos = {10 + 2 * t, 5, 3};
        p.hostGame.camera.rot = {-10, 0, 90 + 10 * t};
        p.hostGame.camera.fov = 40;
    };
    p.run(2000);
    CHECK(p.guestGame.spectating && p.guest->status().spectating);
    CHECK(p.guestGame.playerProtected);
    Vec3 hostCam = p.hostGame.camera.pos;
    float lag = distance(p.guestGame.spectateCam.pos, hostCam);
    std::printf("  spectator: camera %.2f m behind the host's (moving 2 m/s)\n", lag);
    CHECK(lag < 0.6f);
    CHECK(std::abs(p.guestGame.spectateCam.fov - 40) < 0.1f);
    CHECK(std::abs(angleDelta(p.guestGame.spectateCam.rot.z, p.hostGame.camera.rot.z)) < 3.0f);
    p.perFrame = nullptr;
    p.hostGame.player.flags = PF_InMission;
    p.hostGame.cameraValid = false;
    p.run(800);
    CHECK(!p.guestGame.spectating && !p.guestGame.playerProtected);
    // The scene ended within regroup distance (50 m < 150 m): the guest stays where they are.
    CHECK(distance(p.guestGame.player.pos, Vec3{50, 0, 0}) < 0.5f);
}


static void dropPlayer(Cluster& c, Player& p) {
    p.link.stop();
    c.players.erase(std::remove(c.players.begin(), c.players.end(), &p), c.players.end());
}

static bool journalHas(Cluster& c, const char* campaign, const char* needle) {
    for (const auto& line : c.storage.journals[campaign])
        if (line.find(needle) != std::string::npos) return true;
    return false;
}

static void testServerAdmission() {
    Cluster c;
    c.serverConfig.password = "secret";
    c.serverConfig.maxPlayers = 2;
    Player& a = c.join("A", true, "alpha", "secret");
    Player& wrong = c.join("B", false, "alpha", "nope");
    Player& d = c.join("D", false, "beta", "secret");
    c.run(1500);
    CHECK(a.session->status().welcomed && a.session->status().worldOwner);
    CHECK(!wrong.session->status().welcomed && wrong.session->status().denied == "wrong password");
    CHECK(d.session->status().worldOwner);  // owns the world of its own campaign
    CHECK(a.session->status().players == 1 && d.session->status().players == 1);
    CHECK(a.game.count(SpawnRole::PlayerPuppet) == 0);  // campaigns do not see each other

    Player& e = c.join("E", false, "alpha", "secret");
    c.run(800);
    Player& f = c.join("F", false, "alpha", "secret");
    c.run(1500);
    CHECK(e.session->status().welcomed && e.session->status().players == 2 && !e.session->status().worldOwner);
    CHECK(e.session->status().ownerName == "A");
    CHECK(!f.session->status().welcomed && f.session->status().denied == "campaign is full");
    // The same name twice in one campaign gets told apart.
    c.join("D", false, "beta", "secret");
    c.run(1500);
    EntityHandle other = d.game.find(SpawnRole::PlayerPuppet);
    CHECK(other && d.game.ents[other].label == "D 2");
}

static void testServerThreePlayers() {
    const uint32_t kOutlaw = joaat("g_m_m_uniduster_01"), kCow = joaat("a_c_cow");
    Cluster c;
    Player& owner = c.join("Owner", true);
    c.run(200);
    EntityHandle outlaw = owner.game.addPed(kOutlaw, {10, 0, 0});
    owner.game.addPed(kCow, {-10, 5, 0});
    Player& b = c.join("B");
    b.game.player.pos = {5, 0, 0};
    c.run(2000);
    // A late joiner sees the world at once, from the server's copy (an idle cow only sends a keyframe a second).
    Player& late = c.join("Late");
    late.game.player.pos = {3, 3, 0};
    c.run(400);
    CHECK(mirrorOf(late.game, kCow) != 0 && mirrorOf(late.game, kOutlaw) != 0);
    c.run(1500);
    CHECK(owner.game.count(SpawnRole::PlayerPuppet) == 2);
    CHECK(b.game.count(SpawnRole::PlayerPuppet) == 2 && late.game.count(SpawnRole::PlayerPuppet) == 2);
    CHECK(b.session->status().players == 3);

    // Late shoots the outlaw: the owner's real one takes it, and B's copy shows it.
    late.game.playerShoots(mirrorOf(late.game, kOutlaw), 30);
    c.run(600);
    CHECK(owner.game.ents[outlaw].health == 70);
    CHECK(b.game.ents[mirrorOf(b.game, kOutlaw)].health == 70);
    // The owner's NPCs hurt Late's character: only Late loses health.
    EntityHandle latePuppet = owner.game.find(SpawnRole::PlayerPuppet, joaat("player_Late"));
    CHECK(latePuppet != 0);
    owner.game.ents[latePuppet].health -= 20;
    c.run(600);
    CHECK(late.game.playerHealth == 180 && b.game.playerHealth == 200 && owner.game.playerHealth == 200);
    // B sees Late move.
    late.game.player.pos = {20, 20, 0};
    c.run(1000);
    EntityHandle lateOnB = b.game.find(SpawnRole::PlayerPuppet, joaat("player_Late"));
    CHECK(lateOnB && distance(b.game.ents[lateOnB].pos, Vec3{20, 20, 0}) < 1.0f);
    // Late leaves: gone for everyone.
    dropPlayer(c, late);
    c.run(600);
    CHECK(b.game.count(SpawnRole::PlayerPuppet) == 1 && owner.game.count(SpawnRole::PlayerPuppet) == 1);
}

static void testServerWorldHandover() {
    const uint32_t kOutlaw = joaat("g_m_m_uniduster_01"), kCow = joaat("a_c_cow");
    Cluster c;
    c.serverConfig.storyMode = StoryMode::HostWorld;
    Player& a = c.join("A", true);
    c.run(200);
    a.game.addPed(kOutlaw, {5, 0, 0});
    Player& b = c.join("B");
    c.run(200);
    Player& d = c.join("C");
    EntityHandle bStory = b.game.addPed(kCow, {6, 0, 0});
    b.game.ents[bStory].mission = true;  // B's own story ped: hidden while B visits A's world
    c.run(2000);
    CHECK(mirrorOf(b.game, kOutlaw) && mirrorOf(d.game, kOutlaw));
    CHECK(b.game.ents[bStory].hidden);

    // The world owner leaves: B (next in) takes over the world, everybody follows.
    dropPlayer(c, a);
    c.run(2500);
    CHECK(b.session->status().worldOwner && d.session->status().ownerName == "B");
    CHECK(!mirrorOf(b.game, kOutlaw) && !b.game.ents[bStory].hidden);  // B's own world is back
    CHECK(!mirrorOf(d.game, kOutlaw) && mirrorOf(d.game, kCow));        // C now sees B's world
    CHECK(journalHas(c, "default", "B now owns the world"));
}

static void testServerStoresEverything() {
    Cluster c;
    Player& a = c.join("A", true);
    for (int i = 0; i < 5000; ++i) a.saves.latest.push_back(static_cast<uint8_t>(i * 7));
    a.saves.stamp = 7;
    c.run(2500);
    CHECK(c.storage.saves["default"].size() == 1);  // a new campaign starts from the owner's save
    // The owner saves again in the game: the server gets it by itself.
    a.saves.latest[0] = 99;
    a.saves.stamp = 8;
    c.run(6000);
    CHECK(c.storage.saves["default"].size() == 2 && c.storage.saves["default"].back()[0] == 99);
    a.game.player.flags = PF_InMission;
    c.run(500);
    a.game.player.flags = 0;
    c.run(500);
    a.game.player.pos = {123, 45, 6};
    c.run(300);

    // The owner goes offline; someone joins later and still gets the campaign save.
    dropPlayer(c, a);
    c.run(500);
    Player& b = c.join("B");
    c.run(1000);
    CHECK(b.session->status().campaignHasSave);
    CHECK(b.session->requestCampaignSave());
    c.run(3000);
    CHECK(b.saves.written.size() == 1 && b.saves.written[0] == a.saves.latest);

    // Even a restarted server still has it: everything lives in the storage.
    dropPlayer(c, b);
    c.run(500);
    c.server = std::make_unique<Server>(*c.serverSock, c.serverConfig, c.storage);
    Player& d = c.join("D");
    c.run(1000);
    CHECK(d.session->requestCampaignSave());
    c.run(3000);
    CHECK(d.saves.written.size() == 1 && d.saves.written[0][0] == 99);
    CHECK(journalHas(c, "default", "A joined") && journalHas(c, "default", "A started a mission") &&
          journalHas(c, "default", "A left"));
    CHECK(c.storage.players["default"].count("A") && distance(c.storage.players["default"]["A"], Vec3{123, 45, 6}) < 0.1f);
}

static void testServerThreePlayerStory() {
    Cluster c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    c.run(1500);
    for (Player* p : {&a, &b, &d}) p->game.player.flags = PF_InMission | PF_Cutscene;
    c.run(1000);
    // Everybody waits for the slowest watcher.
    a.game.player.flags = PF_InMission;
    c.run(600);
    CHECK(a.game.held && !b.game.held);
    b.game.player.flags = PF_InMission;
    c.run(600);
    CHECK(a.game.held && b.game.held);
    d.game.player.flags = PF_InMission;
    c.run(2500);
    CHECK(!a.game.held && !b.game.held && !d.game.held);
    // One dies: the mission restarts for the other two.
    d.game.playerHealth = 0;
    c.run(800);
    CHECK(a.game.playerHealth == 0 && b.game.playerHealth == 0);
    for (Player* p : {&a, &b, &d}) p->game.playerHealth = 200;
    c.run(1500);
    CHECK(a.game.playerHealth == 200 && b.game.playerHealth == 200 && d.game.playerHealth == 200);  // no echo
}


static void testFileStorage() {
    std::string root = "coop-test-storage";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    {
        FileStorage fs(root, 3);
        std::vector<uint8_t> data;
        CHECK(!fs.loadSave("My Campaign!", data));
        for (uint8_t i = 1; i <= 5; ++i) CHECK(fs.storeSave("My Campaign!", std::vector<uint8_t>(100, i), "A"));
        CHECK(fs.loadSave("My Campaign!", data) && data.size() == 100 && data[0] == 5);  // the newest
        fs.journal("My Campaign!", "hello");
        fs.storePlayer("My Campaign!", "Arthur", {1, 2, 3});
        fs.storePlayer("My Campaign!", "John", {4, 5, 6});
        fs.storePlayer("My Campaign!", "Arthur", {7, 8, 9});
    }
    std::filesystem::path dir = std::filesystem::path(root) / "mycampaign";
    size_t saves = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir / "saves")) saves += e.path().extension() == ".sav";
    CHECK(saves == 3);  // history is capped
    std::ifstream players(dir / "players.ini");
    std::string all((std::istreambuf_iterator<char>(players)), std::istreambuf_iterator<char>());
    CHECK(all.find("Arthur=") != std::string::npos && all.find("7.0 8.0 9.0") != std::string::npos &&
          all.find("John=") != std::string::npos);
    std::ifstream journal(dir / "journal.log");
    std::string log((std::istreambuf_iterator<char>(journal)), std::istreambuf_iterator<char>());
    CHECK(log.find("hello") != std::string::npos && log.find("save stored") != std::string::npos);
    CHECK(sanitizeCampaign("../../etc") == "etc" && sanitizeCampaign("Blackwater_1") == "blackwater_1");
    std::filesystem::remove_all(root, ec);
}

static void testUdpLoopback() {
    UdpSocket a, b;
    std::string error;
    CHECK(a.open(0, &error));
    CHECK(b.open(0, &error));
    Endpoint toA;
    CHECK(UdpSocket::resolve("127.0.0.1:" + std::to_string(a.localPort()), 1, toA));
    CHECK(toA.ip == 0x7F000001u && toA.port == a.localPort());
    const uint8_t msg[] = {1, 2, 3};
    CHECK(b.sendTo(toA, msg, sizeof(msg)));
    CHECK(a.waitReadable(1000));
    uint8_t buf[16];
    Endpoint from;
    CHECK(a.recvFrom(from, buf, sizeof(buf)) == 3);
    CHECK(from.port == b.localPort() && buf[2] == 3);
    CHECK(a.recvFrom(from, buf, sizeof(buf)) == -1);
    Endpoint def;
    CHECK(UdpSocket::resolve("localhost", 27960, def) && def.port == 27960);
    CHECK(!UdpSocket::resolve("127.0.0.1:99999", 1, def));
}

int main() {
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"protocol", testProtocol},
        {"interpolation: smooth under jitter and loss", testInterpolationSmooth},
        {"ini", testIni},
        {"link: reliable channel under 35% loss + jitter", testLinkReliableUnderLoss},
        {"link: reconnect, bye, role clash", testLinkReconnectAndRoleClash},
        {"session: shared world, vehicles, damage, removal", testSessionSharedWorld},
        {"session: rides, vehicles, control transfer", testSessionRidesAndControl},
        {"session: tracking accuracy", testTrackingAccuracy},
        {"session: bandwidth and level of detail", testBandwidthAndLod},
        {"session: pool, hysteresis, cpu lod, outfits", testPoolHysteresisAndLod},
        {"session: cutscenes, regroup, appearance, idle rate", testCutsceneRegroupAndAppearance},
        {"story: linked mission entities", testSharedStoryMissionEntities},
        {"story: save copy, mission start, cutscene barrier, shared fail, waypoints", testSharedStoryFlow},
        {"story: spectator camera (host world)", testSpectatorCamera},
        {"server: passwords, campaigns, capacity, names", testServerAdmission},
        {"server: three players, late join from the server's world copy", testServerThreePlayers},
        {"server: world handed over when the owner leaves", testServerWorldHandover},
        {"server: saves, journal and players stored (also across restarts)", testServerStoresEverything},
        {"server: cutscene barrier and shared failure with three players", testServerThreePlayerStory},
        {"server: campaign files on disk", testFileStorage},
        {"udp loopback", testUdpLoopback},
    };
    for (const Test& t : tests) {
        int before = g_failures;
        t.fn();
        std::printf("[%s] %s\n", g_failures == before ? " ok " : "FAIL", t.name);
    }
    std::printf("%s\n", g_failures ? "TESTS FAILED" : "all tests passed");
    return g_failures ? 1 : 0;
}
