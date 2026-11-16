// Story progression and campaign saves on the client (story.cpp): a regroup carried out after our
// own scene, a save made just before the world owner quits, and uploads the server did not take.
// The reviewers' repro tests come first in each section, then regression tests around them. The
// tests with network trouble run with several seeds of the simulated network.
#include "harness.h"

namespace {

const uint32_t kFixMission = joaat("mission_x");

void fixPlay(Player& p, uint16_t flags, uint32_t script = kFixMission) {
    p.game.player.flags = flags;
    p.game.missionScript = (flags & PF_InMission) ? script : 0;
}

std::vector<uint8_t> fixBytes(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> out(n);
    for (auto& b : out) b = static_cast<uint8_t>(rng());
    return out;
}

// A cluster that keeps every notice each player gets (the session only queues a few).
struct FixRig : Cluster {
    std::map<const Player*, std::vector<std::string>> notices;
    std::function<void()> each;

    explicit FixRig(uint32_t seed = 0) {
        if (seed) {
            net.rng.seed(seed);
            net.latencyMs = 20 + seed % 40;
            net.jitterMs = seed % 15;
            net.loss = 0.01;
        }
        perFrame = [this] {
            collect();
            if (each) each();
        };
    }
    void collect() {
        for (Player* p : players) {
            std::string text;
            while (p->session && p->session->popNotice(text)) notices[p].push_back(text);
        }
    }
    bool saw(const Player& p, const char* needle) {
        collect();
        for (const std::string& t : notices[&p])
            if (t.find(needle) != std::string::npos) return true;
        return false;
    }
    void forget() {
        collect();
        notices.clear();
    }
};

const uint32_t kFixSeeds[] = {11, 23, 37, 41, 59};

}  // namespace

// ---------------------------------------------------------------- regroup after our own scene

COOP_TEST(testReviewRegroupStaleTarget, "review story: a regroup carried out after our scene uses where the owner is now") {
    Cluster c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {5, 0, 0};
    c.run(1500);
    // B walks into the mission marker a moment before the owner and watches the intro.
    fixPlay(b, PF_InMission | PF_Cutscene);
    c.run(600);
    fixPlay(a, PF_InMission | PF_Cutscene);
    c.run(2000);
    // The intro ends somewhere else, for both of them (the game moves the players).
    a.game.player.pos = {400, 0, 0};
    b.game.player.pos = {404, 0, 0};
    c.run(200);
    fixPlay(a, PF_InMission);
    fixPlay(b, PF_InMission);
    c.run(3000);
    std::printf("  B at %.0f, A at %.0f\n", b.game.player.pos.x, a.game.player.pos.x);
    // B must not be sent back to where the mission started, 400 m from both players.
    CHECK(distance(b.game.player.pos, a.game.player.pos) < 20.0f);
}

COOP_TEST(testFixRegroupLateFollowsOwner, "fix story: a regroup after our scene still brings us to the owner, where the owner is now") {
    for (uint32_t seed : kFixSeeds) {
        FixRig c(seed);
        Player& a = c.join("A", true);
        c.run(200);
        Player& b = c.join("B");
        a.game.player.pos = {0, 0, 0};
        b.game.player.pos = {5, 0, 0};
        c.run(1500);
        fixPlay(b, PF_InMission | PF_Cutscene);
        c.run(600);
        fixPlay(a, PF_InMission);
        c.run(1000);
        // The owner walks on; B's intro drops B far away from both the start and the owner.
        a.game.player.pos = {200, 0, 0};
        b.game.player.pos = {-300, 0, 0};
        c.run(1000);
        CHECK(distance(b.game.player.pos, Vec3{-300, 0, 0}) < 0.5f);  // nothing during the scene
        fixPlay(b, PF_InMission);
        c.run(1000);
        CHECK(distance(b.game.player.pos, a.game.player.pos) < 5.0f);
    }
}

COOP_TEST(testFixRegroupDroppedOnHandover, "fix story: a regroup waiting for our scene is dropped when the world changes hands") {
    FixRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {5, 0, 0};
    c.run(1500);
    fixPlay(b, PF_InMission | PF_Cutscene);
    c.run(600);
    fixPlay(a, PF_InMission);
    c.run(1000);
    b.game.player.pos = {400, 0, 0};  // B's scene moves B
    dropPlayer(c, a);                 // and the owner leaves meanwhile: the world goes to B
    c.run(8000);
    CHECK(b.session->status().worldOwner);
    fixPlay(b, PF_InMission);
    c.run(1500);
    CHECK(distance(b.game.player.pos, Vec3{400, 0, 0}) < 0.5f);  // not back to the old owner's start
}

COOP_TEST(testFixRegroupStartHereDone, "fix story: a 'start it here too' waiting for our scene is dropped once that scene put us in a mission") {
    FixRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {60, 0, 0};
    c.run(1500);
    fixPlay(b, PF_Cutscene);  // a scene of B's own, outside any mission
    c.run(600);
    fixPlay(a, PF_InMission);  // "A started a mission - start it here too" (15 m)
    c.run(1000);
    CHECK(distance(b.game.player.pos, Vec3{60, 0, 0}) < 0.5f);
    // B's scene was the start of the same mission: B is in it now, 60 m from the owner.
    fixPlay(b, PF_InMission);
    c.run(1500);
    CHECK(distance(b.game.player.pos, Vec3{60, 0, 0}) < 0.5f);

    // Without a scene in the way the same request is carried out right away.
    FixRig d;
    Player& a2 = d.join("A", true);
    d.run(200);
    Player& b2 = d.join("B");
    a2.game.player.pos = {0, 0, 0};
    b2.game.player.pos = {60, 0, 0};
    d.run(1500);
    fixPlay(a2, PF_InMission);
    d.run(1000);
    CHECK(distance(b2.game.player.pos, a2.game.player.pos) < 3.0f);
    CHECK(d.saw(b2, "start it here too"));
}

// ---------------------------------------------------------------- a save made just before quitting

COOP_TEST(testReviewSaveAndQuit, "review saves: a save made just before the owner quits reaches the server next time") {
    Cluster c;
    Player& a = c.join("A", true);
    a.saves.latest = std::vector<uint8_t>(3000, 1);
    a.saves.stamp = 1;
    c.run(3000);
    CHECK(c.storage.saves["default"].size() == 1);
    // "Save and quit": the owner saves and closes the game a second later.
    c.run(4000);  // just after a save check
    a.saves.latest = std::vector<uint8_t>(3000, 2);
    a.saves.stamp = 2;
    c.run(1000);
    dropPlayer(c, a);
    c.run(25000);
    CHECK(c.server->playerCount() == 0);
    // Next evening: the game starts again with that save as the newest one.
    a.connect(c.net, 0x0A000101u, "A", "player_A", true, c.config, c.now);
    c.players.push_back(&a);
    c.run(20000);
    CHECK(a.session->status().worldOwner);
    std::printf("  campaign saves on the server: %zu\n", c.storage.saves["default"].size());
    CHECK(c.storage.saves["default"].back() == a.saves.latest);
}

COOP_TEST(testFixSaveQuitMidUpload, "fix saves: a save whose upload the owner's quitting cut off goes up on the next launch, once") {
    for (uint32_t seed : kFixSeeds) {
        FixRig c(seed);
        Player& a = c.join("A", true);
        a.saves.latest = fixBytes(5000, seed);
        a.saves.stamp = 1;
        c.run(4000);
        CHECK(c.storage.saves["default"].size() == 1);
        a.saves.latest = fixBytes(600000, seed + 1);
        a.saves.stamp = 2;
        bool quit = false;
        c.each = [&] {
            if (!quit && a.session->status().saveProgress >= 30) {
                quit = true;
                dropPlayer(c, a);  // the game closes in the middle of the upload
            }
        };
        c.run(12000);
        c.each = nullptr;
        CHECK(quit);
        c.run(15000);
        CHECK(c.storage.saves["default"].size() == 1);
        a.connect(c.net, 0x0A000101u, "A", "player_A", true, c.config, c.now);
        c.players.push_back(&a);
        c.run(25000);
        CHECK(a.session->status().worldOwner);
        CHECK(c.saw(a, "server does not have it yet"));
        CHECK(c.storage.saves["default"].size() == 2 && c.storage.saves["default"].back() == a.saves.latest);
        // No second copy: not on later checks, not after a reconnect.
        a.saves.stamp = 3;  // the game touched the file
        c.run(12000);
        a.link.start(a.sock, a.linkConfig, c.now);
        c.run(12000);
        CHECK(c.storage.saves["default"].size() == 2);
    }
}

COOP_TEST(testFixSaveNoCatchUpOfServerCopies, "fix saves: the first check of a launch sends nothing the server already has") {
    FixRig c;
    Player& a = c.join("A", true);
    a.saves.latest = fixBytes(5000, 70);
    a.saves.stamp = 1;
    c.run(4000);
    CHECK(c.storage.saves["default"].size() == 1);
    // The owner quits with the campaign save as its newest file and comes back: nothing to send.
    dropPlayer(c, a);
    c.run(25000);
    c.forget();
    a.saves.stamp = 9;  // a new process, and the game touched the file
    a.connect(c.net, 0x0A000101u, "A", "player_A", true, c.config, c.now);
    c.players.push_back(&a);
    c.run(15000);
    CHECK(a.session->status().worldOwner);
    CHECK(c.storage.saves["default"].size() == 1);
    CHECK(!c.saw(a, "Uploading"));

    // A player who downloaded the campaign save and owns the world on another evening: the copy
    // (the campaign save) is not sent back.
    Player& b = c.join("B");
    c.run(3000);
    CHECK(b.session->requestCampaignSave());
    c.run(4000);
    CHECK(b.saves.written.size() == 1);
    b.saves.latest = b.saves.written[0];
    b.saves.stamp = 4;
    dropPlayer(c, a);
    dropPlayer(c, b);
    c.run(25000);
    b.connect(c.net, 0x0A000103u, "B", "player_B", true, c.config, c.now);
    c.players.push_back(&b);
    c.run(15000);
    CHECK(b.session->status().worldOwner);
    CHECK(c.storage.saves["default"].size() == 1);

    // A player who does not own the world sends nothing on its first check.
    Player& d = c.join("D");
    d.saves.latest = fixBytes(5000, 71);
    d.saves.stamp = 1;
    c.run(15000);
    CHECK(!d.session->status().worldOwner);
    CHECK(c.storage.saves["default"].size() == 1);
}

// ---------------------------------------------------------------- uploads the server did not take

COOP_TEST(testReviewNonOwnerStaleHasSave, "review saves: a refused upload is not reported as running or done") {
    Cluster c;
    Player& a = c.join("A", true);  // a new campaign: no save yet
    c.run(200);
    Player& b = c.join("B");
    c.run(1500);
    a.saves.latest = std::vector<uint8_t>(3000, 1);  // the owner's first save starts the campaign
    a.saves.stamp = 1;
    c.run(7000);
    CHECK(c.storage.saves["default"].size() == 1);
    b.saves.latest = std::vector<uint8_t>(3000, 9);  // B's own, unrelated save
    b.saves.stamp = 1;
    c.run(6000);
    std::string text;
    while (b.session->popNotice(text)) {}
    bool started = b.session->uploadSave();
    c.run(4000);
    bool told = false;
    while (b.session->popNotice(text)) {
        std::printf("  B: %s\n", text.c_str());
        told = told || text.find("owner") != std::string::npos || text.find("not") != std::string::npos;
    }
    CHECK(c.storage.saves["default"].size() == 1);
    // B either is refused up front, or hears that the server did not take it.
    CHECK(!started || told);
}

COOP_TEST(testFixSaveRaceToStartCampaign, "fix saves: an upload overtaken by the owner's first save is reported as not uploaded") {
    for (uint32_t seed : kFixSeeds) {
        FixRig c(seed);
        Player& a = c.join("A", true);  // a new campaign: anyone may start it
        c.run(200);
        Player& b = c.join("B");
        b.saves.latest = fixBytes(1500000, seed);  // much larger: still going up when A's is stored
        b.saves.stamp = 1;
        c.run(3000);
        c.forget();
        a.saves.latest = fixBytes(200000, seed + 1);  // takes a few round trips: B starts meanwhile
        a.saves.stamp = 1;
        bool started = false;
        c.each = [&] {
            if (!started && a.session->status().saveProgress >= 0) started = b.session->uploadSave();
        };
        c.run(40000);
        CHECK(started);
        CHECK(c.storage.saves["default"].size() == 1 && c.storage.saves["default"].back() == a.saves.latest);
        CHECK(c.saw(b, "not uploaded") || c.saw(b, "not stored") || c.saw(b, "did not take"));
        CHECK(b.session->status().saveProgress == -1);
        CHECK(b.session->status().campaignHasSave);
        // B is not the owner and the campaign has a save now: B is refused up front from now on.
        CHECK(!b.session->uploadSave());
    }
}

namespace {

// One StorySync against a scripted server: what it answers to an upload is up to the test.
struct ScriptedServer {
    enum class Answer { Silent, Refuse, Store, WrongCrc };
    MemoryNetwork net;
    MemoryNetwork::Socket* serverSock = net.open(kHostAddr.ip, kHostAddr.port);
    MemoryNetwork::Socket* playerSock = net.open(kGuestAddr.ip, kGuestAddr.port);
    Link server, player;
    FakeGame game;
    MemorySaveStore store;
    SessionConfig config;
    std::vector<std::string> notices;
    StorySync story{game, player, config, [this](const std::string& t) { notices.push_back(t); }};
    Answer answer = Answer::Silent;
    std::vector<std::vector<uint8_t>> uploads;  // complete uploads that arrived
    std::vector<uint8_t> receiving;
    uint32_t now = 1000;

    explicit ScriptedServer(uint32_t seed) {
        net.rng.seed(seed);
        net.latencyMs = 20 + seed % 40;
        net.jitterMs = seed % 15;
        net.loss = 0.01;
        server.start(serverSock, hostConfig(), now);
        player.start(playerSock, guestConfig(), now);
        story.setSaveStore(&store);
    }
    void run(uint32_t ms) {
        for (uint32_t end = now + ms; now < end; now += 16) {
            net.now = now;
            server.update(now);
            player.update(now);
            Msg type;
            std::vector<uint8_t> payload;
            while (server.poll(type, payload)) {
                SaveChunk chunk;
                if (type != Msg::SaveChunk || !decode(payload, chunk)) continue;
                if (chunk.offset == 0) receiving.clear();
                if (chunk.offset != receiving.size()) continue;
                receiving.insert(receiving.end(), chunk.data.begin(), chunk.data.end());
                if (receiving.size() < chunk.total) continue;
                uploads.push_back(receiving);
                SaveResult r;
                r.crc = chunk.crc;
                if (answer == Answer::Refuse) {
                    r.reason = "Only the world owner can update the campaign save";
                    server.send(Msg::SaveResult, r, true);
                } else if (answer == Answer::Store || answer == Answer::WrongCrc) {
                    r.stored = 1;
                    if (answer == Answer::WrongCrc) r.crc ^= 1;  // the answer to some other upload
                    server.send(Msg::SaveResult, r, true);
                }
            }
            while (player.poll(type, payload))
                story.handle(type, 0, payload.data(), payload.size(), now);
            if (player.connected()) {
                PlayerState local;
                story.tick(now, local, {}, 1, 1);  // we own the world
            }
            server.flush(now);
            player.flush(now);
        }
    }
    bool saw(const char* needle) const {
        for (const std::string& t : notices)
            if (t.find(needle) != std::string::npos) return true;
        return false;
    }
};

}  // namespace

COOP_TEST(testFixSaveSilentServer, "fix saves: an upload the server never answers is reported, not taken as stored") {
    for (uint32_t seed : kFixSeeds) {
        ScriptedServer s(seed);
        std::vector<uint8_t> campaign = fixBytes(4000, seed);
        s.story.setCampaignSave(true, crc32(campaign.data(), campaign.size()));
        s.store.latest = fixBytes(40000, seed + 1);  // newer than the campaign save: goes up
        s.store.stamp = 1;
        s.run(10000);
        CHECK(s.uploads.size() == 1);
        CHECK(!s.saw("did not confirm"));
        s.run(30000);
        CHECK(s.saw("did not confirm"));
        CHECK(s.story.saveProgress() == -1);
        // Not retried by itself: no loop of uploads the server drops.
        s.run(30000);
        CHECK(s.uploads.size() == 1);
        // It was never stored, so a touched file (the same content) is offered again once it changes.
        s.store.stamp = 2;
        s.run(8000);
        CHECK(s.uploads.size() == 2);
    }
}

COOP_TEST(testFixSaveRefused, "fix saves: a refused upload stops with the server's reason and is not offered again") {
    for (uint32_t seed : kFixSeeds) {
        ScriptedServer s(seed);
        s.answer = ScriptedServer::Answer::Refuse;
        std::vector<uint8_t> campaign = fixBytes(4000, seed);
        s.story.setCampaignSave(true, crc32(campaign.data(), campaign.size()));
        s.store.latest = fixBytes(40000, seed + 1);
        s.store.stamp = 1;
        s.run(10000);
        CHECK(s.uploads.size() == 1);
        CHECK(s.saw("Save not uploaded: Only the world owner"));
        CHECK(s.story.saveProgress() == -1);
        // The same file again (the game touched it, a reconnect): not offered by itself.
        s.store.stamp = 2;
        s.run(8000);
        s.story.reset();
        s.story.setCampaignSave(true, crc32(campaign.data(), campaign.size()));
        s.run(8000);
        CHECK(s.uploads.size() == 1);
        CHECK(!s.saw("did not confirm"));
        // A really new save is.
        s.store.latest = fixBytes(40000, seed + 2);
        s.store.stamp = 3;
        s.run(8000);
        CHECK(s.uploads.size() == 2);
    }
}

COOP_TEST(testFixSaveStoredOnlyOnMatchingResult, "fix saves: only the server's answer for this very file confirms an upload") {
    for (uint32_t seed : kFixSeeds) {
        // An answer for another file does not confirm ours: the upload is still unconfirmed and
        // times out.
        ScriptedServer w(seed);
        w.answer = ScriptedServer::Answer::WrongCrc;
        std::vector<uint8_t> campaign = fixBytes(4000, seed);
        w.story.setCampaignSave(true, crc32(campaign.data(), campaign.size()));
        w.store.latest = fixBytes(40000, seed + 1);
        w.store.stamp = 1;
        w.run(45000);
        CHECK(w.uploads.size() == 1);
        CHECK(w.saw("did not confirm"));

        // The right answer: stored, quietly; the same file is never sent again, not even after a
        // reconnect whose Welcome still names the old campaign save.
        ScriptedServer s(seed);
        s.answer = ScriptedServer::Answer::Store;
        s.story.setCampaignSave(true, crc32(campaign.data(), campaign.size()));
        s.store.latest = fixBytes(40000, seed + 1);
        s.store.stamp = 1;
        s.run(45000);
        CHECK(s.uploads.size() == 1);
        CHECK(!s.saw("did not confirm") && !s.saw("not uploaded"));
        s.store.stamp = 2;
        s.run(8000);
        s.story.reset();
        s.story.setCampaignSave(true, crc32(campaign.data(), campaign.size()));
        s.run(8000);
        CHECK(s.uploads.size() == 1);
    }
}
