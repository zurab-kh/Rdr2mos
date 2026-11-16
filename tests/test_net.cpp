// Network, protocol and server: throughput, server cost, and hostile input.
#include <chrono>

#include "harness.h"

namespace {

// Two raw links on a simulated network: one side sends `bytes` the way a save upload does (960-byte
// chunks, at most `window` unacknowledged), the other receives. Returns the simulated time it took.
struct TransferResult {
    uint32_t ms = 0;
    uint64_t wireBytes = 0;  // everything the sender put on the network
    bool intact = false;
};

TransferResult transfer(double loss, uint32_t latencyMs, uint32_t jitterMs, size_t bytes, size_t window = 48,
                        uint32_t frameMs = 16) {
    MemoryNetwork net;
    net.loss = 0;
    net.latencyMs = latencyMs;
    net.jitterMs = jitterMs;
    auto* a = net.open(kHostAddr.ip, kHostAddr.port);
    auto* b = net.open(kGuestAddr.ip, kGuestAddr.port);
    Link host, guest;
    host.start(a, hostConfig(), 0);
    guest.start(b, guestConfig(), 0);
    uint32_t now = 0;
    for (; now < 2000 && !(host.connected() && guest.connected()); now += frameMs) {
        net.now = now;
        host.update(now);
        guest.update(now);
        host.flush(now);
        guest.flush(now);
    }
    net.loss = loss;

    std::vector<uint8_t> data(bytes);
    for (size_t i = 0; i < bytes; ++i) data[i] = static_cast<uint8_t>(i * 131 + (i >> 9));
    std::vector<uint8_t> got;
    size_t offset = 0;
    TransferResult res;
    const uint64_t sentBefore = b->bytesSent;
    const uint32_t start = now;
    for (; now < start + 600000 && got.size() < bytes; now += frameMs) {
        net.now = now;
        host.update(now);
        guest.update(now);
        while (offset < bytes && guest.pendingReliable() < window) {
            SaveChunk c;
            c.total = static_cast<uint32_t>(bytes);
            c.offset = static_cast<uint32_t>(offset);
            size_t n = std::min(kSaveChunkBytes, bytes - offset);
            c.data = std::vector<uint8_t>(data.data() + offset, data.data() + offset + n);
            guest.send(Msg::SaveChunk, c, true);
            offset += n;
        }
        Msg type;
        std::vector<uint8_t> payload;
        while (host.poll(type, payload)) {
            SaveChunk c;
            if (type == Msg::SaveChunk && decode(payload, c) && c.offset == got.size())
                got.insert(got.end(), c.data.begin(), c.data.end());
        }
        host.flush(now);
        guest.flush(now);
    }
    res.ms = now - start;
    res.wireBytes = b->bytesSent - sentBefore;
    res.intact = got == data;
    return res;
}

double kbps(size_t bytes, uint32_t ms) { return ms ? bytes / 1024.0 / (ms / 1000.0) : 0; }

}  // namespace

COOP_TEST(testNetSaveThroughput, "net: save transfer throughput (lossless, 5% loss, long RTT)") {
    const size_t kBytes = 1u << 20;
    TransferResult clean = transfer(0.0, 40, 10, kBytes);
    TransferResult lossy = transfer(0.05, 40, 10, kBytes);
    TransferResult lossy2 = transfer(0.05, 40, 10, kBytes, 48, 33);  // a 30 fps game on both ends
    TransferResult far = transfer(0.02, 110, 30, kBytes);             // ~250 ms RTT
    TransferResult steady = transfer(0.05, 45, 0, kBytes);            // no reordering
    std::printf("  save 1 MB, RTT ~90 ms: lossless %.0f KB/s, 5%% loss %.0f KB/s (30 fps: %.0f KB/s, no jitter: %.0f KB/s); "
                "RTT ~250 ms + 2%% loss %.0f KB/s; wire bytes per byte %.2f / %.2f / %.2f\n",
                kbps(kBytes, clean.ms), kbps(kBytes, lossy.ms), kbps(kBytes, lossy2.ms), kbps(kBytes, steady.ms),
                kbps(kBytes, far.ms), clean.wireBytes / double(kBytes), lossy.wireBytes / double(kBytes),
                far.wireBytes / double(kBytes));
    CHECK(clean.intact && lossy.intact && lossy2.intact && far.intact && steady.intact);
    // Go-back-N with a fixed 150 ms resend managed 10-12 KB/s in every one of these cases (jitter
    // reorders datagrams, and every out-of-order record was thrown away) at 23-28 bytes per byte.
    CHECK(kbps(kBytes, clean.ms) > 300 && kbps(kBytes, lossy.ms) > 200 && kbps(kBytes, lossy2.ms) > 150);
    CHECK(kbps(kBytes, steady.ms) > 200 && kbps(kBytes, far.ms) > 100);
    for (const TransferResult* r : {&clean, &lossy, &far, &steady}) CHECK(r->wireBytes < kBytes * 5 / 4);
}

COOP_TEST(testNetServerPumpCost, "net: server pump cost with 8 players streaming") {
    Cluster c;
    c.config.maxPeds = 80;
    c.config.npcRadius = 200;
    std::vector<Player*> ps;
    ps.push_back(&c.join("Owner", true));
    c.run(200);
    for (int i = 1; i < 8; ++i) ps.push_back(&c.join("P" + std::to_string(i)));
    std::vector<EntityHandle> npcs;
    for (int i = 0; i < 80; ++i) npcs.push_back(ps[0]->game.addPed(joaat("walker"), {float(i % 10) * 4, float(i / 10) * 4, 0}));
    float t = 0;
    c.perFrame = [&] {
        t += 0.016f;
        for (size_t i = 0; i < npcs.size(); ++i) {
            auto& e = ps[0]->game.ents[npcs[i]];
            e.pos.x += 1.4f * 0.016f * std::cos(t + float(i));
            e.pos.y += 1.4f * 0.016f * std::sin(t + float(i));
            e.vel = {1.4f * std::cos(t + float(i)), 1.4f * std::sin(t + float(i)), 0};
        }
        for (size_t i = 0; i < ps.size(); ++i) {
            ps[i]->game.player.pos = {10.0f + 5.0f * float(i) + 3.0f * std::cos(t), 10.0f + 3.0f * std::sin(t), 0};
            ps[i]->game.player.vel = {-3.0f * std::sin(t), 3.0f * std::cos(t), 0};
        }
    };
    c.run(3000);
    CHECK(c.server->playerCount() == 8);

    // Time only the server's work; the simulated games and their sessions run outside the clock.
    using Clock = std::chrono::steady_clock;
    Clock::duration spent{};
    int pumps = 0;
    const uint32_t kMs = 10000;
    for (uint32_t end = c.now + kMs; c.now < end; c.now += 16) {
        c.net.now = c.now;
        c.perFrame();
        auto t0 = Clock::now();
        c.server->pump(c.now);
        spent += Clock::now() - t0;
        ++pumps;
        for (Player* p : c.players) p->session->tick(c.now);
        for (Player* p : c.players) p->game.simulate(0.016f);
    }
    // Measured on this scenario before the per-message copies went (withSource, a queue entry per
    // datagram, one vector per forwarded message and recipient): 7.8-9.5 us per pump, ~30 allocations
    // per pump. The time includes the simulated network's own sends.
    double us = std::chrono::duration<double, std::micro>(spent).count();
    uint64_t datagrams = c.server->stats().datagrams;
    std::printf("  server: 8 players + 80 NPCs, %.1f us per pump (%.2f ms CPU per second of play, %.0f datagrams in "
                "per second)\n",
                us / pumps, us / 1000.0 / (kMs / 1000.0), datagrams / ((kMs + 3200) / 1000.0));
    CHECK(us / pumps < 2000);  // generous: the sanitizer build (Debug) runs this too
    for (Player* p : ps) CHECK(p->session->status().players == 8);
    CHECK(ps[7]->game.count(SpawnRole::Mirror) == 80);
}

// ---------------------------------------------------------------- hostile input

namespace {

std::vector<uint8_t> mutate(std::mt19937& rng, std::vector<uint8_t> v) {
    int edits = 1 + static_cast<int>(rng() % 4);
    for (int i = 0; i < edits; ++i) {
        size_t at = v.empty() ? 0 : rng() % v.size();
        switch (rng() % 7) {
            case 0:
                if (!v.empty()) v[at] ^= static_cast<uint8_t>(1u << (rng() % 8));
                break;
            case 1:
                if (!v.empty()) v[at] = static_cast<uint8_t>(rng());
                break;
            case 2:
                v.resize(v.empty() ? 0 : rng() % v.size());
                break;
            case 3:
                for (int k = static_cast<int>(rng() % 16); k > 0; --k) v.push_back(static_cast<uint8_t>(rng()));
                break;
            case 4:
                v.insert(v.begin() + static_cast<std::ptrdiff_t>(at), static_cast<uint8_t>(rng()));
                break;
            case 5: {
                static const uint8_t edge[] = {0, 1, 0x7F, 0x80, 0xFE, 0xFF};
                if (!v.empty()) v[at] = edge[rng() % sizeof(edge)];
                break;
            }
            default:
                if (v.size() > 2) v.erase(v.begin() + static_cast<std::ptrdiff_t>(at));
                break;
        }
    }
    return v;
}

std::vector<uint8_t> randomBytes(std::mt19937& rng, size_t maxSize) {
    std::vector<uint8_t> v(rng() % (maxSize + 1));
    for (auto& b : v) b = static_cast<uint8_t>(rng());
    return v;
}

// Every decoder: valid input round-trips; random and mutated input never reads out of bounds
// (the sanitizer build checks), and whatever it accepts can be encoded again.
template <typename T>
int fuzzDecoder(std::mt19937& rng, const std::vector<T>& samples, int rounds) {
    int accepted = 0;
    for (const T& s : samples) {
        T back;
        CHECK(decode(encode(s), back));
    }
    for (int i = 0; i < rounds; ++i) {
        std::vector<uint8_t> bytes =
            i % 4 == 0 ? randomBytes(rng, 64) : mutate(rng, encode(samples[rng() % samples.size()]));
        T v;
        if (decode(bytes, v)) {
            ++accepted;
            std::vector<uint8_t> again = encode(v);
            CHECK(again.size() <= 4 + bytes.size() * 2 + 64);
        }
    }
    return accepted;
}

// Keeps every datagram one endpoint sends.
struct Capture {
    std::vector<std::vector<uint8_t>> packets;
    void attach(MemoryNetwork& net, Endpoint from, size_t keep = 400) {
        net.tap = [this, from, keep](const Endpoint& f, const Endpoint&, const uint8_t* d, size_t n) {
            if (f == from && packets.size() < keep) packets.emplace_back(d, d + n);
        };
    }
};

std::vector<uint8_t> hello(uint8_t version, uint32_t cookie, uint32_t nonce, const std::string& name = "x") {
    ByteWriter w;
    w.u16(kMagic);
    w.u8(version);
    w.u8(static_cast<uint8_t>(PacketType::Hello));
    HelloInfo h;
    h.role = Role::Guest;
    h.nonce = nonce;
    h.name = name;
    h.cookie = cookie;
    write(w, h);
    return w.data;
}

std::vector<uint8_t> dataPacket(std::mt19937& rng, size_t maxTail) {
    ByteWriter w;
    writePacketHeader(w, PacketType::Data);
    std::vector<uint8_t> tail = randomBytes(rng, maxTail);
    w.bytes(tail.data(), tail.size());
    return w.data;
}

}  // namespace

COOP_TEST(testNetDecoderFuzz, "net: decoders survive random and mutated input") {
    std::mt19937 rng(99);
    const int kRounds = 4000;
    PlayerState ps;
    ps.timeMs = 9;
    ps.pos = {1, 2, 3};
    ps.flags = PF_OnMount | PF_InVehicle;
    ps.mountRef = 3;
    ps.vehicleRef = 4;
    fuzzDecoder(rng, std::vector<PlayerState>{ps, PlayerState{}}, kRounds);

    EntitySnapshot snap;
    snap.anchor = {10, 10, 10};
    EntityState e;
    e.id = 7;
    e.fields = EF_Static | EF_Pos | EF_Heading | EF_Tilt | EF_Vel | EF_Health | EF_State;
    e.pos = {12, 9, 10};
    snap.entities = {e, e};
    snap.entities[1].id = 8;
    snap.entities[1].pos = {5000, 0, 0};  // far: full floats
    snap.removed = {3, 4};
    int snaps = fuzzDecoder(rng, std::vector<EntitySnapshot>{snap, EntitySnapshot{}}, kRounds);
    CHECK(snaps > 0);  // the fuzzer does reach the accepting paths

    HelloInfo h;
    h.name = "Arthur";
    h.campaign = "c";
    h.cookie = 5;
    fuzzDecoder(rng, std::vector<HelloInfo>{h}, kRounds);
    fuzzDecoder(rng, std::vector<Damage>{{5, 10}}, kRounds);
    fuzzDecoder(rng, std::vector<Appearance>{{77}}, kRounds);
    fuzzDecoder(rng, std::vector<StoryInfo>{{StoryEvent::Died, 3}}, kRounds);
    CameraState cam;
    cam.pos = {1, 2, 3};
    fuzzDecoder(rng, std::vector<CameraState>{cam}, kRounds);
    fuzzDecoder(rng, std::vector<WaypointInfo>{{1, 2.0f, 3.0f}}, kRounds);
    fuzzDecoder(rng, std::vector<SaveRequest>{{}}, kRounds);
    SaveChunk chunk;
    chunk.total = 3000;
    chunk.offset = 960;
    chunk.data.assign(960, 0xAB);
    CHECK(fuzzDecoder(rng, std::vector<SaveChunk>{chunk}, kRounds) > 0);
    fuzzDecoder(rng, std::vector<Welcome>{{3, StoryMode::Shared, 1, "camp"}}, kRounds);
    Roster roster;
    roster.worldOwner = 1;
    roster.players = {{1, "A", "a"}, {2, "B", "b"}};
    fuzzDecoder(rng, std::vector<Roster>{roster}, kRounds);
    StoryCommandMsg cmd;
    cmd.text = "hold on";
    fuzzDecoder(rng, std::vector<StoryCommandMsg>{cmd}, kRounds);
    fuzzDecoder(rng, std::vector<Denied>{{"full"}}, kRounds);
    fuzzDecoder(rng, std::vector<WorldInfo>{{12, 30, 0, 1, 99}}, kRounds);

    // A roster must not list a player twice; a camera needs a usable field of view.
    roster.players[1].id = 1;
    Roster rosterBack;
    CHECK(!decode(encode(roster), rosterBack));
    cam.fov = 0;
    CameraState camBack;
    CHECK(!decode(encode(cam), camBack));

    for (int i = 0; i < kRounds; ++i) {
        std::vector<uint8_t> b = randomBytes(rng, 8);
        ByteReader r(b.data(), b.size());
        PacketType type;
        bool versionOk = false;
        readPacketHeader(r, type, versionOk);
    }
}

COOP_TEST(testNetCrc, "net: crc32 (slicing-by-8) matches the bytewise definition") {
    std::mt19937 rng(5);
    auto reference = [](const uint8_t* d, size_t n) {
        uint32_t c = ~0u;
        for (size_t i = 0; i < n; ++i) {
            c ^= d[i];
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        return ~c;
    };
    for (int i = 0; i < 200; ++i) {
        std::vector<uint8_t> v = randomBytes(rng, 300);
        size_t skip = v.empty() ? 0 : rng() % v.size();  // unaligned starts too
        CHECK(crc32(v.data() + skip, v.size() - skip) == reference(v.data() + skip, v.size() - skip));
        // Chaining works like one pass.
        size_t cut = v.size() > skip ? skip + rng() % (v.size() - skip) : skip;
        CHECK(crc32(v.data() + cut, v.size() - cut, crc32(v.data() + skip, cut - skip)) ==
              reference(v.data() + skip, v.size() - skip));
    }
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(crc32(check, sizeof(check)) == 0xCBF43926u);

    std::vector<uint8_t> big(8u << 20);
    for (auto& b : big) b = static_cast<uint8_t>(rng());
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    uint32_t fast = crc32(big.data(), big.size());
    auto t1 = Clock::now();
    uint32_t slow = reference(big.data(), big.size());
    auto t2 = Clock::now();
    CHECK(fast == slow);
    std::printf("  crc32 of an 8 MB save: %.1f ms (bit by bit: %.1f ms)\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count(),
                std::chrono::duration<double, std::milli>(t2 - t1).count());
}

COOP_TEST(testNetLinkHostileDatagrams, "net: link survives hostile datagrams") {
    MemoryNetwork net;
    auto* a = net.open(kHostAddr.ip, kHostAddr.port);
    auto* b = net.open(kGuestAddr.ip, kGuestAddr.port);
    Link host, guest;
    host.start(a, hostConfig(), 0);
    guest.start(b, guestConfig(), 0);
    Capture cap;
    cap.attach(net, kGuestAddr);
    std::mt19937 rng(17);
    uint32_t now = 0, sent = 0, next = 0;
    bool ordered = true;
    auto frame = [&] {
        net.now = now;
        host.update(now);
        guest.update(now);
        if (guest.connected() && guest.pendingReliable() < 32) {
            ByteWriter w;
            w.u32(sent++);
            guest.send(Msg::Waypoint, w.data, true);
            guest.send(Msg::PlayerState, PlayerState{}, false);
        }
        Msg type;
        std::vector<uint8_t> payload;
        while (host.poll(type, payload)) {
            if (type != Msg::Waypoint) continue;
            ByteReader r(payload.data(), payload.size());
            uint32_t v = r.u32();
            ordered = ordered && r.ok() && v == next;
            next = v + 1;
        }
        host.flush(now);
        guest.flush(now);
        now += 16;
    };
    for (int i = 0; i < 100; ++i) frame();
    CHECK(host.connected() && guest.connected() && cap.packets.size() > 50);
    uint32_t epoch = host.epoch();

    // Off-path garbage, from strangers and forged as the peer: foreign traffic, wrong session keys,
    // junk after a valid header. None of it may disturb the stream.
    for (int i = 0; i < 3000; ++i) {
        std::vector<uint8_t> d;
        switch (i % 3) {
            case 0: d = randomBytes(rng, 1400); break;
            case 1: {
                d = cap.packets[rng() % cap.packets.size()];
                if (d.size() < 8 || d[3] != static_cast<uint8_t>(PacketType::Data)) continue;
                d = mutate(rng, d);
                if (d.size() >= 6) d[4] ^= 0x5A;  // whatever else changed, the session key is wrong
                break;
            }
            default: {
                ByteWriter w;
                writePacketHeader(w, static_cast<PacketType>(2 + rng() % 5));  // anything but Hello
                std::vector<uint8_t> tail = randomBytes(rng, 200);
                w.bytes(tail.data(), tail.size());
                d = w.data;
            }
        }
        Endpoint from = i % 2 ? Endpoint{0x0B000001u + static_cast<uint32_t>(i), 1234} : kGuestAddr;
        host.receive(from, d.data(), d.size(), now);
        if (i % 10 == 0) frame();
    }
    for (int i = 0; i < 200; ++i) frame();
    CHECK(host.connected() && guest.connected() && host.epoch() == epoch);
    CHECK(ordered && next > 300 && next + 40 >= sent);

    // On-path: mutated copies of real packets (right key) may corrupt the session, nothing worse.
    for (int i = 0; i < 5000; ++i) {
        std::vector<uint8_t> d = mutate(rng, cap.packets[rng() % cap.packets.size()]);
        host.receive(kGuestAddr, d.data(), d.size(), now);
        if (i % 20 == 0) frame();
    }
    // Starting over works.
    guest.stop();
    host.stop();
    guest.start(b, guestConfig(), now);
    host.start(a, hostConfig(), now);
    next = sent = 0;
    ordered = true;
    for (int i = 0; i < 300; ++i) frame();
    CHECK(host.connected() && guest.connected() && ordered && next > 100);
}

COOP_TEST(testNetLinkLimits, "net: link refuses oversized records and never drops a reliable one") {
    MemoryNetwork net;
    net.latencyMs = 20;
    auto* a = net.open(kHostAddr.ip, kHostAddr.port);
    auto* b = net.open(kGuestAddr.ip, kGuestAddr.port);
    Link host, guest;
    host.start(a, hostConfig(), 0);
    guest.start(b, guestConfig(), 0);
    uint32_t now = 0;
    auto step = [&] {
        net.now = now;
        host.update(now);
        guest.update(now);
        host.flush(now);
        guest.flush(now);
        now += 16;
    };
    for (int i = 0; i < 50; ++i) step();
    CHECK(guest.connected());

    // A record too big for one datagram would have wedged the reliable channel for good.
    guest.send(Msg::Waypoint, std::vector<uint8_t>(Link::kMaxPayload + 1, 7), true);
    CHECK(guest.pendingReliable() == 0);
    guest.send(Msg::Waypoint, std::vector<uint8_t>(Link::kMaxPayload, 7), true);
    CHECK(guest.pendingReliable() == 1);

    // The receiver stops reading for a while: its inbox fills up, the rest waits unacknowledged at
    // the sender, and nothing is lost once it reads again.
    const uint32_t kCount = 3000;
    uint32_t queued = 0;
    auto feed = [&] {
        while (queued < kCount && guest.pendingReliable() < 100) {
            ByteWriter w;
            w.u32(queued++);
            std::vector<uint8_t> pad(1000, 1);
            w.bytes(pad.data(), pad.size());
            guest.send(Msg::Waypoint, w.data, true);
        }
    };
    for (int i = 0; i < 400; ++i) {
        feed();
        step();
    }
    CHECK(queued < kCount);  // the sender was held back instead of piling up
    uint32_t expect = 0;
    bool ordered = true, big = false;
    for (int i = 0; i < 3000 && expect < kCount; ++i) {
        feed();
        step();
        Msg type;
        std::vector<uint8_t> payload;
        while (host.poll(type, payload)) {
            if (payload.size() == Link::kMaxPayload) {
                big = true;
                continue;
            }
            ByteReader r(payload.data(), payload.size());
            ordered = ordered && r.u32() == expect;
            ++expect;
        }
    }
    CHECK(big && ordered && expect == kCount);
}

COOP_TEST(testNetForgedSenders, "net: forged senders get no state and no amplification") {
    Cluster c;
    Player& a = c.join("A", true);
    c.run(1000);
    CHECK(a.session->status().welcomed);
    size_t clientsBefore = c.server->clientCount();

    uint64_t toAttackers = 0, fromAttackers = 0, replies = 0;
    size_t biggestReply = 0;
    c.net.tap = [&](const Endpoint& from, const Endpoint& to, const uint8_t*, size_t n) {
        if (from == kServerAddr && (to.ip >> 24) == 0x0C) {
            toAttackers += n;
            ++replies;
            biggestReply = std::max(biggestReply, n);
        }
    };
    std::mt19937 rng(3);
    for (int i = 0; i < 2000; ++i) {
        Endpoint forged{0x0C000000u | static_cast<uint32_t>(rng() & 0xFFFFFF), static_cast<uint16_t>(1024 + rng() % 60000)};
        std::vector<uint8_t> d;
        switch (i % 4) {
            case 0: d = hello(kProtocolVersion, 0, rng()); break;              // first contact
            case 1: d = hello(kProtocolVersion, rng() | 1, rng()); break;      // a guessed cookie
            case 2: d = hello(kProtocolVersion - 1, 0, rng(), "old"); break;   // another version
            default: d = dataPacket(rng, 1000); break;                         // data, no session
        }
        fromAttackers += d.size();
        c.net.inject(forged, kServerAddr, d.data(), d.size());
        if (i % 50 == 0) c.run(16);
    }
    c.run(500);
    c.net.tap = nullptr;
    std::printf("  forged senders: %llu bytes in, %llu bytes out in %llu replies (largest %zu bytes)\n",
                static_cast<unsigned long long>(fromAttackers), static_cast<unsigned long long>(toAttackers),
                static_cast<unsigned long long>(replies), biggestReply);
    CHECK(c.server->clientCount() == clientsBefore);  // no state for any of them
    CHECK(toAttackers * 2 < fromAttackers);           // far less out than in
    CHECK(biggestReply <= 18);                         // a Challenge (8) or a bare Hello (18)
    CHECK(c.server->stats().challenges >= 1000);
    CHECK(a.session->status().welcomed && a.link.connected());

    // The answer to another version is our own Hello, so an older mod shows "version mismatch".
    std::vector<uint8_t> reply;
    Endpoint old{0x0C000001u, 5000};
    c.net.tap = [&](const Endpoint& from, const Endpoint& to, const uint8_t* d, size_t n) {
        if (from == kServerAddr && to == old) reply.assign(d, d + n);
    };
    std::vector<uint8_t> older = hello(kProtocolVersion - 1, 0, 1, "older");
    c.net.inject(old, kServerAddr, older.data(), older.size());
    c.run(50);
    c.net.tap = nullptr;
    CHECK(reply.size() >= 4 && reply.size() <= older.size() && reply[2] == kProtocolVersion &&
          reply[3] == static_cast<uint8_t>(PacketType::Hello));

    // A player whose mod is newer or older than the server sees why.
    MemoryNetwork net;
    auto* sock = net.open(kGuestAddr.ip, kGuestAddr.port);
    net.open(kHostAddr.ip, kHostAddr.port);
    Link guest;
    guest.start(sock, guestConfig(), 0);
    ByteWriter w;
    w.u16(kMagic);
    w.u8(static_cast<uint8_t>(kProtocolVersion + 1));
    w.u8(static_cast<uint8_t>(PacketType::Challenge));
    w.u32(1234);
    net.inject(kHostAddr, kGuestAddr, w.data.data(), w.data.size());
    guest.update(0);
    CHECK(guest.state() == Link::State::Failed && guest.error().find("version") != std::string::npos);
}

COOP_TEST(testNetServerFuzz, "net: server survives random and mutated datagrams") {
    Cluster c;
    c.net.latencyMs = 10;
    Player& a = c.join("A", true);
    c.run(300);
    Player& b = c.join("B");
    Player& d = c.join("C");
    EntityHandle npc = a.game.addPed(joaat("walker"), {3, 3, 0});
    c.perFrame = [&] { a.game.ents[npc].pos.x += 0.02f; };
    for (Player* p : {&a, &b, &d}) p->game.player.pos = {1, 1, 0};
    const Endpoint ownerAddr = a.sock->self();
    Capture cap;
    cap.attach(c.net, ownerAddr, 600);  // what the world owner sends
    c.run(3000);
    c.net.tap = nullptr;
    CHECK(b.session->status().players == 3 && mirrorOf(b.game, joaat("walker")));
    CHECK(cap.packets.size() > 30);
    uint32_t epochs[3] = {a.link.epoch(), b.link.epoch(), d.link.epoch()};

    // Off-path: anything from other addresses, including attackers that complete the cookie
    // exchange from their own address and then send garbage.
    std::mt19937 rng(11);
    std::vector<MemoryNetwork::Socket*> attackers;
    for (uint32_t i = 0; i < 4; ++i) attackers.push_back(c.net.open(0x0D000001u + i, 6000));
    std::vector<uint32_t> cookies(attackers.size(), 0);
    for (int round = 0; round < 300; ++round) {
        for (size_t k = 0; k < attackers.size(); ++k) {
            MemoryNetwork::Socket* s = attackers[k];
            uint8_t buf[2048];
            Endpoint from;
            int n;
            while ((n = s->recvFrom(from, buf, sizeof(buf))) >= 0)
                if (n == 8 && buf[3] == static_cast<uint8_t>(PacketType::Challenge)) std::memcpy(&cookies[k], buf + 4, 4);
            for (int j = 0; j < 8; ++j) {
                std::vector<uint8_t> dgram;
                switch (rng() % 5) {
                    case 0: dgram = randomBytes(rng, 1300); break;
                    case 1: dgram = hello(kProtocolVersion, cookies[k], rng(), std::string(rng() % 40, 'z')); break;
                    case 2: dgram = mutate(rng, cap.packets[rng() % cap.packets.size()]); break;
                    case 3: dgram = mutate(rng, hello(kProtocolVersion, cookies[k], rng())); break;
                    default: dgram = dataPacket(rng, 600); break;
                }
                s->sendTo(kServerAddr, dgram.data(), dgram.size());
                Endpoint forged{static_cast<uint32_t>(rng()), static_cast<uint16_t>(rng() | 1)};
                if (forged != ownerAddr && forged != b.sock->self() && forged != d.sock->self())
                    c.net.inject(forged, kServerAddr, dgram.data(), dgram.size());
            }
        }
        c.run(16);
    }
    c.run(1000);
    ServerStats st = c.server->stats();
    std::printf("  server fuzz: %llu datagrams, %llu dropped, %llu challenges, %zu connections\n",
                static_cast<unsigned long long>(st.datagrams), static_cast<unsigned long long>(st.dropped),
                static_cast<unsigned long long>(st.challenges), c.server->clientCount());
    CHECK(c.server->clientCount() <= 3 + attackers.size() * 16);
    // The players never noticed.
    CHECK(a.link.epoch() == epochs[0] && b.link.epoch() == epochs[1] && d.link.epoch() == epochs[2]);
    CHECK(b.session->status().players >= 3 && d.session->status().ownerName == "A");

    // On-path: mutated copies of the owner's real packets, forged as the owner. They may break the
    // owner's session; afterwards everything must come back by itself.
    for (int round = 0; round < 200; ++round) {
        for (int j = 0; j < 10; ++j) {
            std::vector<uint8_t> dgram = mutate(rng, cap.packets[rng() % cap.packets.size()]);
            c.net.inject(ownerAddr, kServerAddr, dgram.data(), dgram.size());
        }
        c.run(16);
    }
    c.run(15000);
    CHECK(a.link.connected() && b.link.connected() && d.link.connected());
    CHECK(a.session->status().players == 3 && b.session->status().players == 3 && d.session->status().players == 3);
    CHECK(mirrorOf(d.game, joaat("walker")) != 0);
}

COOP_TEST(testNetServerLimits, "net: per-address, per-player and upload limits") {
    Cluster c;
    c.serverConfig.password = "pw";
    c.serverConfig.maxPlayers = 32;
    Player& a = c.join("A", true, "lim", "pw");
    c.run(500);
    Player& b = c.join("B", false, "lim", "pw");
    c.run(1000);
    CHECK(b.session->status().welcomed);

    // Many connections from one address: only 16 get a slot.
    std::vector<Link> links(20);
    for (size_t i = 0; i < links.size(); ++i) {
        LinkConfig lc;
        lc.remote = kServerAddr;
        lc.local.role = Role::Guest;
        lc.local.name = "crowd";
        lc.local.campaign = "crowd" + std::to_string(i % 2);
        lc.local.password = "pw";
        links[i].start(c.net.open(0x0E000001u, static_cast<uint16_t>(7000 + i)), lc, c.now);
    }
    c.perFrame = [&] {
        for (Link& l : links) {
            l.update(c.now);
            Msg t;
            std::vector<uint8_t> p;
            while (l.poll(t, p)) {}
            l.flush(c.now);
        }
    };
    c.run(2000);
    int connected = 0;
    for (Link& l : links) connected += l.connected();
    CHECK(connected == 16);
    for (Link& l : links) l.stop();
    c.perFrame = nullptr;
    c.run(3000);

    // A player spamming waypoints: the others get a trickle, the server does not do the work.
    std::vector<uint8_t> wp = encode(WaypointInfo{1, 5.0f, 5.0f});
    c.perFrame = [&] {
        for (int i = 0; i < 20; ++i) b.link.send(Msg::Waypoint, wp, true);
    };
    uint64_t limitedBefore = c.server->stats().rateLimited;
    c.run(2000);
    c.perFrame = nullptr;
    c.run(500);
    uint64_t limited = c.server->stats().rateLimited - limitedBefore;
    std::printf("  spam: %llu of ~2500 waypoints dropped by the per-player limit\n",
                static_cast<unsigned long long>(limited));
    CHECK(limited > 2000);
    CHECK(a.link.connected() && a.session->status().players == 2);

    // Uploads: announcing a huge save costs the server only what really arrives.
    SaveChunk chunk;
    chunk.total = 60u << 20;
    chunk.crc = 1;
    for (uint32_t i = 0; i < 10; ++i) {
        chunk.offset = i * static_cast<uint32_t>(kSaveChunkBytes);
        chunk.data.assign(kSaveChunkBytes, static_cast<uint8_t>(i));
        a.link.send(Msg::SaveChunk, chunk, true);
    }
    c.run(500);
    CHECK(c.server->stats().uploadBytes == 10 * kSaveChunkBytes);
    dropPlayer(c, a);
    c.run(500);
    CHECK(c.server->stats().uploadBytes == 0);  // gone with its sender

    // Guessing the password from one address stops being checked after a few tries.
    Player& g = c.join("G", false, "lim", "guess");
    c.run(400);
    CHECK(g.session->status().denied == "wrong password");
    for (int i = 0; i < 11; ++i) {
        g.connect(c.net, 0, "G", "player_G", false, c.config, c.now, "lim", "guess");
        c.run(400);
    }
    g.connect(c.net, 0, "G", "player_G", false, c.config, c.now, "lim", "pw");
    c.run(1000);
    CHECK(!g.session->status().welcomed && g.session->status().denied.find("too many") != std::string::npos);
    // Other addresses are not affected, and this one may try again a minute later.
    Player& other = c.join("Other", false, "lim", "pw");
    c.run(1000);
    CHECK(other.session->status().welcomed);
    c.run(61000);
    g.connect(c.net, 0, "G", "player_G", false, c.config, c.now, "lim", "pw");
    c.run(1000);
    CHECK(g.session->status().welcomed);
}

COOP_TEST(testNetDeniedThenAdmitted, "net: a refused address can come back with the right password") {
    Cluster c;
    c.serverConfig.password = "pw";
    Player& a = c.join("A", true, "default", "wrong");
    c.run(600);
    CHECK(a.session->status().denied == "wrong password");
    // The same game (same address) tries again at once, now with the right password.
    a.connect(c.net, 0, "A", "player_A", true, c.config, c.now, "default", "pw");
    c.run(4000);
    CHECK(a.session->status().welcomed && a.link.connected() && c.server->playerCount() == 1);
}

COOP_TEST(testNetNamesAndCampaigns, "net: player names, avatars and campaign folders are cleaned") {
    Cluster c;
    Player& a = c.join("A", true);
    c.run(300);
    // Control characters, '=', spaces around, and a 24th byte inside a two-byte character.
    std::string raw = std::string("  \x01Jo=hn\x7f ") + std::string(14, 'x') + "\xD0\x96\xD0\x96";
    c.join(raw);
    c.run(1500);
    EntityHandle puppet = a.game.find(SpawnRole::PlayerPuppet);
    CHECK(puppet != 0);
    if (puppet) {
        const std::string& label = a.game.ents[puppet].label;
        CHECK(label.size() <= 24 && label.compare(0, 4, "John") == 0);
        bool printable = true;
        for (unsigned char ch : label) printable = printable && ch >= 32 && ch != 127 && ch != '=';
        CHECK(printable);
        // Never cut inside a UTF-8 character.
        CHECK(!label.empty() && (static_cast<unsigned char>(label.back()) & 0xE0) != 0xC0);
    }
    CHECK(sanitizeCampaign("CON") == "con_" && sanitizeCampaign("nul") == "nul_" && sanitizeCampaign("com1") == "com1_" &&
          sanitizeCampaign("lpt9") == "lpt9_" && sanitizeCampaign("com10") == "com10" &&
          sanitizeCampaign("console") == "console");
}

COOP_TEST(testNetShutdown, "net: a stopping server tells the players at once") {
    Cluster c;
    Player& a = c.join("A", true);
    Player& b = c.join("B");
    c.run(1500);
    CHECK(a.link.connected() && b.link.connected());
    c.server->shutdown(c.now);
    c.server.reset();  // the process ends
    c.run(300);
    CHECK(!a.link.connected() && !b.link.connected());  // not only after a 10 s timeout
    CHECK(journalHas(c, "default", "server stopped"));
}

COOP_TEST(testNetFileStorageSafety, "net: save files are written whole and found by number") {
    std::string root = "coop-test-net-storage";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    {
        FileStorage fs(root, 50);
        std::filesystem::path saves = std::filesystem::path(root) / "camp" / "saves";
        std::vector<uint8_t> data;
        CHECK(fs.storeSave("camp", {1}, "A"));
        // A leftover of a crash mid-write, and a numbering that has grown past six digits.
        std::ofstream(saves / "save-000002-x.sav.part") << "broken";
        std::ofstream(saves / "save-999999-x.sav", std::ios::binary).put(9);
        std::ofstream(saves / "save-1000000-x.sav", std::ios::binary).put(7);
        CHECK(fs.loadSave("camp", data) && data.size() == 1 && data[0] == 7);
        CHECK(fs.storeSave("camp", {8}, "A"));
        CHECK(fs.loadSave("camp", data) && data.size() == 1 && data[0] == 8);
    }
    MemoryStorage mem;
    std::vector<uint8_t> data;
    for (uint8_t i = 0; i < 30; ++i) mem.storeSave("m", {i}, "A");
    CHECK(mem.saves["m"].size() <= 5 && mem.loadSave("m", data) && data[0] == 29);
    std::filesystem::remove_all(root, ec);
}

COOP_TEST(testNetHelloCannotDesync, "net: a repeated or forged Hello cannot desynchronise a session") {
    // The listening side restarts its session on a Hello with another nonce. Before, it kept its
    // own nonce: after a forged Hello and then a repeat of the real one, both sides had the same
    // session key again but the listener had renumbered its reliable records from 0 while the
    // player still expected the old numbers - connected, and deaf, for good.
    MemoryNetwork net;
    auto* a = net.open(kHostAddr.ip, kHostAddr.port);
    auto* b = net.open(kGuestAddr.ip, kGuestAddr.port);
    Link host, guest;
    host.start(a, hostConfig(), 0);
    guest.start(b, guestConfig(), 0);
    Capture cap;
    cap.attach(net, kGuestAddr);
    uint32_t now = 0;
    int toGuest = 0, toHost = 0;
    auto run = [&](int frames) {
        for (int i = 0; i < frames; ++i, now += 16) {
            net.now = now;
            host.update(now);
            guest.update(now);
            Msg type;
            std::vector<uint8_t> payload;
            while (host.poll(type, payload)) toHost += type == Msg::Waypoint;
            while (guest.poll(type, payload)) toGuest += type == Msg::Waypoint;
            host.flush(now);
            guest.flush(now);
        }
    };
    run(30);
    CHECK(host.connected() && guest.connected());
    std::vector<uint8_t> realHello;
    for (const auto& p : cap.packets)
        if (p.size() > 4 && p[3] == static_cast<uint8_t>(PacketType::Hello)) realHello = p;
    net.tap = nullptr;
    CHECK(!realHello.empty());
    // Some reliable traffic both ways, so the numbering has moved on.
    for (int i = 0; i < 40; ++i) {
        host.send(Msg::Waypoint, std::vector<uint8_t>{1}, true);
        guest.send(Msg::Waypoint, std::vector<uint8_t>{2}, true);
        run(1);
    }
    run(30);
    // A forged Hello (another nonce) and then the player's real one again, both from its address.
    std::vector<uint8_t> forged = hello(kProtocolVersion, 0, 0xBADF00D, "John");
    host.receive(kGuestAddr, forged.data(), forged.size(), now);
    run(5);
    host.receive(kGuestAddr, realHello.data(), realHello.size(), now);
    run(5);
    // Whatever happened, within a timeout the two must talk again, both ways.
    toGuest = toHost = 0;
    for (int i = 0; i < 1000 && (toGuest < 5 || toHost < 5); ++i) {
        if (host.connected()) host.send(Msg::Waypoint, std::vector<uint8_t>{1}, true);
        if (guest.connected()) guest.send(Msg::Waypoint, std::vector<uint8_t>{2}, true);
        run(1);
    }
    CHECK(toGuest >= 5 && toHost >= 5);
}
