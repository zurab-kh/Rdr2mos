// Tests for the portable core: protocol, link, relay and the co-op session logic,
// driven against a simulated game world and an in-memory lossy network.
#include <cstdio>
#include <cstdlib>
#include <deque>
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
    std::mt19937 rng{1234};

    class Socket : public IDatagramSocket {
    public:
        Socket(MemoryNetwork& net, Endpoint self) : net_(net), self_(self) {}
        bool sendTo(const Endpoint& to, const uint8_t* data, size_t size) override {
            if (std::uniform_real_distribution<double>(0, 1)(net_.rng) < net_.loss) return true;
            if (!net_.sockets_.count(self_)) return true;  // unplugged: nothing gets out either
            auto it = net_.sockets_.find(to);
            if (it != net_.sockets_.end())
                it->second->queue_.push_back({self_, std::vector<uint8_t>(data, data + size)});
            return true;
        }
        int recvFrom(Endpoint& from, uint8_t* buffer, size_t capacity) override {
            if (queue_.empty()) return -1;
            auto& front = queue_.front();
            size_t n = std::min(capacity, front.second.size());
            std::copy(front.second.begin(), front.second.begin() + n, buffer);
            from = front.first;
            queue_.pop_front();
            return static_cast<int>(n);
        }
        Endpoint self() const { return self_; }

    private:
        friend class MemoryNetwork;
        MemoryNetwork& net_;
        Endpoint self_;
        std::deque<std::pair<Endpoint, std::vector<uint8_t>>> queue_;
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
    struct Ped {
        uint32_t model = 0;
        Vec3 pos;
        float heading = 0;
        int health = 100;
        bool ours = false;  // created through spawnPed
        PedKind kind = PedKind::NpcMirror;
        bool hidden = false;
        bool hitByPlayer = false;
        EntityHandle mount = 0;
        uint32_t weapon = 0;
        uint8_t flags = 0;
        bool fightsPeer = false;
        int shotsFired = 0;
        std::string label;
    };

    std::map<EntityHandle, Ped> peds;
    EntityHandle next = 100;
    PlayerState player;
    int playerHealth = 200;
    EntityHandle ownHorse = 0;
    bool isReady = true;
    bool populationSuppressed = false;
    WorldInfo clock{12, 0, 0, 0};
    int modelLoadDelay = 0;  // spawnPed returns 0 this many times first

    EntityHandle addNpc(uint32_t model, Vec3 pos, int health = 100) {
        EntityHandle h = next++;
        Ped p;
        p.model = model;
        p.pos = pos;
        p.health = health;
        peds[h] = p;
        return h;
    }
    // The local player shoots a ped.
    void playerShoots(EntityHandle h, int damage) {
        Ped& p = peds.at(h);
        p.health = std::max(0, p.health - damage);
        p.hitByPlayer = true;
    }
    int countKind(PedKind kind) const {
        int n = 0;
        for (auto& [h, p] : peds) n += p.ours && p.kind == kind;
        return n;
    }
    EntityHandle findKind(PedKind kind, uint32_t model = 0) const {
        for (auto& [h, p] : peds)
            if (p.ours && p.kind == kind && (!model || p.model == model)) return h;
        return 0;
    }

    bool ready() override { return isReady; }
    PlayerState readLocalPlayer() override {
        PlayerState s = player;
        s.health = static_cast<uint16_t>(playerHealth);
        s.maxHealth = 200;
        if (playerHealth <= 0) s.flags |= PF_Dead;
        return s;
    }
    void damageLocalPlayer(int amount) override { playerHealth = std::max(0, playerHealth - amount); }
    void teleportLocalPlayer(const Vec3& pos) override { player.pos = pos; }
    bool belongsToLocalPlayer(EntityHandle h) override { return h == ownHorse; }

    void listNpcs(const Vec3& a, const Vec3& b, float radius, const std::vector<EntityHandle>& exclude,
                  std::vector<LocalNpc>& out) override {
        for (auto& [h, p] : peds) {
            if (h == ownHorse) continue;
            if (std::find(exclude.begin(), exclude.end(), h) != exclude.end()) continue;
            if (distance(p.pos, a) > radius && distance(p.pos, b) > radius) continue;
            LocalNpc n;
            n.handle = h;
            n.mount = p.mount;
            n.fightsPeer = p.fightsPeer;
            n.state.model = p.model;
            n.state.pos = p.pos;
            n.state.heading = p.heading;
            n.state.health = static_cast<int16_t>(p.health);
            n.state.flags = p.flags | (p.health <= 0 ? NF_Dead : 0);
            n.state.weapon = p.weapon;
            out.push_back(n);
        }
    }
    void listAllPeds(std::vector<EntityHandle>& out) override {
        for (auto& [h, p] : peds) out.push_back(h);
    }
    WorldInfo readWorldInfo() override { return clock; }
    void applyWorldInfo(const WorldInfo& info) override { clock = info; }

    EntityHandle spawnPed(uint32_t model, const Vec3& pos, float heading, PedKind kind) override {
        if (modelLoadDelay > 0) { --modelLoadDelay; return 0; }
        EntityHandle h = addNpc(model, pos);
        peds[h].heading = heading;
        peds[h].ours = true;
        peds[h].kind = kind;
        return h;
    }
    void deletePed(EntityHandle h) override { peds.erase(h); }
    bool exists(EntityHandle h) override { return peds.count(h) != 0; }
    bool isDead(EntityHandle h) override { return peds.at(h).health <= 0; }
    Vec3 position(EntityHandle h) override { return peds.at(h).pos; }
    float heading(EntityHandle h) override { return peds.at(h).heading; }
    int health(EntityHandle h) override { return peds.at(h).health; }
    void setHealth(EntityHandle h, int health, int) override { peds.at(h).health = health; }
    void kill(EntityHandle h) override { peds.at(h).health = 0; }
    void teleport(EntityHandle h, const Vec3& pos, float heading) override {
        peds.at(h).pos = pos;
        peds.at(h).heading = heading;
    }
    void moveTo(EntityHandle h, const Vec3& pos, float, float heading) override { teleport(h, pos, heading); }
    void standStill(EntityHandle h, float heading) override { peds.at(h).heading = heading; }
    void aimAt(EntityHandle, const Vec3&) override {}
    void shootAt(EntityHandle h, const Vec3&) override { ++peds.at(h).shotsFired; }
    void setWeapon(EntityHandle h, uint32_t weapon) override { peds.at(h).weapon = weapon; }
    void setMount(EntityHandle rider, EntityHandle mount) override { peds.at(rider).mount = mount; }
    void setLabel(EntityHandle h, const char* name) override { peds.at(h).label = name; }
    void damageNpc(EntityHandle npc, int amount, EntityHandle) override {
        peds.at(npc).health = std::max(0, peds.at(npc).health - amount);
    }
    bool wasDamagedByLocalPlayer(EntityHandle h) override {
        bool hit = peds.at(h).hitByPlayer;
        peds.at(h).hitByPlayer = false;
        return hit;
    }
    void suppressAmbientPopulation() override { populationSuppressed = true; }
    void setHidden(EntityHandle h, bool hidden) override { peds.at(h).hidden = hidden; }
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

    void start() {
        hostLink.start(hostSock, hostConfig(), now);
        guestLink.start(guestSock, guestConfig(), now);
        host = std::make_unique<CoopSession>(hostGame, hostLink, config);
        guest = std::make_unique<CoopSession>(guestGame, guestLink, config);
    }
    void run(uint32_t ms) {
        for (uint32_t end = now + ms; now < end; now += 16) {
            host->tick(now);
            guest->tick(now);
        }
    }
};

// ---------------------------------------------------------------- tests
static void testProtocol() {
    PlayerState s;
    s.timeMs = 42;
    s.pos = {1.5f, -2.5f, 3.25f};
    s.heading = 271.0f;
    s.health = 180;
    s.weapon = joaat("weapon_revolver_cattleman");
    s.flags = PF_OnMount | PF_Shooting;
    PlayerState t;
    CHECK(decode(encode(s), t));
    CHECK(t.timeMs == 42 && t.pos.y == -2.5f && t.heading == 271.0f && t.health == 180);
    CHECK(t.weapon == s.weapon && t.flags == s.flags);

    WorldSnapshot snap;
    snap.timeMs = 7;
    for (int i = 0; i < 32; ++i) {
        NpcState n;
        n.id = static_cast<uint16_t>(i + 1);
        n.model = 1000u + i;
        n.pos = {float(i), 2, 3};
        n.heading = 90.0f;
        n.speed = 3.4f;
        n.health = static_cast<int16_t>(100 - i);
        n.mountId = static_cast<uint16_t>(i);
        snap.npcs.push_back(n);
    }
    std::vector<uint8_t> bytes = encode(snap);
    CHECK(bytes.size() == 5 + 32 * kNpcWireSize);
    CHECK(bytes.size() + 16 < kMaxDatagram);
    WorldSnapshot back;
    CHECK(decode(bytes, back));
    CHECK(back.npcs.size() == 32 && back.npcs[31].id == 32 && back.npcs[31].health == 69);
    CHECK(std::abs(back.npcs[3].heading - 90.0f) < 0.01f && std::abs(back.npcs[3].speed - 3.4f) < 0.01f);

    bytes.resize(bytes.size() - 3);  // truncated input must be rejected, not read out of bounds
    CHECK(!decode(bytes, back));
    CHECK(!decode(std::vector<uint8_t>{1, 2}, t));

    CHECK(joaat("player_zero") == 0x0D7114C9u);
    CHECK(joaat("PLAYER_THREE") == 0x00B69710u);
}

static void testInterpolation() {
    MotionBuffer buf(100);
    buf.push({1000, {0, 0, 0}, 350, 1}, 5000);
    buf.push({1100, {10, 0, 0}, 10, 1}, 5100);
    MotionSample mid = buf.sample(5150);  // sender time 1050
    CHECK(std::abs(mid.pos.x - 5.0f) < 0.01f);
    CHECK(std::abs(angleDelta(mid.heading, 0.0f)) < 0.01f);  // wraps through 360
    MotionSample late = buf.sample(9000);  // extrapolation is capped
    CHECK(late.pos.x > 10.0f && late.pos.x < 40.0f);
    buf.push({1050, {99, 0, 0}, 0, 0}, 5200);  // out-of-order sample is ignored
    CHECK(buf.latest().pos.x == 10.0f);
    CHECK(std::abs(angleDelta(350, 10) - 20.0f) < 0.001f);
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
    auto* a = net.open(kHostAddr.ip, kHostAddr.port);
    auto* b = net.open(kGuestAddr.ip, kGuestAddr.port);
    Link host, guest;
    host.start(a, hostConfig(), 0);
    guest.start(b, guestConfig(), 0);

    std::vector<uint32_t> received;
    uint32_t sent = 0;
    for (uint32_t now = 0; now < 30000; now += 16) {
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
    CHECK(host.pingMs() >= 0);
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

    // Reliable traffic works again in the new session.
    guest.send(Msg::Damage, Damage{5, 9}, true);
    run(500);
    Msg type;
    std::vector<uint8_t> payload;
    bool got = false;
    while (host.poll(type, payload)) got = got || type == Msg::Damage;
    CHECK(got);

    // Clean goodbye puts the other side back into waiting immediately.
    guest.stop();
    run(200);
    CHECK(host.state() == Link::State::Connecting);

    // Two hosts cannot play together.
    Link other;
    LinkConfig clash = guestConfig();
    clash.local.role = Role::Host;
    other.start(b, clash, now);
    for (uint32_t end = now + 1000; now < end; now += 16) {
        host.update(now);
        other.update(now);
    }
    CHECK(other.state() == Link::State::Failed && !other.error().empty());
    CHECK(host.state() == Link::State::Connecting && !host.error().empty());  // host keeps waiting, shows why
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

    host.send(Msg::WorldInfo, WorldInfo{1, 2, 3, 1}, true);
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

    // Idle clients are forgotten, freeing the room.
    net.unplug(a);
    net.unplug(b);
    for (uint32_t end = now + 20000; now < end; now += 100) relay.pump(now);
    CHECK(relay.roomCount() == 0 && relay.clientCount() == 0);
}

static void testSessionSharedWorld() {
    Pair p;
    const uint32_t kOutlaw = joaat("g_m_m_uniduster_01");
    const uint32_t kHorse = joaat("a_c_horse_morgan_bay");
    p.hostGame.player.pos = {100, 100, 50};
    p.guestGame.player.pos = {110, 100, 50};
    EntityHandle outlaw = p.hostGame.addNpc(kOutlaw, {105, 105, 50});
    EntityHandle horse = p.hostGame.addNpc(kHorse, {120, 100, 50});
    EntityHandle rider = p.hostGame.addNpc(kOutlaw, {120, 100, 51});
    p.hostGame.peds[rider].mount = horse;
    p.hostGame.addNpc(kOutlaw, {5000, 5000, 0});  // far away: must not be streamed
    // The guest's own game has spawned a ped the host knows nothing about, plus the guest's horse.
    EntityHandle stray = p.guestGame.addNpc(kOutlaw, {111, 101, 50});
    p.guestGame.ownHorse = p.guestGame.addNpc(kHorse, {112, 100, 50});
    p.guestGame.modelLoadDelay = 3;
    p.hostGame.clock = WorldInfo{21, 30, 0, 0};

    p.start();
    p.run(1500);

    // Each side sees the other player as a puppet with the announced avatar and name.
    EntityHandle guestPuppet = p.hostGame.findKind(PedKind::PlayerPuppet);
    EntityHandle hostPuppet = p.guestGame.findKind(PedKind::PlayerPuppet);
    CHECK(guestPuppet && hostPuppet);
    CHECK(p.hostGame.peds[guestPuppet].model == joaat("player_three"));
    CHECK(p.guestGame.peds[hostPuppet].model == joaat("player_zero"));
    CHECK(p.hostGame.peds[guestPuppet].label == "John");
    CHECK(distance(p.hostGame.peds[guestPuppet].pos, p.guestGame.player.pos) < 0.5f);
    CHECK(p.host->status().peerVisible && std::abs(p.host->status().peerDistance - 10.0f) < 0.5f);

    // The guest got mirrors of the three nearby NPCs only, never of puppets.
    CHECK(p.guestGame.countKind(PedKind::NpcMirror) == 3);
    CHECK(p.guest->status().syncedNpcs == 3);
    CHECK(p.hostGame.countKind(PedKind::NpcMirror) == 0);
    EntityHandle mirrorHorse = p.guestGame.findKind(PedKind::NpcMirror, kHorse);
    CHECK(mirrorHorse != 0);
    int mounted = 0;
    for (auto& [h, ped] : p.guestGame.peds) mounted += ped.ours && ped.mount == mirrorHorse;
    CHECK(mounted == 1);

    // Guest world: ambient population off, stray local ped hidden, own horse untouched, clock follows host.
    CHECK(p.guestGame.populationSuppressed && !p.hostGame.populationSuppressed);
    CHECK(p.guestGame.peds[stray].hidden);
    CHECK(!p.guestGame.peds[p.guestGame.ownHorse].hidden);
    CHECK(p.guestGame.clock.hour == 21 && p.guestGame.clock.minute == 30);

    // NPC movement on the host is followed by the mirror.
    p.hostGame.peds[outlaw].pos = {108, 108, 50};
    p.run(600);
    EntityHandle mirrorOutlaw = 0;
    for (auto& [h, ped] : p.guestGame.peds)
        if (ped.ours && ped.kind == PedKind::NpcMirror && ped.model == kOutlaw && !ped.mount) mirrorOutlaw = h;
    CHECK(mirrorOutlaw != 0);
    CHECK(distance(p.guestGame.peds[mirrorOutlaw].pos, Vec3{108, 108, 50}) < 0.5f);

    // The guest shoots the mirror: the damage lands on the host's real NPC, exactly once.
    p.guestGame.playerShoots(mirrorOutlaw, 40);
    p.run(500);
    CHECK(p.hostGame.peds[outlaw].health == 60);
    CHECK(p.guestGame.peds[mirrorOutlaw].health == 60);

    // A host NPC hurts the guest's puppet: the guest player loses that health.
    p.hostGame.peds[guestPuppet].health -= 25;
    p.run(500);
    CHECK(p.guestGame.playerHealth == 175);
    CHECK(p.hostGame.peds[guestPuppet].health == 175);

    // Friendly fire is off: the host shooting the puppet changes nothing for the guest.
    p.hostGame.playerShoots(guestPuppet, 50);
    p.run(500);
    CHECK(p.guestGame.playerHealth == 175);

    // The guest finishes the outlaw; both worlds agree it is dead and it stays a corpse.
    p.guestGame.playerShoots(mirrorOutlaw, 500);
    p.run(500);
    CHECK(p.hostGame.peds[outlaw].health == 0);
    p.run(3000);
    CHECK(p.guestGame.peds.count(mirrorOutlaw) && p.guestGame.peds[mirrorOutlaw].health == 0);

    // NPC firing on the host shows up as firing on the guest.
    p.hostGame.peds[rider].flags = NF_Shooting | NF_InCombat;
    p.run(700);
    int shots = 0;
    for (auto& [h, ped] : p.guestGame.peds) shots += ped.ours ? ped.shotsFired : 0;
    CHECK(shots >= 1);

    // Removed on the host -> removed on the guest after the timeout.
    p.hostGame.peds.erase(rider);
    p.hostGame.peds.erase(horse);
    p.run(3000);
    CHECK(p.guestGame.countKind(PedKind::NpcMirror) == 1);

    // Disconnect: everything the session created disappears and hidden peds come back.
    p.hostLink.stop();
    p.run(300);
    CHECK(p.guestGame.countKind(PedKind::NpcMirror) == 0);
    CHECK(p.guestGame.countKind(PedKind::PlayerPuppet) == 0);
    CHECK(!p.guestGame.peds[stray].hidden);
    p.host->cleanup();
    CHECK(p.hostGame.countKind(PedKind::PlayerPuppet) == 0);
}

static void testSessionPlayerStates() {
    Pair p;
    p.net.loss = 0.1;
    p.config.friendlyFire = true;
    const uint32_t kHorse = joaat("a_c_horse_morgan_bay");
    p.hostGame.player.pos = {0, 0, 10};
    p.guestGame.player.pos = {3, 0, 10};
    p.start();
    p.run(1500);
    EntityHandle guestPuppet = p.hostGame.findKind(PedKind::PlayerPuppet);
    CHECK(guestPuppet != 0);

    // Mounting, riding away and shooting from the saddle.
    p.guestGame.player.flags = PF_OnMount | PF_Shooting;
    p.guestGame.player.mountModel = kHorse;
    p.guestGame.player.weapon = joaat("weapon_repeater_carbine");
    for (int i = 0; i < 60; ++i) {
        p.guestGame.player.pos.x += 0.2f;
        p.guestGame.player.vel = {12, 0, 0};
        p.run(16);
    }
    p.guestGame.player.vel = {0, 0, 0};
    p.run(800);
    EntityHandle puppetHorse = p.hostGame.findKind(PedKind::PuppetMount);
    CHECK(puppetHorse != 0);
    CHECK(p.hostGame.peds[guestPuppet].mount == puppetHorse);
    CHECK(distance(p.hostGame.peds[puppetHorse].pos, p.guestGame.player.pos) < 1.0f);
    CHECK(p.hostGame.peds[guestPuppet].weapon == joaat("weapon_repeater_carbine"));
    CHECK(p.hostGame.peds[guestPuppet].shotsFired > 0);

    // Dismount.
    p.guestGame.player.flags = 0;
    p.run(500);
    CHECK(p.hostGame.peds[guestPuppet].mount == 0);
    CHECK(distance(p.hostGame.peds[guestPuppet].pos, p.guestGame.player.pos) < 1.0f);

    // Friendly fire enabled: host shoots the puppet, guest takes the damage.
    p.hostGame.playerShoots(guestPuppet, 30);
    p.run(800);
    CHECK(p.guestGame.playerHealth == 170);

    // Guest dies -> puppet dies; guest respawns -> a live puppet is back.
    p.guestGame.playerHealth = 0;
    p.run(800);
    CHECK(p.hostGame.peds[guestPuppet].health == 0);
    CHECK(p.host->status().peerDead);
    p.guestGame.playerHealth = 200;
    p.run(3000);
    EntityHandle reborn = p.hostGame.findKind(PedKind::PlayerPuppet);
    CHECK(reborn != 0 && p.hostGame.peds[reborn].health == 200);
    CHECK(p.hostGame.countKind(PedKind::PlayerPuppet) == 1);

    // Teleport to friend.
    p.hostGame.player.pos = {900, 900, 10};
    p.run(500);
    CHECK(p.guest->teleportToPeer());
    CHECK(distance(p.guestGame.player.pos, Vec3{900, 900, 10}) < 3.0f);

    // World sync switched off on the host: the guest keeps its own world.
    Pair q;
    q.config.worldSync = false;
    q.hostGame.addNpc(1, {1, 1, 1});
    EntityHandle local = q.guestGame.addNpc(2, {1, 1, 1});
    q.start();
    q.run(1500);
    CHECK(q.guestGame.countKind(PedKind::NpcMirror) == 0);
    CHECK(!q.guestGame.peds[local].hidden && !q.guestGame.populationSuppressed);
    CHECK(q.guestGame.countKind(PedKind::PlayerPuppet) == 1);
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
        {"interpolation", testInterpolation},
        {"ini", testIni},
        {"link: reliable channel under 35% loss", testLinkReliableUnderLoss},
        {"link: reconnect, bye, role clash", testLinkReconnectAndRoleClash},
        {"relay", testRelay},
        {"session: shared world", testSessionSharedWorld},
        {"session: player states", testSessionPlayerStates},
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
