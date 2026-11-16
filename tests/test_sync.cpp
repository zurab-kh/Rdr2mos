// Synchronisation and smoothness: movement prediction, loss bursts, clock drift, N-player
// control transfer and seats, world handover, and the CPU cost per frame.
#include <chrono>

#include "harness.h"

namespace {

struct TrackStats {
    float maxErr = 0, meanErr = 0;  // against where the guest was one smoothing delay + latency ago
    float maxPathDev = 0;           // distance to the guest's real path, whatever the timing
    int warps = 0, tasks = 0, nudges = 0;
};

// The guest moves along `path` (position at time t in seconds); the host's puppet follows through
// game tasks only. Measured for `seconds` after a warm-up.
TrackStats trackPath(const std::function<Vec3(float)>& path, float seconds, uint32_t latency, uint32_t jitter,
                     double loss, float warmup = 2.0f) {
    Pair p;
    p.net.latencyMs = latency;
    p.net.jitterMs = jitter;
    p.net.loss = loss;
    p.hostGame.player.pos = path(0) + Vec3{0, -30, 0};
    p.guestGame.player.pos = path(0);
    p.start();
    p.run(1000);
    float t = 0;
    std::vector<Vec3> history;
    p.perFrame = [&] {
        t += 0.016f;
        Vec3 pos = path(t), ahead = path(t + 0.01f);
        Vec3 vel = (ahead - pos) * 100.0f;
        p.guestGame.player.pos = pos;
        p.guestGame.player.vel = vel;
        if (length(vel) > 0.1f) p.guestGame.player.heading = std::atan2(-vel.x, vel.y) * 180.0f / 3.14159265f;
        history.push_back(pos);
    };
    p.run(static_cast<uint32_t>(warmup * 1000));
    TrackStats st;
    EntityHandle puppet = p.hostGame.find(SpawnRole::PlayerPuppet);
    if (!puppet) {
        st.maxErr = 1e9f;
        return st;
    }
    int teleports = p.hostGame.ents[puppet].teleports, tasks = p.hostGame.ents[puppet].tasks,
        nudges = p.hostGame.ents[puppet].nudges;
    float sum = 0;
    int samples = 0;
    for (int i = 0; i < static_cast<int>(seconds * 1000 / 16); ++i) {
        p.run(16);
        Vec3 at = p.hostGame.ents[puppet].pos;
        int lag = (p.host->status().bufferMs + 100) / 16;
        if (static_cast<int>(history.size()) <= lag) continue;
        float err = distance(at, history[history.size() - 1 - lag]);
        st.maxErr = std::max(st.maxErr, err);
        sum += err;
        ++samples;
        float dev = 1e9f;
        for (size_t k = history.size() > 200 ? history.size() - 200 : 0; k < history.size(); ++k)
            dev = std::min(dev, distance(at, history[k]));
        st.maxPathDev = std::max(st.maxPathDev, dev);
    }
    st.meanErr = samples ? sum / samples : 0;
    st.warps = p.hostGame.ents[puppet].teleports - teleports;
    st.tasks = p.hostGame.ents[puppet].tasks - tasks;
    st.nudges = p.hostGame.ents[puppet].nudges - nudges;
    return st;
}

void printTrack(const char* what, const TrackStats& s, float seconds) {
    std::printf("  %s: max %.2f m, mean %.2f m, off path %.2f m, warps %d, tasks %.1f/s, nudges %.1f/s\n", what,
                s.maxErr, s.meanErr, s.maxPathDev, s.warps, s.tasks / seconds, s.nudges / seconds);
}

}  // namespace

COOP_TEST(testSyncMovementPrediction, "sync: movement prediction (figure-eight, sharp turns, gallop)") {
    // Figure-eight at 5 m/s, the same course as the tracking test.
    auto eight = [](float t) {
        float w = 5.0f / 15.0f;
        return Vec3{15 * std::sin(w * t), 7.5f * std::sin(2 * w * t), 0};
    };
    TrackStats a = trackPath(eight, 10, 50, 40, 0.05);
    printTrack("figure-eight 5 m/s", a, 10);
    CHECK(a.warps == 0);
    CHECK(a.meanErr < 0.3f && a.maxErr < 0.6f);  // was 0.33 / 0.43 m: puppets ran ahead of the stream

    // Zig-zag: 90-degree turns every 2 s at a run.
    auto zigzag = [](float t) {
        const float v = 5.0f, leg = 2.0f;
        int n = static_cast<int>(t / leg);
        float r = t - n * leg;
        float x = 0, y = 0;
        for (int i = 0; i < n; ++i) (i % 2 ? x : y) += v * leg;
        (n % 2 ? x : y) += v * r;
        return Vec3{x, y, 0};
    };
    TrackStats b = trackPath(zigzag, 10, 50, 40, 0.05);
    printTrack("zig-zag 5 m/s", b, 10);
    CHECK(b.warps == 0);
    CHECK(b.maxPathDev < 0.8f && b.meanErr < 0.2f);

    // A horse galloping a tight circle (12 m/s, radius 25 m).
    auto gallop = [](float t) {
        float w = 12.0f / 25.0f;
        return Vec3{25 * std::cos(w * t), 25 * std::sin(w * t), 0};
    };
    TrackStats c = trackPath(gallop, 10, 50, 40, 0.05);
    printTrack("gallop 12 m/s r=25", c, 10);
    CHECK(c.warps == 0);
    CHECK(c.meanErr < 0.8f && c.maxErr < 1.2f);  // was 1.6 / 2.5 m: sprinting ahead at 1.6x speed
}

COOP_TEST(testSyncStopPrecision, "sync: stopping precision (no overshoot, exact rest)") {
    // Run 5 m/s along +x, stop dead; then a walk (1.4 m/s) and stop again.
    Pair p;
    p.net.latencyMs = 50;
    p.net.jitterMs = 40;
    p.net.loss = 0.05;
    p.hostGame.player.pos = {0, -30, 0};
    p.guestGame.player.pos = {0, 0, 0};
    p.start();
    p.run(1500);
    float speed = 5.0f;
    bool moving = true;
    p.perFrame = [&] {
        if (moving) {
            p.guestGame.player.pos.x += speed * 0.016f;
            p.guestGame.player.vel = {speed, 0, 0};
            p.guestGame.player.heading = -90;
        } else {
            p.guestGame.player.vel = {};
        }
    };
    EntityHandle puppet = p.hostGame.find(SpawnRole::PlayerPuppet);
    CHECK(puppet != 0);
    float worstOvershoot = 0, worstRest = 0;
    for (float v : {5.0f, 1.4f}) {
        speed = v;
        moving = true;
        p.run(3000);
        moving = false;
        float stopX = p.guestGame.player.pos.x, overshoot = 0;
        int teleports = p.hostGame.ents[puppet].teleports;
        for (int i = 0; i < 100; ++i) {
            p.run(16);
            overshoot = std::max(overshoot, p.hostGame.ents[puppet].pos.x - stopX);
        }
        float rest = distance(p.hostGame.ents[puppet].pos, p.guestGame.player.pos);
        std::printf("  stop from %.1f m/s: overshoot %.2f m, rest error %.3f m, warps %d\n", v, overshoot, rest,
                    p.hostGame.ents[puppet].teleports - teleports);
        worstOvershoot = std::max(worstOvershoot, overshoot);
        worstRest = std::max(worstRest, rest);
        CHECK(p.hostGame.ents[puppet].teleports == teleports);
    }
    CHECK(worstOvershoot < 0.15f);  // was 0.31 m for a walk (a lost stop update meant 200 ms of guessing)
    CHECK(worstRest < 0.15f);       // was 0.31 m: idle peds stayed wherever they were within 0.5 m
}

COOP_TEST(testSyncLossBursts, "sync: extrapolation through loss bursts") {
    // A rider circles at 8 m/s (30 Hz, 60-140 ms latency); every 4 s the network drops everything
    // for a while. Measure how far playback strays during the gap and how hard it snaps back.
    for (uint32_t burst : {300u, 600u}) {
        std::mt19937 rng(11);
        MotionBuffer buf;
        const float radius = 20.0f, speed = 8.0f, omega = speed / radius;
        auto truth = [&](float t) { return Vec3{radius * std::cos(omega * t), radius * std::sin(omega * t), 0}; };
        auto truthVel = [&](float t) {
            return Vec3{-radius * omega * std::sin(omega * t), radius * omega * std::cos(omega * t), 0};
        };
        struct InFlight {
            uint32_t at;
            MotionSample s;
        };
        std::vector<InFlight> flight;
        float maxErr = 0, maxStep = 0, sumErr = 0;
        int frames = 0;
        Vec3 prev;
        bool havePrev = false;
        for (uint32_t local = 0; local < 30000; local += 16) {
            uint32_t sender = local + 5000;
            bool dropped = local > 3000 && (local % 4000) < burst;
            if (local % 33 < 16 && !dropped) {
                float t = sender / 1000.0f;
                MotionSample s;
                s.timeMs = sender;
                s.pos = truth(t);
                s.vel = truthVel(t);
                s.hasVel = true;
                s.heading = std::atan2(-s.vel.x, s.vel.y) * 180.0f / 3.14159265f;
                flight.push_back({local + 60 + static_cast<uint32_t>(rng() % 81), s});
            }
            for (size_t i = 0; i < flight.size();) {
                if (flight[i].at <= local) {
                    buf.push(flight[i].s, local);
                    flight.erase(flight.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
            if (buf.empty() || local < 2000) continue;
            MotionSample out = buf.sample(local);
            float err = distance(out.pos, truth(out.timeMs / 1000.0f));
            maxErr = std::max(maxErr, err);
            sumErr += err;
            ++frames;
            if (havePrev) maxStep = std::max(maxStep, distance(out.pos, prev));
            prev = out.pos;
            havePrev = true;
        }
        std::printf("  loss burst %u ms: max error %.2f m, mean %.3f m, max frame step %.3f m (normal %.3f)\n", burst,
                    maxErr, sumErr / frames, maxStep, speed * 0.016f);
        CHECK(maxStep < speed * 0.016f * 2);                 // was 0.66 / 2.9 m: a snap back onto the path
        CHECK(maxErr < (burst < 400 ? 0.25f : 1.2f));       // was 0.52 / 2.8 m: a straight guess, then a stop
    }
}

COOP_TEST(testSyncClockDrift, "sync: clocks running at different rates") {
    // The sender's clock runs 0.5% fast or slow (far worse than real crystals) for two minutes. The
    // buffer must keep a steady delay: no creeping latency, no running dry.
    for (double rate : {1.005, 0.995}) {
        std::mt19937 rng(5);
        MotionBuffer buf;
        struct InFlight {
            uint32_t at;
            MotionSample s;
        };
        std::vector<InFlight> flight;
        float maxErr = 0;
        uint32_t minDelay = 100000, maxDelay = 0;
        int extrapolated = 0, frames = 0;
        uint32_t nextSend = 0;
        for (uint32_t local = 0; local < 120000; local += 16) {
            if (local >= nextSend) {
                nextSend += 33;
                MotionSample s;
                s.timeMs = static_cast<uint32_t>(local * rate) + 777777;
                float t = local / 1000.0f;  // true time
                s.pos = {3.0f * t, 0, 0};
                s.vel = {3.0f, 0, 0};
                s.hasVel = true;
                flight.push_back({local + 40 + static_cast<uint32_t>(rng() % 41), s});
            }
            for (size_t i = 0; i < flight.size();) {
                if (flight[i].at <= local) {
                    buf.push(flight[i].s, local);
                    flight.erase(flight.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
            if (buf.empty()) continue;
            MotionSample out = buf.sample(local);
            if (local < 5000) continue;
            ++frames;
            if (static_cast<int32_t>(out.timeMs - buf.latest().timeMs) > 0) ++extrapolated;
            // Position must match the sender's clock at the time shown.
            float shownTrue = static_cast<float>((out.timeMs - 777777) / rate) / 1000.0f;
            maxErr = std::max(maxErr, std::fabs(out.pos.x - 3.0f * shownTrue));
            uint32_t behind = static_cast<uint32_t>((local - (out.timeMs - 777777) / rate));
            minDelay = std::min(minDelay, behind);
            maxDelay = std::max(maxDelay, behind);
        }
        std::printf("  clock rate %.3f: shown %u..%u ms behind, %.1f%% frames past the newest sample, max error %.3f m\n",
                    rate, minDelay, maxDelay, 100.0 * extrapolated / frames, maxErr);
        CHECK(maxDelay < 260 && minDelay > 40);
        CHECK(extrapolated < frames / 50);
        CHECK(maxErr < 0.05f);
    }
}

COOP_TEST(testSyncCpuBenchmark, "sync: cpu per frame with 240 NPCs and 4 players") {
    // Owner + 3 viewers spread over the map; 240 NPCs, a third of them walking.
    Cluster c;
    c.config.maxPeds = 96;
    c.config.npcRadius = 150;
    Player& owner = c.join("Owner", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    Player& e = c.join("D");
    owner.game.player.pos = {0, 0, 0};
    b.game.player.pos = {60, 0, 0};
    d.game.player.pos = {0, 60, 0};
    e.game.player.pos = {-60, -40, 0};
    std::mt19937 rng(9);
    std::vector<EntityHandle> walkers;
    for (int i = 0; i < 240; ++i) {
        Vec3 pos{static_cast<float>(rng() % 300) - 150.0f, static_cast<float>(rng() % 300) - 150.0f, 0};
        EntityHandle h = owner.game.addPed(joaat(i % 3 ? "a_m_m_idle" : "a_m_m_walker"), pos);
        if (i % 3 == 0) walkers.push_back(h);
    }
    for (int i = 0; i < 12; ++i)
        owner.game.add(EntityKind::Vehicle, joaat("wagon02x"), {static_cast<float>(i * 10 - 60), 20, 0});
    c.perFrame = [&] {
        for (EntityHandle h : walkers) {
            owner.game.ents[h].pos.x += 1.4f * 0.016f;
            owner.game.ents[h].vel = {1.4f, 0, 0};
        }
    };
    c.run(4000);

    struct Load {
        std::vector<double> us;  // per frame (viewers: summed over the three)
        long nearby = 0, reads = 0, queries = 0, exists = 0;
        double typical() {  // mean without the slowest 5% of frames (the machine may be busy)
            std::sort(us.begin(), us.end());
            size_t n = us.size() - us.size() / 20;
            double sum = 0;
            for (size_t i = 0; i < n; ++i) sum += us[i];
            return n ? sum / n : 0;
        }
    } ownerLoad, viewerLoad;
    auto queries = [](FakeGame& g) {
        long n = 0;
        for (auto& [h, en] : g.ents) n += en.queries;
        return n;
    };
    const int frames = 300;
    std::vector<long> q0;
    for (Player* p : c.players) q0.push_back(queries(p->game));
    std::vector<int> n0, r0, x0;
    for (Player* p : c.players) {
        n0.push_back(p->game.nearbyCalls);
        r0.push_back(p->game.readCalls);
        x0.push_back(p->game.existsCalls);
    }
    for (int f = 0; f < frames; ++f) {
        c.now += 16;
        c.net.now = c.now;
        c.perFrame();
        c.server->pump(c.now);
        double viewerUs = 0;
        for (Player* p : c.players) {
            auto start = std::chrono::steady_clock::now();
            p->session->tick(c.now);
            double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            if (p == &owner) ownerLoad.us.push_back(us);
            else viewerUs += us;
        }
        viewerLoad.us.push_back(viewerUs);
        for (Player* p : c.players) p->game.simulate(0.016f);
    }
    for (size_t i = 0; i < c.players.size(); ++i) {
        Player* p = c.players[i];
        Load& l = p == &owner ? ownerLoad : viewerLoad;
        l.nearby += p->game.nearbyCalls - n0[i];
        l.reads += p->game.readCalls - r0[i];
        l.exists += p->game.existsCalls - x0[i];
        l.queries += queries(p->game) - q0[i];
    }
    const double viewers = 3.0;
    // Times are trimmed means; the query counts are exact.
    std::printf("  owner per frame: %.1f us, %.2f listNearby, %.1f reads, %.1f position/health, %.1f exists\n",
                ownerLoad.typical(), ownerLoad.nearby / double(frames), ownerLoad.reads / double(frames),
                ownerLoad.queries / double(frames), ownerLoad.exists / double(frames));
    std::printf("  viewer per frame: %.1f us, %.1f position/health, %.1f exists (%d, %d, %d mirrors)\n",
                viewerLoad.typical() / viewers, viewerLoad.queries / frames / viewers,
                viewerLoad.exists / frames / viewers, b.session->status().syncedEntities, d.session->status().syncedEntities,
                e.session->status().syncedEntities);
    CHECK(b.session->status().syncedEntities > 30 && d.session->status().syncedEntities > 30);
    // One world query per pair of players (was one per viewer, each also around the owner), and
    // mirrors skipped by the CPU LOD or standing still cost no game queries (was 0.60 / 92 / 111).
    CHECK(ownerLoad.nearby <= frames / 2 + 2);
    CHECK(viewerLoad.queries / frames / viewers < 75 && viewerLoad.exists / frames / viewers < 70);
}

COOP_TEST(testSyncHoldingRate, "sync: a held player is not resent every frame") {
    // The cutscene barrier holds the host (it finished its scene first). Its state carries the
    // holding flag, which used to make every frame look like a change: ~60 updates a second.
    Pair p;
    p.hostGame.player.pos = {0, 0, 0};
    p.guestGame.player.pos = {100, 0, 0};
    p.start();
    p.run(2000);
    uint32_t idle = p.hostLink.sendRate();
    p.hostGame.player.flags = PF_InMission | PF_Cutscene;
    p.hostGame.missionScript = joaat("mission_a");
    p.run(1000);
    p.hostGame.player.flags = PF_InMission;
    p.run(300);
    CHECK(p.host->status().holding);
    p.run(2000);
    uint32_t held = p.hostLink.sendRate();
    CHECK(p.host->status().holding);
    std::printf("  held player: host sends %u B/s (standing, not held: %u B/s)\n", held, idle);
    CHECK(held < idle * 2 + 200);
}

COOP_TEST(testSyncPassengerInFriendsVehicle, "sync: three players, passengers in a player's own wagon") {
    const uint32_t kWagon = joaat("wagon02x"), kDutch = joaat("cs_dutch");
    Cluster c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    a.game.player.pos = {0, 5, 0};
    b.game.player.pos = {3, 0, 0};
    d.game.player.pos = {6, 0, 0};
    // A drives a wagon of its own with Dutch beside it (a story ride).
    EntityHandle wagon = a.game.add(EntityKind::Vehicle, kWagon, {0, 5, 0});
    EntityHandle dutch = a.game.addPed(kDutch, {0, 5, 1});
    a.game.ents[dutch].vehicle = wagon;
    a.game.ents[dutch].seat = 0;
    a.game.playerVehicle = wagon;
    a.game.playerSeat = -1;
    c.run(2000);
    EntityHandle wagonOnB = b.game.find(SpawnRole::PuppetVehicle, kWagon, EntityKind::Vehicle);
    EntityHandle wagonOnC = d.game.find(SpawnRole::PuppetVehicle, kWagon, EntityKind::Vehicle);
    CHECK(wagonOnB && wagonOnC);
    // Dutch sits in A's wagon everywhere, not next to it.
    EntityHandle dutchOnC = mirrorOf(d.game, kDutch);
    CHECK(dutchOnC && d.game.ents[dutchOnC].vehicle == wagonOnC && d.game.ents[dutchOnC].seat == 0);

    // B climbs onto A's wagon (B's copy of it) and rides along while A drives.
    b.game.playerVehicle = wagonOnB;
    b.game.playerSeat = 1;
    c.perFrame = [&] {
        a.game.ents[wagon].pos.y += 4 * 0.016f;
        a.game.ents[wagon].vel = {0, 4, 0};
        a.game.player.pos = a.game.ents[wagon].pos;
        a.game.player.vel = {0, 4, 0};
        if (b.game.ents.count(wagonOnB)) {
            b.game.player.pos = b.game.ents[wagonOnB].pos;
            b.game.player.vel = b.game.ents[wagonOnB].vel;
        }
    };
    c.run(3000);
    c.perFrame = nullptr;
    EntityHandle bOnA = a.game.find(SpawnRole::PlayerPuppet, joaat("player_B"));
    EntityHandle bOnC = d.game.find(SpawnRole::PlayerPuppet, joaat("player_B"));
    EntityHandle aOnC = d.game.find(SpawnRole::PlayerPuppet, joaat("player_A"));
    CHECK(bOnA && bOnC && aOnC);
    // One wagon everywhere: B sits in A's real one, and in A's copy at C.
    CHECK(a.game.count(SpawnRole::PuppetVehicle, EntityKind::Vehicle) == 0);
    CHECK(d.game.count(SpawnRole::PuppetVehicle, EntityKind::Vehicle) == 1);
    CHECK(a.game.ents[bOnA].vehicle == wagon && a.game.ents[bOnA].seat == 1);
    CHECK(d.game.ents[bOnC].vehicle == wagonOnC && d.game.ents[bOnC].seat == 1);
    CHECK(d.game.ents[aOnC].vehicle == wagonOnC && d.game.ents[aOnC].seat == -1);
    CHECK(b.game.ents.count(wagonOnB) && distance(b.game.ents[wagonOnB].pos, a.game.ents[wagon].pos) < 1.5f);

    // A stops and gets down; B and Dutch stay on. A's game now streams the wagon as part of its
    // world: everybody keeps the one wagon they had (now a mirror), nobody gets a second one.
    a.game.ents[wagon].vel = {};
    a.game.player.vel = b.game.player.vel = {};
    a.game.playerVehicle = 0;
    a.game.player.pos = a.game.ents[wagon].pos + Vec3{2, 0, 0};
    c.run(1500);
    auto wagons = [&](FakeGame& g) {
        int n = 0;
        for (auto& [h, e] : g.ents) n += e.kind == EntityKind::Vehicle && e.model == kWagon && !e.parked;
        return n;
    };
    CHECK(wagons(a.game) == 1 && wagons(b.game) == 1 && wagons(d.game) == 1);
    CHECK(b.game.playerVehicle == wagonOnB && b.game.ents.count(wagonOnB));
    CHECK(d.game.ents.count(wagonOnC) && d.game.ents[bOnC].vehicle == wagonOnC && d.game.ents[dutchOnC].vehicle == wagonOnC);
    CHECK(a.game.ents[bOnA].vehicle == wagon);
    CHECK(d.game.ents[aOnC].vehicle == 0);
}

COOP_TEST(testSyncOwnerTakesWorldWagon, "sync: the world owner takes a wagon everybody sees") {
    // The owner climbs onto a wagon of its world and drives off. It used to vanish for the others
    // (the game no longer lists the player's own vehicle) and a copy popped up in its place.
    const uint32_t kWagon = joaat("wagon02x");
    Pair p;
    p.hostGame.player.pos = {0, 0, 0};
    p.guestGame.player.pos = {5, 0, 0};
    EntityHandle wagon = p.hostGame.add(EntityKind::Vehicle, kWagon, {0, 4, 0});
    p.start();
    p.run(1500);
    EntityHandle mirror = mirrorOf(p.guestGame, kWagon, EntityKind::Vehicle);
    CHECK(mirror != 0);
    int spawns = p.guestGame.spawns;
    p.hostGame.playerVehicle = wagon;
    p.hostGame.playerSeat = -1;
    p.perFrame = [&] {
        p.hostGame.ents[wagon].pos.y += 5 * 0.016f;
        p.hostGame.ents[wagon].vel = {0, 5, 0};
        p.hostGame.player.pos = p.hostGame.ents[wagon].pos;
        p.hostGame.player.vel = {0, 5, 0};
    };
    p.run(2500);
    EntityHandle hostPuppet = p.guestGame.find(SpawnRole::PlayerPuppet);
    CHECK(p.guestGame.ents.count(mirror) && !p.guestGame.ents[mirror].parked);
    CHECK(p.guestGame.count(SpawnRole::PuppetVehicle, EntityKind::Vehicle) == 0 && p.guestGame.spawns == spawns);
    CHECK(hostPuppet && p.guestGame.ents[hostPuppet].vehicle == mirror);
    CHECK(distance(p.guestGame.ents[mirror].pos, p.hostGame.ents[wagon].pos) < 1.5f);
    CHECK(p.guestGame.ents[mirror].teleports == 0);
}

namespace {
// B drives (or rides) an entity of A's world in a circle; how far behind B is C's copy of it?
struct ControlLag {
    float meanLag = 0, maxLag = 0;
    bool seated = false;
    float ownerError = 0;
};
ControlLag thirdPlayerSeesControl(EntityKind kind) {
    const uint32_t model = kind == EntityKind::Vehicle ? joaat("wagon02x") : joaat("a_c_horse_morgan_bay");
    Cluster c;
    c.net.latencyMs = 40;
    c.net.jitterMs = 20;
    c.net.loss = 0.02;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    a.game.player.pos = {-20, 0, 0};
    b.game.player.pos = {18, 0, 0};
    d.game.player.pos = {0, -15, 0};
    EntityHandle real = a.game.add(kind, model, {20, 0, 0});
    c.run(2000);
    ControlLag out;
    EntityHandle onB = b.game.find(SpawnRole::Mirror, model, kind), onC = d.game.find(SpawnRole::Mirror, model, kind);
    if (!onB || !onC) return out;
    if (kind == EntityKind::Vehicle) {
        b.game.playerVehicle = onB;
        b.game.playerSeat = -1;
    } else {
        b.game.playerMount = onB;
    }
    float t = 0;
    const float r = 20, v = 6, w = v / r;
    c.perFrame = [&] {
        t += 0.016f;
        FakeGame::Ent& e = b.game.ents[onB];  // B's game moves what B drives
        e.pos = {r * std::cos(w * t), r * std::sin(w * t), 0};
        e.vel = {-v * std::sin(w * t), v * std::cos(w * t), 0};
        e.heading = std::atan2(-e.vel.x, e.vel.y) * 57.29578f;
        b.game.player.pos = e.pos;
        b.game.player.vel = e.vel;
        b.game.player.heading = e.heading;
    };
    c.run(3000);
    float sum = 0;
    int n = 0;
    for (int i = 0; i < 300; ++i) {
        c.run(16);
        float lag = distance(d.game.ents[onC].pos, b.game.ents[onB].pos);
        out.maxLag = std::max(out.maxLag, lag);
        sum += lag;
        ++n;
    }
    out.meanLag = sum / n;
    out.ownerError = distance(a.game.ents[real].pos, b.game.ents[onB].pos);
    EntityHandle bOnC = d.game.find(SpawnRole::PlayerPuppet, joaat("player_B"));
    out.seated = bOnC && (kind == EntityKind::Vehicle ? d.game.ents[bOnC].vehicle : d.game.ents[bOnC].mount) == onC;
    return out;
}
}  // namespace

COOP_TEST(testSyncControlSeenByThirdPlayer, "sync: three players, a player driving the owner's wagon / horse") {
    // B drives a wagon and rides a horse of A's world at 6 m/s. A's real one follows B; C's copy
    // used to follow A's echo of it (B -> server -> A -> server -> C), now B's own stream.
    for (EntityKind kind : {EntityKind::Vehicle, EntityKind::Ped}) {
        ControlLag l = thirdPlayerSeesControl(kind);
        std::printf("  %s driven by B, seen by C: %.2f m behind B on average (max %.2f); A's real one %.2f m off\n",
                    kind == EntityKind::Vehicle ? "wagon" : "horse", l.meanLag, l.maxLag, l.ownerError);
        CHECK(l.seated);
        CHECK(l.meanLag < 1.6f);
        CHECK(l.ownerError < 2.5f);
    }
}

COOP_TEST(testSyncRideSurvivesWorldChanges, "sync: three players, rides survive the owner boarding, removal, handover") {
    const uint32_t kWagon = joaat("wagon02x"), kHorse = joaat("a_c_horse_morgan_bay");
    Cluster c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {10, 0, 0};
    d.game.player.pos = {14, 3, 0};
    EntityHandle wagon = a.game.add(EntityKind::Vehicle, kWagon, {10, 2, 0});
    EntityHandle horse = a.game.addPed(kHorse, {14, 2, 0});
    c.run(2000);
    EntityHandle wagonOnB = mirrorOf(b.game, kWagon, EntityKind::Vehicle);
    EntityHandle horseOnC = mirrorOf(d.game, kHorse);
    CHECK(wagonOnB && horseOnC);
    // B drives A's wagon, C rides A's horse.
    b.game.playerVehicle = wagonOnB;
    b.game.playerSeat = -1;
    d.game.playerMount = horseOnC;
    c.perFrame = [&] {
        if (b.game.playerVehicle) {
            b.game.ents[b.game.playerVehicle].pos.x += 3 * 0.016f;
            b.game.ents[b.game.playerVehicle].vel = {3, 0, 0};
            b.game.player.pos = b.game.ents[b.game.playerVehicle].pos;
            b.game.player.vel = {3, 0, 0};
        }
        if (d.game.playerMount) {
            d.game.ents[d.game.playerMount].pos.y += 2 * 0.016f;
            d.game.ents[d.game.playerMount].vel = {0, 2, 0};
            d.game.player.pos = d.game.ents[d.game.playerMount].pos;
            d.game.player.vel = {0, 2, 0};
        }
        if (a.game.playerVehicle) a.game.player.pos = a.game.ents[a.game.playerVehicle].pos;
    };
    c.run(1500);

    // A climbs onto the wagon B drives. For A's game it is now the player's vehicle (no longer
    // listed for streaming); it must stay in the stream, or B's copy vanishes under B.
    a.game.playerVehicle = wagon;
    a.game.playerSeat = 1;
    c.run(1500);
    CHECK(b.game.playerVehicle == wagonOnB && b.game.ents.count(wagonOnB) && !b.game.ents[wagonOnB].parked);
    EntityHandle aOnB = b.game.find(SpawnRole::PlayerPuppet, joaat("player_A"));
    CHECK(aOnB && b.game.ents[aOnB].vehicle == wagonOnB);
    CHECK(distance(a.game.ents[wagon].pos, b.game.ents[wagonOnB].pos) < 2.0f);  // B still drives it for A
    a.game.playerVehicle = 0;
    c.run(500);

    // A's game deletes the horse (a script cleans up): C keeps riding its copy, now C's own horse.
    a.game.remove(horse);
    c.run(1000);
    CHECK(d.game.playerMount == horseOnC && d.game.ents.count(horseOnC) && !d.game.ents[horseOnC].ours);
    EntityHandle cOnB = b.game.find(SpawnRole::PlayerPuppet, joaat("player_C"));
    EntityHandle cHorseOnB = b.game.find(SpawnRole::PuppetMount, kHorse);
    CHECK(cOnB && cHorseOnB && b.game.ents[cOnB].mount == cHorseOnB);

    // A leaves: B owns the world now, still on the wagon, which is part of B's world from now on.
    dropPlayer(c, a);
    c.run(2500);
    CHECK(b.session->status().worldOwner);
    CHECK(b.game.playerVehicle == wagonOnB && b.game.ents.count(wagonOnB) && !b.game.ents[wagonOnB].ours);
    CHECK(b.game.adoptions == 1 && d.game.adoptions == 1);
    EntityHandle bOnC = d.game.find(SpawnRole::PlayerPuppet, joaat("player_B"));
    EntityHandle wagonOnC = d.game.find(SpawnRole::PuppetVehicle, kWagon, EntityKind::Vehicle);
    CHECK(bOnC && wagonOnC && d.game.ents[bOnC].vehicle == wagonOnC);
    CHECK(wagonOnC && distance(d.game.ents[wagonOnC].pos, b.game.ents[wagonOnB].pos) < 2.0f);
    c.perFrame = nullptr;
}

COOP_TEST(testSyncSenderClockGoesBack, "sync: playback follows a sender whose clock went back") {
    // Machines' clocks are unrelated. When the samples for one entity start coming from a clock
    // far behind (the sender's game restarted, or another machine now sends them), the buffer used
    // to drop every one of them as "out of order", freezing the entity for good.
    MotionBuffer buf;
    uint32_t local = 0;
    auto feed = [&](uint32_t sender, float x) {
        MotionSample s;
        s.timeMs = sender;
        s.pos = {x, 0, 0};
        s.vel = {3, 0, 0};
        s.hasVel = true;
        buf.push(s, local);
    };
    for (; local < 2000; local += 33) feed(900000 + local, local * 0.003f);
    buf.sample(local);
    // One stale packet (a late resend) is ignored: no restart, no jump.
    feed(890000, 999.0f);
    MotionSample kept = buf.sample(local);
    CHECK(kept.pos.x < 10.0f && buf.latest().timeMs >= 900000);
    // A steady stream on a new clock takes over within a few samples.
    for (uint32_t end = local + 1500; local < end; local += 33) {
        feed(20000 + local, 100.0f + local * 0.003f);
        buf.sample(local);
    }
    MotionSample now = buf.sample(local);
    std::printf("  clock went back: following again at x=%.2f (sent %.2f)\n", now.pos.x, 100.0f + local * 0.003f);
    CHECK(buf.latest().timeMs < 900000);
    CHECK(std::fabs(now.pos.x - (100.0f + local * 0.003f)) < 0.5f);
}

COOP_TEST(testSyncMissionPairing, "sync: three players, mission NPCs paired with their own twins") {
    // Two enemies of the same type stand 3 m apart at the owner; in the other games their copies
    // spawned a little off (their AI differs). Each copy must be linked to its own twin, whatever
    // order the session happens to look at them in, so two mirrored groups are used.
    const uint32_t kScript = joaat("mission_pairs");
    Cluster c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    a.game.player.pos = {50, 0, 0};
    b.game.player.pos = {50, 2, 0};
    d.game.player.pos = {50, -2, 0};
    auto missionPed = [&](FakeGame& g, uint32_t model, Vec3 pos) {
        EntityHandle h = g.addPed(model, pos);
        g.ents[h].mission = true;
        g.ents[h].script = kScript;
        return h;
    };
    const uint32_t kOutlaw = joaat("g_m_m_uniduster_01"), kGuard = joaat("s_m_m_ambientlawrural_01");
    missionPed(a.game, kOutlaw, {40, 10, 0});
    missionPed(a.game, kOutlaw, {43, 10, 0});
    missionPed(a.game, kGuard, {54, 10, 0});
    missionPed(a.game, kGuard, {57, 10, 0});
    struct Twin {
        EntityHandle h;
        Vec3 home;  // where its original stands at the owner
    };
    std::vector<std::pair<Player*, std::vector<Twin>>> copies;
    for (Player* p : {&b, &d}) {
        std::vector<Twin> twins;
        twins.push_back({missionPed(p->game, kOutlaw, {37, 10, 0}), {40, 10, 0}});  // 3 m off its original
        twins.push_back({missionPed(p->game, kOutlaw, {42, 10, 0}), {43, 10, 0}});  // 2 m from the other one
        twins.push_back({missionPed(p->game, kGuard, {51, 10, 0}), {54, 10, 0}});  // the same, mirrored
        twins.push_back({missionPed(p->game, kGuard, {56, 10, 0}), {57, 10, 0}});
        copies.push_back({p, twins});
    }
    c.run(4000);
    for (auto& [p, twins] : copies) {
        CHECK(p->game.find(SpawnRole::Mirror, kOutlaw) == 0 && p->game.find(SpawnRole::Mirror, kGuard) == 0);
        for (const Twin& t : twins) {
            CHECK(p->game.ents[t.h].harmless);
            CHECK(distance(p->game.ents[t.h].pos, t.home) < 1.0f);
        }
    }
}

COOP_TEST(testSyncDamageOwnership, "sync: three players, only real hits reach the owner's NPCs; friendly fire once") {
    const uint32_t kOutlaw = joaat("g_m_m_uniduster_01");
    Cluster c;
    c.config.friendlyFire = true;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {5, 0, 0};
    d.game.player.pos = {0, 5, 0};
    EntityHandle outlaw = a.game.addPed(kOutlaw, {4, 4, 0});
    c.run(2000);
    EntityHandle onB = mirrorOf(b.game, kOutlaw), onC = mirrorOf(d.game, kOutlaw);
    CHECK(onB && onC);
    // B's copy gets hurt by B's own game (a fall after a correction, a bump from another copy):
    // the owner simulates such things with the real one, so it must not count a second time.
    b.game.ents[onB].health -= 30;
    c.run(800);
    CHECK(a.game.ents[outlaw].health == 100);
    CHECK(b.game.ents[onB].health == 100);  // the owner's value is restored
    // C really shoots it: that counts, once, and everybody's copy shows it.
    d.game.playerShoots(onC, 25);
    c.run(800);
    CHECK(a.game.ents[outlaw].health == 75 && b.game.ents[onB].health == 75 && d.game.ents[onC].health == 75);
    // Friendly fire: C shoots B's character; B loses exactly that.
    EntityHandle bOnC = d.game.find(SpawnRole::PlayerPuppet, joaat("player_B"));
    CHECK(bOnC != 0);
    d.game.playerShoots(bOnC, 40);
    c.run(800);
    CHECK(b.game.playerHealth == 160 && a.game.playerHealth == 200 && d.game.playerHealth == 200);
}
