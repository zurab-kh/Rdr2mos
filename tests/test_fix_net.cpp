// Network and server fixes: reconnects from a new port, resets for Data without a session, the
// per-player budgets, campaign save storage, stale world damage and refused uploads.
//
// COOP_NET_SEED=<n> runs these tests with that seed and a little loss and jitter on the network.
#include <filesystem>
#include <fstream>
#include "harness.h"

namespace {
namespace fs = std::filesystem;

// The network conditions for one test: perfect by default, or seeded loss and jitter.
void netConditions(Cluster& c) {
    const char* seed = std::getenv("COOP_NET_SEED");
    if (!seed || !*seed) return;
    c.net.rng.seed(static_cast<uint32_t>(std::strtoul(seed, nullptr, 10)));
    c.net.loss = std::max(c.net.loss, 0.02);
    c.net.jitterMs += 4;
}

// Joins and waits until the server has let the player in: under loss a Hello can be lost, and a
// fixed wait would let whoever joins next arrive first (and take the world, ids and names).
Player& joinWelcomed(Cluster& c, const std::string& name, bool wantsWorld) {
    Player& p = c.join(name, wantsWorld);
    for (int i = 0; i < 400 && !(p.session->status().welcomed && p.session->status().players >= 1); ++i) c.run(16);
    c.run(100);
    return p;
}

// A player without a game: a bare link the test drives itself, keeping everything it receives.
struct RawClient {
    MemoryNetwork::Socket* sock = nullptr;
    Link link;
    std::vector<std::pair<Msg, std::vector<uint8_t>>> got;

    void start(Cluster& c, uint32_t ip, uint16_t port, const std::string& name, const std::string& campaign,
               bool wantsWorld) {
        sock = c.net.open(ip, port);
        LinkConfig lc;
        lc.remote = kServerAddr;
        lc.local.role = Role::Guest;
        lc.local.name = name;
        lc.local.campaign = campaign;
        lc.local.wantsWorld = wantsWorld ? 1 : 0;
        link.start(sock, lc, c.now);
    }
    void tick(uint32_t now) {
        link.update(now);
        Msg t;
        std::vector<uint8_t> p;
        while (link.poll(t, p)) got.push_back({t, p});
        link.flush(now);
    }
    bool welcomed() const {
        return std::any_of(got.begin(), got.end(), [](const auto& m) { return m.first == Msg::Welcome; });
    }
    std::vector<SaveResult> results() const {
        std::vector<SaveResult> out;
        for (const auto& [t, p] : got) {
            SaveResult r;
            if (t == Msg::SaveResult && decode(p, r)) out.push_back(r);
        }
        return out;
    }
    void upload(const std::vector<uint8_t>& file, uint32_t crc, size_t pieces = SIZE_MAX) {
        SaveChunk chunk;
        chunk.total = static_cast<uint32_t>(file.size());
        chunk.crc = crc;
        for (size_t off = 0, n = 0; off < file.size() && n < pieces; off += kSaveChunkBytes, ++n) {
            chunk.offset = static_cast<uint32_t>(off);
            size_t len = std::min(kSaveChunkBytes, file.size() - off);
            chunk.data.assign(file.begin() + static_cast<std::ptrdiff_t>(off),
                              file.begin() + static_cast<std::ptrdiff_t>(off + len));
            link.send(Msg::SaveChunk, chunk, true);
        }
    }
};

fs::path freshDir(const char* name) {
    fs::path root = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    return root;
}

size_t saveFiles(const fs::path& saves) {
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(saves, ec)) n += e.path().extension() == ".sav";
    return n;
}

const uint32_t kMission = joaat("mission_review");
void play(Player& p, uint16_t flags, uint32_t script = kMission) {
    p.game.player.flags = flags;
    p.game.missionScript = (flags & PF_InMission) ? script : 0;
}
}  // namespace

// ---------------------------------------------------------------- 0, 18: a new port, the same player

COOP_TEST(testFixNetReconnectFromNewPort, "fix-net: a player reconnecting from a new port replaces its old connection") {
    Cluster c;
    netConditions(c);
    Player& a = c.join("A", true);
    Player& b = c.join("B");
    c.run(2000);
    CHECK(c.server->playerCount() == 2 && a.session->status().worldOwner);
    // A loads a save: the script restarts, the old socket is closed, a new one connects.
    c.net.unplug(a.sock);
    a.sock = c.net.open(0x0A000101u, 40001);
    a.connect(c.net, 0x0A000101u, "A", "player_A", true, c.config, c.now);
    c.run(2000);
    std::vector<std::string> lines = c.server->describe();
    for (const auto& l : lines) std::printf("  %s\n", l.c_str());
    std::printf("  players on server %zu, A owns the world again: %d, A's players %d\n", c.server->playerCount(),
                a.session->status().worldOwner, a.session->status().players);
    CHECK(c.server->playerCount() == 2);                                     // no ghost of A's old connection
    CHECK(lines.size() == 1 && lines[0].find("A 2") == std::string::npos);  // still called "A"
    CHECK(a.session->status().worldOwner);                                   // the world is not stuck with the ghost
    CHECK(c.server->stats().takeovers == 1);
    (void)b;
}

COOP_TEST(testFixNetSaveLoadReconnect, "fix-net: the world owner loading a save (script restart) keeps the world and its name") {
    Cluster c;
    netConditions(c);
    c.net.latencyMs = 30;
    const uint32_t kFolk = joaat("a_m_m_townfolk");
    Player& a = c.join("A", true);
    c.run(300);
    Player& b = c.join("B");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {4, 0, 0};
    for (int i = 0; i < 5; ++i) a.game.addPed(kFolk, {float(6 + i), 6, 0});
    c.run(4000);
    CHECK(mirrorOf(b.game, kFolk) != 0);
    // A loads a save: new App, new socket, new link and session; no goodbye from the old one.
    c.net.unplug(a.sock);
    a.sock = c.net.open(0x0A000101u, 40001);
    a.link.start(a.sock, a.linkConfig, c.now);
    a.session = std::make_unique<CoopSession>(a.game, a.link, c.config);
    a.session->setSaveStore(&a.saves);
    c.run(4000);
    std::printf("  4 s after A reconnected: B sees %d players, owner \"%s\", A owns the world: %d, A's town shown at B: %d\n",
                b.session->status().players, b.session->status().ownerName.c_str(), a.session->status().worldOwner,
                mirrorOf(b.game, kFolk) != 0);
    CHECK(a.session->status().welcomed);
    CHECK(a.session->status().worldOwner);
    CHECK(b.session->status().ownerName == "A");
    CHECK(b.session->status().players == 2);
    CHECK(mirrorOf(b.game, kFolk) != 0);
    CHECK(journalHas(c, "default", "A reconnected") && !journalHas(c, "default", "A 2"));
}

COOP_TEST(testFixNetGuestNewPortFullCampaign, "fix-net: a guest back on a new port keeps its slot, id and puppet in a full campaign") {
    Cluster c;
    netConditions(c);
    c.serverConfig.maxPlayers = 2;
    Player& a = c.join("A", true);
    c.run(300);
    Player& b = c.join("B");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {3, 0, 0};
    c.run(2500);
    uint8_t idBefore = b.session->status().playerId;
    CHECK(idBefore != 0 && a.game.count(SpawnRole::PlayerPuppet) == 1);
    c.net.unplug(b.sock);
    b.sock = c.net.open(0x0A000102u, 40001);
    b.connect(c.net, 0x0A000102u, "B", "player_B", false, c.config, c.now);
    c.run(3000);
    std::printf("  B: welcomed %d, denied \"%s\", id %d (was %d); A shows %d puppet(s)\n", b.session->status().welcomed,
                b.session->status().denied.c_str(), b.session->status().playerId, idBefore,
                a.game.count(SpawnRole::PlayerPuppet));
    CHECK(b.session->status().welcomed && b.session->status().denied.empty());  // not "campaign is full"
    CHECK(b.session->status().playerId == idBefore);
    CHECK(c.server->playerCount() == 2 && a.session->status().players == 2);
    CHECK(a.game.count(SpawnRole::PlayerPuppet) == 1);  // no frozen copy of B next to the real one
    CHECK(a.session->status().worldOwner);
    CHECK(!journalHas(c, "default", "B left"));
}

COOP_TEST(testFixNetSameAddressNewSession, "fix-net: a new session from the same address keeps the player's place and world") {
    Cluster c;
    netConditions(c);
    Player& a = joinWelcomed(c, "A", true);
    Player& b = c.join("B", true);  // wants the world too: it would keep it, had A left
    c.run(2000);
    CHECK(a.session->status().worldOwner);
    uint8_t idBefore = a.session->status().playerId;
    // The mod starts over on the same socket (or the connection blips): a new session, same address.
    a.link.start(a.sock, a.linkConfig, c.now);
    a.session = std::make_unique<CoopSession>(a.game, a.link, c.config);
    a.session->setSaveStore(&a.saves);
    c.run(2000);
    std::vector<std::string> lines = c.server->describe();
    for (const auto& l : lines) std::printf("  %s\n", l.c_str());
    CHECK(lines.size() == 1 && lines[0] == "campaign \"default\": A (world), B");
    CHECK(a.session->status().worldOwner && a.session->status().playerId == idBefore);
    CHECK(b.session->status().players == 2 && b.session->status().ownerName == "A");
    CHECK(!journalHas(c, "default", "B now owns the world") && !journalHas(c, "default", "A left"));
}

COOP_TEST(testFixNetSameNameSameAddress, "fix-net: two players with one name behind one address both stay in") {
    Cluster c;
    netConditions(c);
    Player& a = joinWelcomed(c, "A", true);
    // Two machines behind one NAT, both called "Pat": the second must not take the first one's place.
    auto pat = [&](uint16_t port) -> Player& {
        c.extra.push_back(std::make_unique<Player>());
        Player& p = *c.extra.back();
        p.sock = c.net.open(0x0A000200u, port);
        p.connect(c.net, 0x0A000200u, "Pat", "player_Pat", false, c.config, c.now);
        c.players.push_back(&p);
        return p;
    };
    Player& p1 = pat(50000);
    c.run(1500);
    Player& p2 = pat(50001);
    uint32_t epoch1 = p1.link.epoch();
    c.run(4000);
    std::vector<std::string> lines = c.server->describe();
    for (const auto& l : lines) std::printf("  %s\n", l.c_str());
    CHECK(c.server->playerCount() == 3 && c.server->stats().takeovers == 0);
    CHECK(lines.size() == 1 && lines[0].find("Pat, Pat 2") != std::string::npos);
    CHECK(p1.session->status().welcomed && p2.session->status().welcomed);
    CHECK(p1.link.epoch() == epoch1 && p1.link.connected());  // the first one was never thrown out
    CHECK(a.session->status().players == 3);
}

// ---------------------------------------------------------------- 1: Data without a session

COOP_TEST(testFixNetLoadingScreenRejoin, "fix-net: a player back from a long loading screen rejoins at once") {
    Cluster c;
    netConditions(c);
    Player& a = c.join("A", true);
    Player& b = c.join("B");
    c.run(2000);
    CHECK(c.server->playerCount() == 2);
    // B's game: loading screen of 12 s (no tick at all).
    c.players.erase(std::remove(c.players.begin(), c.players.end(), &b), c.players.end());
    c.run(12000);
    CHECK(c.server->playerCount() == 1);
    c.players.push_back(&b);
    uint32_t back = c.now, rejoinedAfter = 0;
    while (c.now - back < 15000) {
        c.run(16);
        if (c.server->playerCount() == 2 && b.link.connected()) {
            rejoinedAfter = c.now - back;
            break;
        }
    }
    std::printf("  rejoined %u ms after the loading screen\n", rejoinedAfter);
    CHECK(rejoinedAfter > 0 && rejoinedAfter < 3000);
    (void)a;
}

COOP_TEST(testFixNetServerCrashRestart, "fix-net: players reconnect promptly to a restarted server") {
    Cluster c;
    netConditions(c);
    Player& a = c.join("A", true);
    Player& b = c.join("B");
    c.run(2000);
    CHECK(c.server->playerCount() == 2);
    c.server.reset();  // crash: no shutdown(), no Bye
    c.run(500);        // restart takes half a second
    c.server = std::make_unique<Server>(*c.serverSock, c.serverConfig, c.storage);
    uint32_t back = c.now, rejoinedAfter = 0;
    while (c.now - back < 15000) {
        c.run(16);
        if (c.server->playerCount() == 2) {
            rejoinedAfter = c.now - back;
            break;
        }
    }
    std::printf("  rejoined %u ms after the restart\n", rejoinedAfter);
    CHECK(rejoinedAfter > 0 && rejoinedAfter < 3000);
    c.run(1000);
    CHECK(a.session->status().worldOwner && b.session->status().ownerName == "A");
}

COOP_TEST(testFixNetResetsAreSmallAndBounded, "fix-net: resets for stray Data are smaller than the Data and rate limited") {
    Cluster c;
    netConditions(c);
    Player& a = c.join("A", true);
    c.run(500);
    uint64_t resetsBefore = c.server->stats().resets;
    size_t byes = 0, biggest = 0;
    c.net.tap = [&](const Endpoint& from, const Endpoint&, const uint8_t* data, size_t size) {
        if (from == kServerAddr && size >= 4 && data[3] == static_cast<uint8_t>(PacketType::Bye)) {
            ++byes;
            biggest = std::max(biggest, size);
        }
    };
    // A flood of the smallest possible Data from forged addresses: 300 per frame for 3 s.
    std::mt19937 rng(7);
    ByteWriter w;
    writePacketHeader(w, PacketType::Data);
    w.u16(0x1234);
    w.u16(0);
    const size_t dataSize = w.data.size();
    c.perFrame = [&] {
        for (int i = 0; i < 300; ++i) {
            Endpoint forged{0x0B000000u | static_cast<uint32_t>(rng() & 0xFFFFFF), static_cast<uint16_t>(1024 + rng() % 60000)};
            c.net.inject(forged, kServerAddr, w.data.data(), w.data.size());
        }
    };
    c.run(3000);
    c.perFrame = nullptr;
    c.net.tap = nullptr;
    uint64_t resets = c.server->stats().resets - resetsBefore;
    std::printf("  %llu resets for ~56000 stray datagrams, largest %zu bytes (Data %zu bytes)\n",
                static_cast<unsigned long long>(resets), biggest, dataSize);
    CHECK(byes == resets && resets > 0 && resets <= 200 + 100 * 3 + 10);
    CHECK(biggest < dataSize);
    CHECK(a.link.connected() && a.session->status().welcomed);
}

COOP_TEST(testFixNetNoResetForDenied, "fix-net: a refused player is not reset into asking again and again") {
    Cluster c;
    netConditions(c);
    c.serverConfig.password = "pw";
    Player& a = c.join("A", true, "default", "wrong");
    c.run(600);
    CHECK(a.session->status().denied == "wrong password");
    uint64_t challenges = c.server->stats().challenges, resets = c.server->stats().resets;
    c.run(6000);  // the server forgets it after 1.5 s; its link still sends keep-alives
    std::printf("  6 s after the refusal: %llu resets, %llu new challenges\n",
                static_cast<unsigned long long>(c.server->stats().resets - resets),
                static_cast<unsigned long long>(c.server->stats().challenges - challenges));
    CHECK(c.server->stats().resets == resets);
    CHECK(c.server->stats().challenges == challenges);
}

// ---------------------------------------------------------------- 2, 16: budgets

COOP_TEST(testFixNetDamageBurstsKeepReliable, "fix-net: damage bursts do not cost the owner its reliable messages") {
    Cluster c;
    netConditions(c);
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

COOP_TEST(testFixNetBurningEnemiesKeepDeath, "fix-net: enemies set on fire do not swallow a player's death in the mission") {
    Cluster c;
    netConditions(c);
    c.net.latencyMs = 30;
    const uint32_t kOutlaw = joaat("g_m_m_uniduster_01");
    Player& a = joinWelcomed(c, "A", true);  // the campaign's first world: epoch 1
    Player& b = c.join("B");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {3, 0, 0};
    for (int i = 0; i < 4; ++i) a.game.addPed(kOutlaw, {float(8 + i), 6, 0}, 400);
    c.run(3000);
    play(a, PF_InMission);
    play(b, PF_InMission);
    c.run(2000);
    std::vector<EntityHandle> burning;
    for (auto& [h, en] : b.game.ents)
        if (en.ours && en.model == kOutlaw) burning.push_back(h);
    CHECK(burning.size() == 4);
    uint64_t limitedBefore = c.server->stats().rateLimited;
    c.perFrame = [&] {
        for (EntityHandle h : burning) {
            auto it = b.game.ents.find(h);
            if (it == b.game.ents.end() || it->second.health <= 1) continue;
            it->second.health -= 1;  // fire: a little every frame, caused by B
            it->second.hitByPlayer = true;
        }
    };
    c.run(2000);
    b.game.playerHealth = 0;  // B burns to death
    c.run(1500);
    c.perFrame = nullptr;
    std::printf("  messages dropped by the server's per-player limits: %llu\n",
                static_cast<unsigned long long>(c.server->stats().rateLimited - limitedBefore));
    CHECK(c.server->stats().rateLimited == limitedBefore);
    CHECK(journalHas(c, "default", "died in a mission"));
    CHECK(a.game.playerHealth == 0);  // shared failure: A restarts too
}

COOP_TEST(testFixNetStoryNeverDropped, "fix-net: a burst of story events is slowed down, not dropped; the latest waypoint wins") {
    Cluster c;
    netConditions(c);
    Player& a = c.join("A", true);
    c.run(300);
    Player& b = c.join("B");
    c.run(1500);
    uint64_t limitedBefore = c.server->stats().rateLimited;
    // 120 mission changes from the owner in one frame: twice the burst of its story budget.
    for (uint32_t i = 1; i <= 120; ++i) a.link.send(Msg::Story, StoryInfo{StoryEvent::Mission, 1000 + i}, true);
    // And 300 waypoints from B, each a little further east.
    for (int i = 1; i <= 300; ++i) b.link.send(Msg::Waypoint, WaypointInfo{1, float(i), 7.0f}, true);
    c.run(6000);
    int missions = 0;
    for (const auto& line : c.storage.journals["default"]) missions += line.find("is playing mission") != std::string::npos;
    std::printf("  %d of 120 mission changes logged; A shows B's waypoint at x=%.0f; %llu superseded\n", missions,
                a.game.friendWaypointPos.x, static_cast<unsigned long long>(c.server->stats().rateLimited - limitedBefore));
    CHECK(missions == 120);
    CHECK(a.game.friendWaypoint && a.game.friendWaypointPos.x == 300.0f);
    CHECK(a.link.connected() && b.link.connected() && b.session->status().players == 2);
}

// ---------------------------------------------------------------- 3, 11: campaign saves on disk

COOP_TEST(testFixNetEmptyLatestSave, "fix-net: an empty newest save file does not hide the campaign save") {
    fs::path root = freshDir("coop-fix-net-empty-newest");
    std::error_code ec;
    {
        FileStorage storage(root.string(), 20);
        std::vector<uint8_t> good(5000, 0x5A);
        CHECK(storage.storeSave("camp", good, "A"));
        // What a power cut right after the rename of the next save leaves behind.
        fs::path saves = root / "camp" / "saves";
        std::ofstream(saves / "save-000002-20261116-120000.sav", std::ios::binary).flush();
        std::vector<uint8_t> data;
        bool found = storage.loadSave("camp", data);
        std::printf("  campaign save found: %zu bytes\n", data.size());
        CHECK(found && data == good);
        CHECK(storage.hasSaveFiles("camp") && !storage.hasSaveFiles("other"));
        // Nothing half-written is left next to the saves.
        size_t parts = 0;
        for (const auto& e : fs::directory_iterator(saves, ec)) parts += e.path().extension() == ".part";
        CHECK(parts == 0);
    }
    fs::remove_all(root, ec);
}

COOP_TEST(testFixNetUnreadableNewestSave, "fix-net: an unreadable newest file does not make the campaign look new") {
    fs::path root = freshDir("coop-fix-net-unreadable-newest");
    std::error_code ec;
    std::vector<uint8_t> campaign(4000, 7);
    {
        FileStorage seed(root.string());
        CHECK(seed.storeSave("default", campaign, "A"));
        // A newer file that cannot be used (left empty by a full disk or an older server version).
        std::ofstream(root / "default" / "saves" / "save-000002-20260101-000000.sav", std::ios::binary);
    }
    FileStorage storage(root.string());
    Cluster c;
    netConditions(c);
    c.server = std::make_unique<Server>(*c.serverSock, c.serverConfig, storage);
    Player& a = c.join("A", true);
    a.saves.latest = std::vector<uint8_t>(4000, 3);  // an old local save of the owner, not the campaign
    a.saves.stamp = 1;
    c.run(200);
    Player& b = c.join("B");
    c.run(8000);
    CHECK(b.session->requestCampaignSave());
    c.run(5000);
    std::vector<uint8_t> served;
    CHECK(storage.loadSave("default", served));
    std::printf("  server now serves %s, B got %zu file(s)\n",
                served == campaign ? "the campaign save" : served == a.saves.latest ? "A's old local save" : "something else",
                b.saves.written.size());
    CHECK(b.saves.written.size() == 1 && b.saves.written[0] == campaign);
    fs::remove_all(root, ec);
}

COOP_TEST(testFixNetAllSavesUnreadable, "fix-net: a campaign whose saves are all unreadable is never started over") {
    fs::path root = freshDir("coop-fix-net-all-unreadable");
    std::error_code ec;
    fs::create_directories(root / "default" / "saves", ec);
    std::ofstream(root / "default" / "saves" / "save-000001-20260101-000000.sav", std::ios::binary);
    FileStorage storage(root.string());
    Cluster c;
    netConditions(c);
    c.server = std::make_unique<Server>(*c.serverSock, c.serverConfig, storage);
    Player& a = c.join("A", true);
    a.saves.latest = std::vector<uint8_t>(4000, 3);  // the owner's own, unrelated save
    a.saves.stamp = 1;
    c.run(200);
    Player& b = c.join("B");
    c.run(8000);
    std::string text;
    while (b.session->popNotice(text)) {}
    CHECK(b.session->requestCampaignSave());
    c.run(2000);
    bool told = sawNotice(*b.session, "cannot read");
    std::printf("  A sees a campaign save: %d, files on disk %zu, B told why there is nothing: %d\n",
                a.session->status().campaignHasSave, saveFiles(root / "default" / "saves"), told);
    CHECK(a.session->status().campaignHasSave && b.session->status().campaignHasSave);
    CHECK(saveFiles(root / "default" / "saves") == 1);  // A's save did not become the campaign save
    CHECK(told && b.saves.written.empty());
    fs::remove_all(root, ec);
}

COOP_TEST(testFixNetPlayersIniWhole, "fix-net: players.ini is replaced whole") {
    fs::path root = freshDir("coop-fix-net-players");
    std::error_code ec;
    {
        FileStorage storage(root.string());
        storage.storePlayer("camp", "A", {1, 2, 3});
        storage.storePlayer("camp", "B", {4, 5, 6});
        storage.storePlayer("camp", "A", {7, 8, 9});
        std::ifstream in(root / "camp" / "players.ini");
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(all.find("A=") != std::string::npos && all.find("B=") != std::string::npos);
        CHECK(all.find("7.0 8.0 9.0") != std::string::npos && all.find("1.0 2.0 3.0") == std::string::npos);
        CHECK(!fs::exists(root / "camp" / "players.ini.part"));
    }
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------- 17: hits on an old world

COOP_TEST(testFixNetStaleWorldDamage, "fix-net: a hit aimed at the previous world is not applied in the new one") {
    Cluster c;
    netConditions(c);
    const uint32_t kOutlaw = joaat("g_m_m_uniduster_01");
    Player& a = joinWelcomed(c, "A", true);  // the campaign's first world: epoch 1
    Player& b = c.join("B");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {3, 0, 0};
    EntityHandle outlaw = a.game.addPed(kOutlaw, {6, 0, 0}, 400);
    c.run(2500);
    CHECK(mirrorOf(b.game, kOutlaw) != 0);
    // A's world is the campaign's first: epoch 1, and its only NPC has world id 1.
    b.link.send(Msg::Damage, Damage{1, 30, 1}, true);
    c.run(500);
    int afterCurrent = a.game.ents[outlaw].health;
    b.link.send(Msg::Damage, Damage{1, 30, 7}, true);  // sent in a world that is gone
    c.run(500);
    int afterStale = a.game.ents[outlaw].health;
    b.link.send(Msg::Damage, Damage{1, 30, 0}, true);  // a sender without a roster names no world
    c.run(500);
    std::printf("  outlaw health: %d after a current hit, %d after a stale one, %d after one without epoch\n",
                afterCurrent, afterStale, a.game.ents[outlaw].health);
    CHECK(afterCurrent == 370 && afterStale == 370 && a.game.ents[outlaw].health == 370);
    CHECK(c.server->stats().staleDamage == 2);
}

// ---------------------------------------------------------------- 12: refused uploads

COOP_TEST(testFixNetRefusedUploads, "fix-net: every refused upload is answered with a SaveResult at once") {
    Cluster c;
    netConditions(c);
    c.storage.saves["camp"].push_back(std::vector<uint8_t>(3000, 1));  // the campaign has a save
    RawClient owner, guest, taker;
    c.perFrame = [&] {
        for (RawClient* r : {&owner, &guest, &taker})
            if (r->sock) r->tick(c.now);
    };
    c.startServer();
    owner.start(c, 0x0A000301u, 41000, "Owner", "camp", false);  // first in: owns the world
    c.run(1000);
    guest.start(c, 0x0A000302u, 41000, "Guest", "camp", false);
    c.run(1000);
    CHECK(owner.welcomed() && guest.welcomed());

    // A player who does not own the world, while the campaign has a save: one answer, at the first piece.
    std::vector<uint8_t> file(5 * kSaveChunkBytes, 9);
    uint32_t crc = crc32(file.data(), file.size());
    guest.upload(file, crc);
    c.run(1000);
    std::vector<SaveResult> r = guest.results();
    CHECK(r.size() == 1 && r[0].stored == 0 && r[0].crc == crc && r[0].reason.find("owner") != std::string::npos);

    // The owner's upload that arrives damaged.
    owner.upload(file, crc ^ 1);
    c.run(1000);
    r = owner.results();
    CHECK(r.size() == 1 && r[0].stored == 0 && r[0].reason.find("damaged") != std::string::npos);

    // The world changes hands in the middle of the owner's upload: refused then, not at the next piece.
    owner.upload(file, crc, 2);
    c.run(1000);
    taker.start(c, 0x0A000303u, 41000, "Taker", "camp", true);  // asks for the world and gets it
    c.run(1500);
    r = owner.results();
    CHECK(r.size() == 2 && r[1].stored == 0 && r[1].reason.find("another player") != std::string::npos);
    owner.upload(file, crc);  // a whole new upload from the former owner: refused once more
    c.run(1000);
    CHECK(owner.results().size() == 3 && owner.results()[2].stored == 0);

    // The new owner's upload is stored and confirmed.
    taker.upload(file, crc);
    c.run(1500);
    r = taker.results();
    CHECK(r.size() == 1 && r[0].stored == 1 && r[0].crc == crc && r[0].reason.empty());
    CHECK(c.storage.saves["camp"].size() == 2 && c.storage.saves["camp"].back() == file);
    std::printf("  refusals: guest \"%s\", owner \"%s\" / \"%s\"\n", guest.results()[0].reason.c_str(),
                owner.results()[0].reason.c_str(), owner.results()[1].reason.c_str());
}
