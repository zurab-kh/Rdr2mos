// Tests for the portable core: protocol, link, relay, interpolation and the co-op session,
// driven against a simulated game world and an in-memory network with latency, jitter and loss.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "config.h"
#include "interp.h"
#include "link.h"
#include "relay.h"
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
        for (auto& [h, e] : ents) n += e.ours && e.role == role && e.kind == kind;
        return n;
    }
    EntityHandle find(SpawnRole role, uint32_t model = 0, EntityKind kind = EntityKind::Ped) const {
        for (auto& [h, e] : ents)
            if (e.ours && e.role == role && e.kind == kind && (!model || e.model == model)) return h;
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
            if (e.steered) {
                e.pos = e.pos + e.vel * dt;
                e.steered = false;
            }
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
    bool belongsToLocalPlayer(EntityHandle h) override {
        return h == playerPed || h == ownHorse || (h && (h == playerMount || h == playerVehicle));
    }

    void listNearby(const Vec3& a, const Vec3& b, float radius, std::vector<NearbyEntity>& out) override {
        for (auto& [h, e] : ents) {
            if (belongsToLocalPlayer(h) || e.draftOf) continue;
            if (distance(e.pos, a) > radius && distance(e.pos, b) > radius) continue;
            out.push_back({h, e.kind, e.pos});
        }
    }
    void readEntity(EntityHandle h, EntityKind, EntityRead& out) override {
        ++readCalls;
        const Ent& e = ents.at(h);
        out.state.kind = e.kind;
        out.state.model = e.model;
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
            if (h != playerPed) out.push_back({h, e.draftOf});
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
        return h;
    }
    void remove(EntityHandle h) override {
        ents.erase(h);
        for (auto& [k, e] : ents) {
            if (e.mount == h) e.mount = 0;
            if (e.vehicle == h) e.vehicle = 0;
        }
    }
    bool exists(EntityHandle h) override { return ents.count(h) != 0; }
    bool isDead(EntityHandle h) override { return ents.at(h).health <= 0; }
    Vec3 position(EntityHandle h) override { return ents.at(h).pos; }
    float heading(EntityHandle h) override { return ents.at(h).heading; }
    int health(EntityHandle h) override { return ents.at(h).health; }
    void setHealth(EntityHandle h, int health, int) override { ents.at(h).health = health; }
    void kill(EntityHandle h) override { ents.at(h).health = 0; }
    void teleport(EntityHandle h, const Vec3& pos, float heading) override {
        Ent& e = ents.at(h);
        e.pos = pos;
        e.heading = heading;
        e.hasGoal = false;
        ++e.teleports;
    }
    void nudge(EntityHandle h, const Vec3& delta) override {
        ents.at(h).pos = ents.at(h).pos + delta;
        ++ents.at(h).nudges;
    }
    void moveTo(EntityHandle h, const Vec3& pos, float speed, float heading) override {
        Ent& e = ents.at(h);
        e.hasGoal = true;
        e.goal = pos;
        e.goalSpeed = speed;
        e.heading = heading;
        ++e.tasks;
    }
    void standStill(EntityHandle h, float heading) override {
        ents.at(h).hasGoal = false;
        ents.at(h).heading = heading;
        ++ents.at(h).tasks;
    }
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
    void steerVehicle(EntityHandle h, const Vec3& velocity, float heading, float, float) override {
        Ent& e = ents.at(h);
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
static const Endpoint kRelayAddr{0x0A000009, 27960};

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

struct Pair {
    MemoryNetwork net;
    MemoryNetwork::Socket* hostSock = net.open(kHostAddr.ip, kHostAddr.port);
    MemoryNetwork::Socket* guestSock = net.open(kGuestAddr.ip, kGuestAddr.port);
    Link hostLink, guestLink;
    FakeGame hostGame, guestGame;
    SessionConfig config;
    std::unique_ptr<CoopSession> host, guest;
    uint32_t now = 1000;
    std::function<void()> perFrame;

    void start() {
        net.now = now;
        hostLink.start(hostSock, hostConfig(), now);
        guestLink.start(guestSock, guestConfig(), now);
        host = std::make_unique<CoopSession>(hostGame, hostLink, config);
        guest = std::make_unique<CoopSession>(guestGame, guestLink, config);
    }
    void run(uint32_t ms) {
        for (uint32_t end = now + ms; now < end; now += 16) {
            net.now = now;
            if (perFrame) perFrame();
            host->tick(now);
            guest->tick(now);
            hostGame.simulate(0.016f);
            guestGame.simulate(0.016f);
        }
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
    far.targetRef = kRefGuestPlayer;
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
    CHECK(back.entities[1].targetRef == kRefGuestPlayer && back.entities[1].seat == 2 && back.entities[1].health == 321);
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

static void testRelay() {
    MemoryNetwork net;
    auto* relaySock = net.open(kRelayAddr.ip, kRelayAddr.port);
    auto* a = net.open(kHostAddr.ip, 5000);
    auto* b = net.open(kGuestAddr.ip, 6000);
    auto* c = net.open(0x0A000003, 7000);
    Relay relay(*relaySock);

    LinkConfig hc = hostConfig();
    hc.listen = false;
    hc.viaRelay = true;
    hc.remote = kRelayAddr;
    hc.room = "blackwater";
    LinkConfig gc = guestConfig();
    gc.viaRelay = true;
    gc.remote = kRelayAddr;
    gc.room = "blackwater";

    Link host, guest, intruder;
    host.start(a, hc, 0);
    guest.start(b, gc, 0);
    uint32_t now = 0;
    auto run = [&](uint32_t ms) {
        for (uint32_t end = now + ms; now < end; now += 16) {
            net.now = now;
            relay.pump(now);
            host.update(now);
            guest.update(now);
            intruder.update(now);
            host.flush(now);
            guest.flush(now);
        }
    };
    run(2000);
    CHECK(host.connected() && guest.connected());
    CHECK(host.relayPeers() == 2);
    CHECK(relay.roomCount() == 1 && relay.clientCount() == 2);

    host.send(Msg::WorldInfo, WorldInfo{1, 2, 3, 1, 0}, true);
    run(300);
    Msg type;
    std::vector<uint8_t> payload;
    bool got = false;
    while (guest.poll(type, payload)) got = got || type == Msg::WorldInfo;
    CHECK(got);

    intruder.start(c, gc, now);
    run(1500);
    CHECK(intruder.state() == Link::State::Failed);
    CHECK(host.connected() && guest.connected());

    net.unplug(a);
    net.unplug(b);
    for (uint32_t end = now + 20000; now < end; now += 100) {
        net.now = now;
        relay.pump(now);
    }
    CHECK(relay.roomCount() == 0 && relay.clientCount() == 0);
}

static void testSessionSharedWorld() {
    Pair p;
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
    CHECK(!p.guestGame.ents.count(mirrorRider) && !p.guestGame.ents.count(mirrorHorse));

    // Disconnect: everything the session created disappears and hidden entities come back.
    p.hostLink.stop();
    p.run(300);
    CHECK(p.guestGame.count(SpawnRole::Mirror) == 0);
    CHECK(p.guestGame.count(SpawnRole::Mirror, EntityKind::Vehicle) == 0);
    CHECK(p.guestGame.count(SpawnRole::PlayerPuppet) == 0);
    CHECK(!p.guestGame.ents[storyPed].hidden);
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
        int lag = (p.host->status().bufferMs + 75) / 16;
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
        {"relay", testRelay},
        {"session: shared world, vehicles, damage, removal", testSessionSharedWorld},
        {"session: rides, vehicles, control transfer", testSessionRidesAndControl},
        {"session: tracking accuracy", testTrackingAccuracy},
        {"session: bandwidth and level of detail", testBandwidthAndLod},
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
