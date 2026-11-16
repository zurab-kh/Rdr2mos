// Client world synchronisation fixes: vehicles shared between players (own wagons, passengers,
// handovers), damage coalescing and world-epoch scoped damage. The reviewers' repro tests come
// first, then regression tests for the paths the fixes touch.
//
// COOP_SEED=<n> in the environment varies the simulated network (jitter, loss) of these tests.
#include "harness.h"

namespace {
const uint32_t kWagonFs = joaat("wagon02x");
const uint32_t kMissionFs = joaat("mission_review");

uint32_t fsSeed() {
    const char* env = std::getenv("COOP_SEED");
    return env ? static_cast<uint32_t>(std::strtoul(env, nullptr, 10)) : 0u;
}

void fsPlay(Player& p, uint16_t flags, uint32_t script = kMissionFs) {
    p.game.player.flags = flags;
    p.game.missionScript = (flags & PF_InMission) ? script : 0;
}

int fsWagons(FakeGame& g) {
    int n = 0;
    for (auto& [h, e] : g.ents) n += e.kind == EntityKind::Vehicle && e.model == kWagonFs && !e.parked;
    return n;
}
}  // namespace

// ---------------------------------------------------------------- reviewers' repros

// Finding 4.
COOP_TEST(testFixSyncOwnerLeavesOwnWagonJitter, "fix sync: owner gets off its wagon on a jittery network, one wagon each") {
    // Same as "passengers in a player's own wagon", but on every second run the one datagram that
    // carries A's "got down" state and the wagon's first snapshot is lost: the snapshot is repeated
    // a world tick later, the state only with the next idle update, so the mirror arrives first.
    int doubled = 0, runs = 0;
    for (uint32_t seed = 1; seed <= 12; ++seed) {
        Cluster c;
        c.net.rng.seed(seed + fsSeed() * 100);
        c.net.latencyMs = 40;
        c.net.jitterMs = 5;
        Player& a = c.join("A", true);
        c.run(200);
        Player& b = c.join("B");
        Player& d = c.join("C");
        a.game.player.pos = {0, 5, 0};
        b.game.player.pos = {3, 0, 0};
        d.game.player.pos = {6, 0, 0};
        EntityHandle wagon = a.game.add(EntityKind::Vehicle, kWagonFs, {0, 5, 0});
        a.game.playerVehicle = wagon;
        a.game.playerSeat = -1;
        c.run(2500);
        EntityHandle wagonOnB = b.game.find(SpawnRole::PuppetVehicle, kWagonFs, EntityKind::Vehicle);
        if (!wagonOnB) continue;
        b.game.playerVehicle = wagonOnB;
        b.game.playerSeat = 1;
        c.run(1500);
        a.game.playerVehicle = 0;
        a.game.player.pos = a.game.ents[wagon].pos + Vec3{2, 0, 0};
        if (seed % 2 == 0) {
            c.net.loss = 1.0;
            c.run(16);
            c.net.loss = 0.0;
        }
        c.run(2000);
        ++runs;
        EntityHandle bOnC = d.game.find(SpawnRole::PlayerPuppet, joaat("player_B"));
        EntityHandle aOnC = d.game.find(SpawnRole::PlayerPuppet, joaat("player_A"));
        bool seated = b.game.playerVehicle == wagonOnB && bOnC && d.game.ents[bOnC].vehicle != 0 && aOnC &&
                      d.game.ents[aOnC].vehicle == 0;
        if (fsWagons(b.game) != 1 || fsWagons(d.game) != 1 || !seated) {
            ++doubled;
            std::printf("  seed %u: wagons on B %d, on C %d, seats right %d\n", seed, fsWagons(b.game), fsWagons(d.game),
                        seated);
        }
    }
    std::printf("  %d of %d runs ended with a second wagon (or a wrong seat)\n", doubled, runs);
    CHECK(runs == 12);
    CHECK(doubled == 0);
}

// Finding 5.
COOP_TEST(testFixSyncDriveFriendsOwnWagon, "fix sync: B drives A's own wagon, A and C see it move") {
    Cluster c;
    c.net.rng.seed(fsSeed() + 5);
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    a.game.player.pos = {0, 5, 0};
    b.game.player.pos = {3, 0, 0};
    d.game.player.pos = {6, 0, 0};
    EntityHandle wagon = a.game.add(EntityKind::Vehicle, kWagonFs, {0, 5, 0});
    a.game.playerVehicle = wagon;
    a.game.playerSeat = -1;
    c.run(2000);
    EntityHandle wagonOnB = b.game.find(SpawnRole::PuppetVehicle, kWagonFs, EntityKind::Vehicle);
    EntityHandle wagonOnC = d.game.find(SpawnRole::PuppetVehicle, kWagonFs, EntityKind::Vehicle);
    CHECK(wagonOnB && wagonOnC);
    if (!wagonOnB || !wagonOnC) return;
    // A moves over to the passenger seat, B takes the reins of (B's copy of) A's wagon.
    a.game.playerSeat = 0;
    b.game.playerVehicle = wagonOnB;
    b.game.playerSeat = -1;
    c.perFrame = [&] {
        if (b.game.ents.count(wagonOnB)) {
            b.game.ents[wagonOnB].pos.y += 4 * 0.016f;
            b.game.ents[wagonOnB].vel = {0, 4, 0};
            b.game.player.pos = b.game.ents[wagonOnB].pos;
            b.game.player.vel = {0, 4, 0};
        }
        a.game.player.pos = a.game.ents[wagon].pos;
    };
    c.run(5000);
    c.perFrame = nullptr;
    Vec3 driven = b.game.ents[wagonOnB].pos;
    float offA = distance(a.game.ents[wagon].pos, driven);
    float offC = d.game.ents.count(wagonOnC) ? distance(d.game.ents[wagonOnC].pos, driven) : 1e9f;
    std::printf("  B drove A's wagon to y=%.1f; A's real wagon %.1f m off, C's copy %.1f m off\n", driven.y, offA, offC);
    CHECK(offA < 3.0f);
    CHECK(offC < 3.0f);
    // Still one wagon each, with both players in it.
    CHECK(fsWagons(a.game) == 1 && fsWagons(b.game) == 1 && fsWagons(d.game) == 1);
    EntityHandle bOnA = a.game.find(SpawnRole::PlayerPuppet, joaat("player_B"));
    EntityHandle aOnC = d.game.find(SpawnRole::PlayerPuppet, joaat("player_A"));
    EntityHandle bOnC = d.game.find(SpawnRole::PlayerPuppet, joaat("player_B"));
    CHECK(bOnA && a.game.ents[bOnA].vehicle == wagon && a.game.ents[bOnA].seat == -1);
    CHECK(aOnC && bOnC && d.game.ents[aOnC].vehicle == wagonOnC && d.game.ents[bOnC].vehicle == wagonOnC);
}

// Finding 6.
COOP_TEST(testFixSyncOwnerLeavesWithPassenger, "fix sync: A leaves while B rides in A's own wagon") {
    Cluster c;
    c.net.rng.seed(fsSeed() + 6);
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    a.game.player.pos = {0, 5, 0};
    b.game.player.pos = {3, 0, 0};
    d.game.player.pos = {6, 0, 0};
    EntityHandle wagon = a.game.add(EntityKind::Vehicle, kWagonFs, {0, 5, 0});
    a.game.playerVehicle = wagon;
    a.game.playerSeat = -1;
    c.run(2000);
    EntityHandle wagonOnB = b.game.find(SpawnRole::PuppetVehicle, kWagonFs, EntityKind::Vehicle);
    CHECK(wagonOnB != 0);
    if (!wagonOnB) return;
    b.game.playerVehicle = wagonOnB;
    b.game.playerSeat = 1;
    c.run(1500);
    // A's game quits (crash, or the player just leaves) while A is still on the box.
    dropPlayer(c, a);
    c.run(3000);
    std::printf("  after A left: B in vehicle %u (exists %d)\n", static_cast<unsigned>(b.game.playerVehicle),
                static_cast<int>(b.game.ents.count(wagonOnB)));
    CHECK(b.game.playerVehicle == wagonOnB && b.game.ents.count(wagonOnB));
    // It is B's own wagon now: C shows B sitting in one copy of it, nobody has a second one.
    EntityHandle bOnC = d.game.find(SpawnRole::PlayerPuppet, joaat("player_B"));
    EntityHandle wagonOnC = d.game.find(SpawnRole::PuppetVehicle, kWagonFs, EntityKind::Vehicle);
    CHECK(bOnC && wagonOnC && d.game.ents[bOnC].vehicle == wagonOnC);
    CHECK(fsWagons(b.game) == 1 && fsWagons(d.game) == 1);
}

// Finding 7.
COOP_TEST(testFixSyncPassengerKeepsMirrorWagon, "fix sync: a passenger of a mirror wagon keeps it when the original goes") {
    Cluster c;
    c.net.rng.seed(fsSeed() + 7);
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {10, 0, 0};
    d.game.player.pos = {12, 0, 0};
    EntityHandle wagon = a.game.add(EntityKind::Vehicle, kWagonFs, {10, 2, 0});
    c.run(2000);
    EntityHandle wagonOnB = mirrorOf(b.game, kWagonFs, EntityKind::Vehicle);
    EntityHandle wagonOnC = mirrorOf(d.game, kWagonFs, EntityKind::Vehicle);
    CHECK(wagonOnB && wagonOnC);
    if (!wagonOnB || !wagonOnC) return;
    b.game.playerVehicle = wagonOnB;
    b.game.playerSeat = -1;
    d.game.playerVehicle = wagonOnC;
    d.game.playerSeat = 0;
    c.perFrame = [&] {
        if (b.game.playerVehicle) {
            b.game.ents[b.game.playerVehicle].pos.x += 3 * 0.016f;
            b.game.ents[b.game.playerVehicle].vel = {3, 0, 0};
            b.game.player.pos = b.game.ents[b.game.playerVehicle].pos;
            b.game.player.vel = {3, 0, 0};
        }
        if (d.game.playerVehicle && d.game.ents.count(d.game.playerVehicle)) {
            d.game.player.pos = d.game.ents[d.game.playerVehicle].pos;
            d.game.player.vel = d.game.ents[d.game.playerVehicle].vel;
        }
    };
    c.run(1500);
    a.game.remove(wagon);
    c.run(1500);
    c.perFrame = nullptr;
    bool bKept = b.game.playerVehicle == wagonOnB && b.game.ents.count(wagonOnB) && !b.game.ents[wagonOnB].parked;
    EntityHandle cv = d.game.playerVehicle;
    bool cSeated = cv && d.game.ents.count(cv) && !d.game.ents[cv].parked;
    std::printf("  wagon removed by its owner: driver keeps it %d, passenger still seated in a live wagon %d\n", bKept, cSeated);
    CHECK(bKept);
    CHECK(cSeated);
    // One wagon in each game, B driving it and C beside B everywhere; C's copy keeps up with B.
    CHECK(fsWagons(a.game) == 1 && fsWagons(b.game) == 1 && fsWagons(d.game) == 1);
    CHECK(cv == wagonOnC);
    EntityHandle bOnC = d.game.find(SpawnRole::PlayerPuppet, joaat("player_B"));
    EntityHandle cOnB = b.game.find(SpawnRole::PlayerPuppet, joaat("player_C"));
    EntityHandle cOnA = a.game.find(SpawnRole::PlayerPuppet, joaat("player_C"));
    EntityHandle bOnA = a.game.find(SpawnRole::PlayerPuppet, joaat("player_B"));
    CHECK(bOnC && d.game.ents[bOnC].vehicle == wagonOnC && d.game.ents[bOnC].seat == -1);
    CHECK(cOnB && b.game.ents[cOnB].vehicle == wagonOnB && b.game.ents[cOnB].seat == 0);
    CHECK(cOnA && bOnA && a.game.ents[cOnA].vehicle != 0 && a.game.ents[cOnA].vehicle == a.game.ents[bOnA].vehicle);
    CHECK(distance(d.game.ents[wagonOnC].pos, b.game.ents[wagonOnB].pos) < 2.0f);
}

// Finding 8.
COOP_TEST(testFixSyncFriendsWagonKeptWhileSeated, "fix sync: A's wagon stays for C while B still sits in it") {
    Cluster c;
    c.net.rng.seed(fsSeed() + 8);
    c.config.worldSync = false;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    a.game.player.pos = {0, 5, 0};
    b.game.player.pos = {3, 0, 0};
    d.game.player.pos = {6, 0, 0};
    EntityHandle wagon = a.game.add(EntityKind::Vehicle, kWagonFs, {0, 5, 0});
    a.game.playerVehicle = wagon;
    a.game.playerSeat = -1;
    c.run(2000);
    EntityHandle wagonOnB = b.game.find(SpawnRole::PuppetVehicle, kWagonFs, EntityKind::Vehicle);
    EntityHandle wagonOnC = d.game.find(SpawnRole::PuppetVehicle, kWagonFs, EntityKind::Vehicle);
    CHECK(wagonOnB && wagonOnC);
    if (!wagonOnB || !wagonOnC) return;
    b.game.playerVehicle = wagonOnB;
    b.game.playerSeat = 1;
    b.game.player.pos = b.game.ents[wagonOnB].pos;
    c.run(1500);
    a.game.playerVehicle = 0;
    a.game.player.pos = {0, 30, 0};  // A walks over to the store
    c.run(35000);
    EntityHandle bOnC = d.game.find(SpawnRole::PlayerPuppet, joaat("player_B"));
    EntityHandle seat = bOnC ? d.game.ents[bOnC].vehicle : 0;
    std::printf("  35 s later: A's wagon on C exists %d, B's puppet on C seated in %u\n",
                static_cast<int>(d.game.ents.count(wagonOnC)), static_cast<unsigned>(seat));
    CHECK(b.game.playerVehicle == wagonOnB);
    CHECK(d.game.ents.count(wagonOnC) && seat == wagonOnC);
    // Once B gets down too, the copy is cleaned up as before.
    b.game.playerVehicle = 0;
    c.run(32000);
    CHECK(!d.game.ents.count(wagonOnC) && !b.game.ents.count(wagonOnB));
}

// Findings 2 and 16 (client side): damage over time no longer floods the server.
COOP_TEST(testFixSyncDamageBurstsKeepReliable, "fix sync: damage bursts do not cost the owner its reliable messages") {
    Cluster c;
    c.net.rng.seed(fsSeed() + 2);
    Player& a = c.join("A", true);
    Player& b = c.join("B");
    Player& d = c.join("C");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {3, 0, 0};
    d.game.player.pos = {-3, 0, 0};
    c.run(3000);
    EntityHandle pb = a.game.find(SpawnRole::PlayerPuppet, joaat("player_B"));
    EntityHandle pc = a.game.find(SpawnRole::PlayerPuppet, joaat("player_C"));
    CHECK(pb && pc);
    c.perFrame = [&] {
        // On fire in A's world (a little damage every frame); B and C keep healing in their own.
        for (EntityHandle h : {pb, pc})
            if (a.game.ents.count(h) && a.game.ents[h].health > 1) a.game.ents[h].health -= 1;
        b.game.playerHealth = d.game.playerHealth = 200;
    };
    c.run(4000);
    uint64_t limitedBefore = c.server->stats().rateLimited;
    a.game.waypoint = true;
    a.game.waypointPos = {500, 600, 0};
    c.run(3000);
    std::printf("  rate limited: %llu, B sees A's waypoint: %d\n",
                static_cast<unsigned long long>(c.server->stats().rateLimited), b.game.friendWaypoint);
    CHECK(limitedBefore == 0 || b.game.friendWaypoint);
    CHECK(b.game.friendWaypoint && d.game.friendWaypoint);
}

COOP_TEST(testFixSyncBurningEnemiesKeepDeath, "fix sync: enemies set on fire do not swallow a player's death in the mission") {
    Cluster c;
    c.net.rng.seed(fsSeed() + 16);
    c.net.latencyMs = 30;
    const uint32_t kOutlaw = joaat("g_m_m_uniduster_01");
    Player& a = c.join("A", true);
    c.run(300);
    Player& b = c.join("B");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {3, 0, 0};
    for (int i = 0; i < 4; ++i) a.game.addPed(kOutlaw, {float(8 + i), 6, 0}, 400);
    c.run(3000);
    fsPlay(a, PF_InMission);
    fsPlay(b, PF_InMission);
    c.run(2000);
    std::vector<EntityHandle> burning;
    for (auto& [h, en] : b.game.ents)
        if (en.ours && en.model == kOutlaw) burning.push_back(h);
    CHECK(burning.size() == 4);
    uint64_t limitedBefore = c.server->stats().rateLimited;
    int burned = 0;
    c.perFrame = [&] {
        for (EntityHandle h : burning) {
            auto it = b.game.ents.find(h);
            if (it == b.game.ents.end() || it->second.health <= 1) continue;
            it->second.health -= 1;  // fire: a little every frame, caused by B
            it->second.hitByPlayer = true;
            ++burned;
        }
    };
    c.run(2000);
    b.game.playerHealth = 0;  // B burns to death
    c.run(1500);
    c.perFrame = nullptr;
    c.run(500);
    uint64_t limited = c.server->stats().rateLimited - limitedBefore;
    int ownerHealth = 0;
    for (auto& [h, en] : a.game.ents)
        if (en.model == kOutlaw) ownerHealth += en.health;
    std::printf("  messages dropped by the server's per-player limits: %llu; the burning enemies have %d of 1600 "
                "health left at the owner (B burned off %d)\n",
                static_cast<unsigned long long>(limited), ownerHealth, burned);
    CHECK(limited == 0);
    CHECK(journalHas(c, "default", "died in a mission"));
    CHECK(a.game.playerHealth == 0);  // shared failure: A restarts too
    CHECK(ownerHealth == 1600 - burned);  // every point of fire damage arrived, once
}

// Finding 17 (client side).
COOP_TEST(testFixSyncDamageAcrossHandover, "fix sync: a hit on the old world's NPC never lands on another NPC of the new world") {
    Cluster c;
    c.net.rng.seed(fsSeed() + 17);
    c.net.latencyMs = 50;
    const uint32_t kOutlaw = joaat("g_m_m_uniduster_01"), kCompanion = joaat("cs_dutch");
    Player& a = c.join("A", true);
    c.run(300);
    Player& b = c.join("B");
    Player& d = c.join("C");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {4, 0, 0};
    d.game.player.pos = {0, 4, 0};
    a.game.addPed(kOutlaw, {10, 10, 0}, 100);
    EntityHandle companion = b.game.addPed(kCompanion, {6, 2, 0}, 100);
    b.game.ents[companion].mission = true;
    b.game.ents[companion].script = kMissionFs;
    fsPlay(a, PF_InMission);
    fsPlay(b, PF_InMission);
    fsPlay(d, PF_InMission);
    c.run(4000);
    EntityHandle outlawAtC = mirrorOf(d.game, kOutlaw);
    CHECK(outlawAtC != 0);
    dropPlayer(c, a);  // A quits; its goodbye reaches the server 50 ms later
    c.run(64);
    if (outlawAtC && d.game.ents.count(outlawAtC)) d.game.playerShoots(outlawAtC, 60);  // C's shot, before C hears of it
    c.run(3000);
    CHECK(b.session->status().worldOwner);
    std::printf("  B's companion (never shot at) has %d health\n", b.game.ents.count(companion) ? b.game.ents[companion].health : -1);
    CHECK(b.game.ents.count(companion) && b.game.ents[companion].health == 100);

    // The new world's own entities are hit as usual: C's next shot carries the new epoch.
    EntityHandle companionAtC = mirrorOf(d.game, kCompanion);
    CHECK(companionAtC != 0);
    if (companionAtC) d.game.playerShoots(companionAtC, 30);
    c.run(800);
    CHECK(b.game.ents.count(companion) && b.game.ents[companion].health == 70);
}

// ---------------------------------------------------------------- regressions

COOP_TEST(testFixSyncDamageCoalesced, "fix sync: damage over time is added up, ~10 messages a second, nothing lost") {
    // B sets an NPC of A's world on fire: B's copy loses 1 health every frame. A's NPC must lose
    // exactly as much, in at most ~10 steps a second (one per Damage message), the first at once.
    Cluster c;
    c.net.rng.seed(fsSeed() + 21);
    c.net.latencyMs = 40;
    c.net.jitterMs = 20;
    const uint32_t kOutlaw = joaat("g_m_m_uniduster_01");
    Player& a = c.join("A", true);
    c.run(300);
    Player& b = c.join("B");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {3, 0, 0};
    EntityHandle outlaw = a.game.addPed(kOutlaw, {6, 6, 0}, 1000);
    c.run(4500);  // with jitter the world can show up only with the owner's second world info
    EntityHandle onB = mirrorOf(b.game, kOutlaw);
    CHECK(onB != 0);
    if (!onB) return;
    // A single shot arrives as one message right away (no 100 ms wait).
    b.game.playerShoots(onB, 10);
    uint32_t shotAt = c.now;
    while (a.game.ents[outlaw].health == 1000 && c.now - shotAt < 1000) c.run(16);
    uint32_t firstHitMs = c.now - shotAt;
    CHECK(a.game.ents[outlaw].health == 990);
    c.net.loss = 0.05;  // lost datagrams are resent; the sum still arrives exactly once
    c.run(500);
    int burned = 0, steps = 0, last = a.game.ents[outlaw].health;
    c.perFrame = [&] {
        auto it = b.game.ents.find(onB);
        if (it != b.game.ents.end() && it->second.health > 1) {
            it->second.health -= 1;
            it->second.hitByPlayer = true;
            ++burned;
        }
        int now = a.game.ents[outlaw].health;
        if (now < last) ++steps;
        last = now;
    };
    c.run(3000);
    c.perFrame = [&] {
        int now = a.game.ents[outlaw].health;
        if (now < last) ++steps;
        last = now;
    };
    c.run(1500);
    c.perFrame = nullptr;
    std::printf("  first hit after %u ms; burned %d over 3 s, the owner's NPC lost %d in %d steps\n", firstHitMs, burned,
                990 - a.game.ents[outlaw].health, steps);
    CHECK(firstHitMs <= 3 * 16 + 2 * 60);  // one frame, plus two hops of latency and jitter
    CHECK(a.game.ents[outlaw].health == 990 - burned);
    CHECK(steps >= 20 && steps <= 33);  // ~3 s at 10 a second (an extra one at the end)
}

COOP_TEST(testFixSyncPuppetRespawnKeepsRide, "fix sync: a friend's puppet recreated while we sit in its wagon keeps the wagon") {
    Cluster c;
    c.net.rng.seed(fsSeed() + 22);
    c.net.latencyMs = 30;
    c.net.jitterMs = 10;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    a.game.player.pos = {0, 5, 0};
    b.game.player.pos = {3, 0, 0};
    EntityHandle wagon = a.game.add(EntityKind::Vehicle, kWagonFs, {0, 5, 0});
    a.game.playerVehicle = wagon;
    a.game.playerSeat = -1;
    c.run(2000);
    EntityHandle wagonOnB = b.game.find(SpawnRole::PuppetVehicle, kWagonFs, EntityKind::Vehicle);
    EntityHandle aOnB = b.game.find(SpawnRole::PlayerPuppet, joaat("player_A"));
    CHECK(wagonOnB && aOnB);
    if (!wagonOnB || !aOnB) return;
    b.game.playerVehicle = wagonOnB;
    b.game.playerSeat = 1;
    c.run(1000);
    // B's game kills A's character (a stray bullet of B's world), A lives on: it is recreated.
    b.game.ents[aOnB].health = 0;
    c.run(2500);
    EntityHandle reborn = b.game.find(SpawnRole::PlayerPuppet, joaat("player_A"));
    CHECK(reborn != 0 && b.game.ents[reborn].health > 0);
    CHECK(b.game.playerVehicle == wagonOnB && b.game.ents.count(wagonOnB));
    CHECK(reborn && b.game.ents[reborn].vehicle == wagonOnB && b.game.ents[reborn].seat == -1);
    CHECK(fsWagons(b.game) == 1 && b.game.adoptions == 0);
}

COOP_TEST(testFixSyncPassengerAcrossHandover, "fix sync: the owner leaves while B drives its wagon with C beside B") {
    Cluster c;
    c.net.rng.seed(fsSeed() + 23);
    c.net.latencyMs = 30;
    c.net.jitterMs = 10;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {10, 0, 0};
    d.game.player.pos = {12, 0, 0};
    a.game.add(EntityKind::Vehicle, kWagonFs, {10, 2, 0});
    c.run(2000);
    EntityHandle wagonOnB = mirrorOf(b.game, kWagonFs, EntityKind::Vehicle);
    EntityHandle wagonOnC = mirrorOf(d.game, kWagonFs, EntityKind::Vehicle);
    CHECK(wagonOnB && wagonOnC);
    if (!wagonOnB || !wagonOnC) return;
    b.game.playerVehicle = wagonOnB;
    b.game.playerSeat = -1;
    d.game.playerVehicle = wagonOnC;
    d.game.playerSeat = 0;
    c.perFrame = [&] {
        if (b.game.playerVehicle && b.game.ents.count(b.game.playerVehicle)) {
            b.game.ents[b.game.playerVehicle].pos.x += 3 * 0.016f;
            b.game.ents[b.game.playerVehicle].vel = {3, 0, 0};
            b.game.player.pos = b.game.ents[b.game.playerVehicle].pos;
            b.game.player.vel = {3, 0, 0};
        }
        if (d.game.playerVehicle && d.game.ents.count(d.game.playerVehicle)) {
            d.game.player.pos = d.game.ents[d.game.playerVehicle].pos;
            d.game.player.vel = d.game.ents[d.game.playerVehicle].vel;
        }
    };
    c.run(1500);
    dropPlayer(c, a);  // the world goes to B
    c.run(3000);
    c.perFrame = nullptr;
    CHECK(b.session->status().worldOwner || d.session->status().worldOwner);  // either may get it
    CHECK(b.game.playerVehicle == wagonOnB && b.game.ents.count(wagonOnB) && !b.game.ents[wagonOnB].parked);
    CHECK(d.game.playerVehicle == wagonOnC && d.game.ents.count(wagonOnC) && !d.game.ents[wagonOnC].parked);
    CHECK(fsWagons(b.game) == 1 && fsWagons(d.game) == 1);
    EntityHandle bOnC = d.game.find(SpawnRole::PlayerPuppet, joaat("player_B"));
    EntityHandle cOnB = b.game.find(SpawnRole::PlayerPuppet, joaat("player_C"));
    CHECK(bOnC && d.game.ents[bOnC].vehicle == wagonOnC);
    CHECK(cOnB && b.game.ents[cOnB].vehicle == wagonOnB && b.game.ents[cOnB].seat == 0);
    CHECK(distance(d.game.ents[wagonOnC].pos, b.game.ents[wagonOnB].pos) < 2.0f);
}
