// Shared test harness: a simulated game world (FakeGame), an in-memory network with latency,
// jitter and loss, and clusters of a server plus players. Include it from any tests/*.cpp file and
// register tests with COOP_TEST; every tests/*.cpp file is built into coop_tests.
#pragma once
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

inline int g_failures = 0;

// Test registry: COOP_TEST(fn, "name") { ... } in any file adds a test.
struct TestCase {
    const char* name;
    void (*fn)();
};
inline std::vector<TestCase>& testRegistry() {
    static std::vector<TestCase> tests;
    return tests;
}
struct RegisterTest {
    RegisterTest(const char* name, void (*fn)()) { testRegistry().push_back({name, fn}); }
};
#define COOP_TEST(fn, name) \
    static void fn();       \
    static RegisterTest fn##_registration(name, fn); \
    static void fn()

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
            bytesSent += size;
            ++datagramsSent;
            if (net_.tap) net_.tap(self_, to, data, size);
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
        uint64_t bytesSent = 0, datagramsSent = 0;  // everything this socket put on the network

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
    // Sees every datagram sent (before loss); for tests that capture or count traffic.
    std::function<void(const Endpoint& from, const Endpoint& to, const uint8_t* data, size_t size)> tap;
    // Delivers a datagram with any sender address: a forged source, or a mutated copy of real traffic.
    void inject(const Endpoint& from, const Endpoint& to, const uint8_t* data, size_t size) {
        auto it = sockets_.find(to);
        if (it != sockets_.end()) it->second->queue_.push_back({now, from, std::vector<uint8_t>(data, data + size)});
    }

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
        Appearance look;      // what applyAppearance() put on
        int looks = 0;        // applyAppearance() calls
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
    int nearbyCalls = 0, existsCalls = 0;  // game queries, for the CPU benchmarks
    int adoptions = 0;
    int spawns = 0, removals = 0, prefetches = 0;
    uint32_t playerOutfit = 0;
    // the local character: its model, what it wears (shop items), its outfit presets
    uint32_t localModelHash = 0;
    std::vector<ShopItem> playerItems;
    int presetsOfModel = 4;
    int playerPreset = -1;
    int modelSwitches = 0;
    int switchFailures = 0;                  // setLocalModel() fails this many times first
    std::vector<uint32_t> missingModels;     // models the game does not have
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
        // What the player sat on was deleted: the game drops them where they are.
        if (playerMount && !ents.count(playerMount)) playerMount = 0;
        if (playerVehicle && !ents.count(playerVehicle)) playerVehicle = 0;
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
    uint32_t localModel() override { return localModelHash; }
    bool setLocalModel(uint32_t model) override {
        if (!modelExists(model)) return false;
        if (switchFailures > 0) {
            --switchFailures;
            return false;
        }
        // The game gives the player a new ped of that model, dressed in its defaults.
        ents.erase(playerPed);
        playerPed = add(EntityKind::Ped, model, player.pos);
        localModelHash = model;
        playerItems.clear();
        playerOutfit = 0;
        playerPreset = -1;
        ++modelSwitches;
        return true;
    }
    bool modelExists(uint32_t model) override {
        return std::find(missingModels.begin(), missingModels.end(), model) == missingModels.end();
    }
    bool readAppearance(EntityHandle ped, Appearance& out) override {
        out = Appearance{};
        if (ped == playerPed) {
            out.model = localModelHash;
            out.outfit = playerOutfit;
            out.items = playerItems;
            return true;
        }
        auto it = ents.find(ped);
        if (it == ents.end()) return false;
        out = it->second.look;
        out.model = it->second.model;
        return true;
    }
    void applyAppearance(EntityHandle ped, const Appearance& look) override {
        if (ped == playerPed) {
            playerItems = look.items;
            playerOutfit = look.outfit;
            if (look.preset != Appearance::kNoPreset) playerPreset = look.preset;
            return;
        }
        Ent& e = ents.at(ped);
        e.look = look;
        e.outfit = look.outfit;
        ++e.looks;
    }
    int outfitPresets(EntityHandle ped) override { return ped == playerPed ? presetsOfModel : 0; }
    void setOutfitPreset(EntityHandle ped, int preset) override {
        if (ped != playerPed) return;
        playerPreset = preset;
        playerOutfit = 0xC0FFEE00u + static_cast<uint32_t>(preset);  // each preset is another outfit
        playerItems.clear();
    }
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
        ++nearbyCalls;
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
    bool adopt(EntityHandle h) override {
        ents.at(h).ours = false;
        ++adoptions;
        return true;
    }
    bool exists(EntityHandle h) override {
        ++existsCalls;
        return ents.count(h) != 0;
    }
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

inline LinkConfig hostConfig() {
    LinkConfig c;
    c.listen = true;
    c.local.role = Role::Host;
    c.local.name = "Arthur";
    c.local.avatar = "player_zero";
    return c;
}
inline LinkConfig guestConfig() {
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
    uint64_t time = 0;  // Unix seconds of `latest`, 0 = not known
    uint64_t latestStamp() override { return latest.empty() ? 0 : stamp; }
    uint64_t latestTime() override { return latest.empty() ? 0 : time; }
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
        if (!game.localModelHash) game.localModelHash = joaat(avatar.c_str());
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

inline EntityHandle mirrorOf(FakeGame& g, uint32_t model, EntityKind kind = EntityKind::Ped) {
    return g.find(SpawnRole::Mirror, model, kind);
}

inline bool sawNotice(CoopSession& s, const char* needle) {
    std::string text;
    bool seen = false;
    while (s.popNotice(text)) seen = seen || text.find(needle) != std::string::npos;
    return seen;
}

inline void dropPlayer(Cluster& c, Player& p) {
    p.link.stop();
    c.players.erase(std::remove(c.players.begin(), c.players.end(), &p), c.players.end());
}

inline bool journalHas(Cluster& c, const char* campaign, const char* needle) {
    for (const auto& line : c.storage.journals[campaign])
        if (line.find(needle) != std::string::npos) return true;
    return false;
}

