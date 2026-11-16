// Story synchronisation in the situations that break state machines: players joining, leaving
// and reconnecting mid-mission or mid-scene, the world changing hands during the cutscene
// barrier, races between the server's commands and the local game, three and more players,
// and campaign save transfers that overlap, get interrupted or change hands.
#include "harness.h"

namespace {

const uint32_t kMissionX = joaat("mission_x"), kMissionY = joaat("mission_y");

// Puts a player into the given story state; the mission script is known while in a mission.
void play(Player& p, uint16_t flags, uint32_t script = kMissionX) {
    p.game.player.flags = flags;
    p.game.missionScript = (flags & PF_InMission) ? script : 0;
}

// A cluster that keeps every notice each player gets (the session only queues a few).
struct StoryRig : Cluster {
    std::map<const Player*, std::vector<std::string>> notices;
    std::function<void()> each;  // the test's own per-frame work

    StoryRig() {
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
    int count(const Player& p, const char* needle) {
        collect();
        int n = 0;
        for (const std::string& t : notices[&p]) n += t.find(needle) != std::string::npos;
        return n;
    }
    void forget() {
        collect();
        notices.clear();
    }
    // A fresh connection to the server (a network blip, or the mod reconnecting after a load).
    void reconnect(Player& p) { p.link.start(p.sock, p.linkConfig, now); }
};

int journalCount(Cluster& c, const char* needle) {
    int n = 0;
    for (const auto& line : c.storage.journals["default"]) n += line.find(needle) != std::string::npos;
    return n;
}

std::vector<uint8_t> randomBytes(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> out(n);
    for (auto& b : out) b = static_cast<uint8_t>(rng());
    return out;
}

}  // namespace

// ---------------------------------------------------------------- joining, leaving, reconnecting

COOP_TEST(testStoryOwnerRejoinMidMission, "story: the world owner rejoining mid-mission is not a new mission start") {
    StoryRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {400, 0, 0};
    c.run(1500);
    play(b, PF_InMission);
    c.run(600);
    play(a, PF_InMission);  // the owner starts the mission: B, far away in it already, is brought over
    c.run(1000);
    CHECK(distance(b.game.player.pos, a.game.player.pos) < 3.0f);
    CHECK(journalCount(c, "A started a mission") == 1);

    b.game.player.pos = {400, 0, 0};  // B rides off on a mission task
    c.run(500);
    c.forget();
    c.reconnect(a);  // the owner's connection blips mid-mission
    c.run(3000);
    CHECK(a.session->status().worldOwner);
    CHECK(distance(b.game.player.pos, Vec3{400, 0, 0}) < 0.5f);  // not pulled back again
    CHECK(!c.saw(b, "started a mission"));
    CHECK(journalCount(c, "A started a mission") == 1);
    // ... and no join wait was started for it: the owner's next scene does not wait for anyone.
    play(a, PF_InMission | PF_Cutscene);
    play(b, PF_InMission);
    c.run(1000);
    play(a, PF_InMission);
    c.run(600);
    CHECK(!a.game.held);
}

COOP_TEST(testStoryOwnerLeavesDuringBarrier, "story: the world owner leaving keeps the cutscene barrier") {
    StoryRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    c.run(1500);
    for (Player* p : {&a, &b, &d}) play(*p, PF_InMission | PF_Cutscene);
    c.run(1000);
    play(a, PF_InMission);
    play(b, PF_InMission);
    c.run(800);
    CHECK(a.game.held && b.game.held);
    CHECK(c.saw(b, "Waiting for C to finish the cutscene"));

    dropPlayer(c, a);  // the owner quits while B waits for C
    c.run(800);
    CHECK(b.session->status().worldOwner);
    CHECK(b.game.held);  // C still watches: B keeps waiting
    play(d, PF_InMission);
    c.run(2500);
    CHECK(!b.game.held && !d.game.held);
    CHECK(c.saw(b, "Continuing together"));
}

COOP_TEST(testStoryRejoinMidCutscene, "story: a player rejoining mid-cutscene is waited for, not mistaken for a scene end") {
    StoryRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    c.run(1500);
    play(a, PF_InMission | PF_Cutscene);
    play(b, PF_InMission | PF_Cutscene);
    c.run(1000);
    c.reconnect(b);  // B's connection blips during the scene
    c.run(1500);
    play(a, PF_InMission);
    c.run(800);
    CHECK(a.game.held);  // B is back and still watching
    CHECK(!b.game.held);
    play(b, PF_InMission);
    c.run(2500);
    CHECK(!a.game.held && !b.game.held);
}

// ---------------------------------------------------------------- the barrier

COOP_TEST(testStoryFrozenWorldIsNotAScene, "story: frozen players that look like they are in a scene do not deadlock") {
    // If the game reports "no control" while the world is frozen, a held player looks like a
    // cutscene watcher. Two held players must not wait for each other until the timeout.
    StoryRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    c.run(1500);
    std::map<Player*, uint16_t> base{{&a, PF_InMission | PF_Cutscene}, {&b, PF_InMission | PF_Cutscene}};
    a.game.missionScript = b.game.missionScript = kMissionX;
    c.each = [&] {
        for (auto& [p, f] : base) p->game.player.flags = f | (p->game.held ? PF_Cutscene : 0);
    };
    c.run(1000);
    base[&a] = PF_InMission;
    c.run(800);
    CHECK(a.game.held);
    base[&b] = PF_InMission;
    c.run(3000);
    CHECK(!a.game.held && !b.game.held);
    c.run(3000);
    CHECK(!a.game.held && !b.game.held);  // and no new hold after the release
}

COOP_TEST(testStoryHoldRaces, "story: a hold that arrives after the mission ended or during a new scene") {
    StoryRig c;
    c.net.latencyMs = 200;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    c.run(2500);
    bool everHeld = false;
    c.each = [&] { everHeld = everHeld || a.game.held; };

    // A's scene ends and right after that its mission ends too: the server's hold comes too late.
    play(a, PF_InMission | PF_Cutscene);
    play(b, PF_InMission | PF_Cutscene);
    c.run(1500);
    play(a, PF_InMission);
    c.run(100);
    play(a, 0);
    c.run(2000);
    CHECK(!everHeld);

    // A's scene ends and the next one starts at once: no freeze during A's own scene...
    play(a, PF_InMission | PF_Cutscene);
    c.run(1500);
    play(a, PF_InMission);
    c.run(100);
    play(a, PF_InMission | PF_Cutscene);
    c.run(2000);
    CHECK(!everHeld);
    // ... but after it, while B still watches.
    play(a, PF_InMission);
    c.run(800);
    CHECK(a.game.held);
    play(b, PF_InMission);
    c.run(3000);
    CHECK(!a.game.held && !b.game.held);
}

COOP_TEST(testStoryStalePlayerNotWaitedFor, "story: a player whose game stopped answering is not waited for") {
    StoryRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    c.run(1500);
    for (Player* p : {&a, &b, &d}) play(*p, PF_InMission | PF_Cutscene);
    c.run(1000);
    play(a, PF_InMission);
    d.game.isReady = false;  // C's game goes into a loading screen mid-scene (still connected)
    c.run(800);
    CHECK(a.game.held);
    play(b, PF_InMission);
    c.run(800);
    CHECK(b.game.held);
    c.run(6500);
    CHECK(!a.game.held && !b.game.held);
    CHECK(c.saw(a, "Continuing without C"));
}

COOP_TEST(testStoryJoinWaitEnds, "story: the wait for players who never start the mission ends after the join window") {
    StoryRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    c.run(1500);
    play(a, PF_InMission | PF_Cutscene);
    c.run(2000);
    play(a, PF_InMission);
    c.run(800);
    CHECK(a.game.held);
    CHECK(c.saw(a, "Waiting for B to start the mission"));
    c.run(30000);
    CHECK(a.game.held);
    c.run(16000);
    CHECK(!a.game.held);
    CHECK(c.saw(a, "Continuing without B"));
    (void)b;
}

COOP_TEST(testStoryHoldEndsWithMission, "story: a hold never outlives the mission or a death") {
    StoryRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    c.run(1500);
    play(a, PF_InMission | PF_Cutscene);
    play(b, PF_InMission | PF_Cutscene);
    c.run(1000);
    play(a, PF_InMission);
    c.run(800);
    CHECK(a.game.held);
    play(a, 0);  // the mission script ends the mission while the world is frozen
    c.run(300);
    CHECK(!a.game.held && !a.session->status().holding);
    c.run(3000);
    CHECK(!a.game.held);
}

COOP_TEST(testStoryRegroupWaitsForScene, "story: being brought to the mission start waits until our own scene is over") {
    StoryRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    a.game.player.pos = {0, 0, 0};
    b.game.player.pos = {500, 0, 0};
    c.run(1500);
    play(b, PF_InMission | PF_Cutscene);  // B started the mission first and watches its intro
    c.run(600);
    play(a, PF_InMission);
    c.run(1000);
    CHECK(distance(b.game.player.pos, Vec3{500, 0, 0}) < 0.5f);  // no teleport in the middle of a scene
    play(b, PF_InMission);
    c.run(500);
    CHECK(distance(b.game.player.pos, a.game.player.pos) < 3.0f);
}

// ---------------------------------------------------------------- mission checks

COOP_TEST(testStoryMismatchFalsePositives, "story: no mission mismatch for chained missions, unknown scripts or free roam") {
    StoryRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    c.run(1500);
    bool ever = false;
    c.each = [&] { ever = ever || a.session->status().missionMismatch || b.session->status().missionMismatch; };
    play(a, PF_InMission);
    play(b, PF_InMission);
    c.run(3000);
    // The next mission follows at once: the scripts change a moment apart on the two machines.
    play(a, PF_InMission, kMissionY);
    c.run(500);
    play(b, PF_InMission, kMissionY);
    c.run(3000);
    // The script is not known yet on one side.
    play(b, PF_InMission, 0);
    c.run(3000);
    // The owner is in free roam while B plays something else.
    play(a, 0);
    play(b, PF_InMission, kMissionX);
    c.run(3000);
    CHECK(!ever);
    CHECK(!c.saw(a, "different mission") && !c.saw(b, "different mission"));

    // A real difference is still reported, with names.
    play(a, PF_InMission, kMissionY);
    c.run(3000);
    CHECK(a.session->status().missionMismatch && b.session->status().missionMismatch);
    CHECK(c.saw(b, "You and A are in different missions"));
    CHECK(c.saw(a, "B is in a different mission"));
    play(b, PF_InMission, kMissionY);
    c.run(1000);
    CHECK(!a.session->status().missionMismatch && !b.session->status().missionMismatch);
}

// ---------------------------------------------------------------- shared failure

COOP_TEST(testStoryFailWithThreeMissions, "story: shared failure reaches the owner even when someone else is in another mission") {
    StoryRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    c.run(1500);
    play(a, PF_InMission, kMissionX);
    play(b, PF_InMission, kMissionX);
    play(d, PF_InMission, kMissionY);
    c.run(3000);
    CHECK(d.session->status().missionMismatch && a.session->status().missionMismatch);
    b.game.playerHealth = 0;  // B dies in the owner's mission
    c.run(800);
    CHECK(a.game.playerHealth == 0);    // the owner's mission restarts too
    CHECK(d.game.playerHealth == 200);  // C plays something else
    for (Player* p : {&a, &b, &d}) p->game.playerHealth = 200;
    c.run(1500);
    d.game.playerHealth = 0;  // C dies in its own mission: nobody else is affected
    c.run(800);
    CHECK(a.game.playerHealth == 200 && b.game.playerHealth == 200);
}

COOP_TEST(testStoryFailFourPlayers, "story: shared failure with four players: held, watching, simultaneous deaths, no echo") {
    StoryRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    Player& d = c.join("C");
    Player& e = c.join("D");
    c.run(1500);
    for (Player* p : {&a, &b, &d, &e}) play(*p, PF_InMission | PF_Cutscene);
    c.run(1000);
    play(a, PF_InMission);
    play(b, PF_InMission);
    play(d, PF_InMission);
    c.run(800);
    CHECK(a.game.held && b.game.held && d.game.held);  // all wait for D
    // B and C die in the same moment (an explosion right after the scene).
    b.game.playerHealth = 0;
    d.game.playerHealth = 0;
    c.run(800);
    CHECK(a.game.playerHealth == 0 && !a.game.held);  // a frozen player fails too
    CHECK(e.game.playerHealth == 200);                // D is still in its scene
    play(e, PF_InMission);
    c.run(500);
    CHECK(e.game.playerHealth == 0);
    for (Player* p : {&a, &b, &d, &e}) p->game.playerHealth = 200;
    c.run(3000);
    for (Player* p : {&a, &b, &d, &e}) CHECK(p->game.playerHealth == 200);  // each restarted once, no echo
    CHECK(c.count(a, "restarts for everyone") >= 1);
}

COOP_TEST(testStoryPendingFailAcrossReconnect, "story: a failure waiting for the end of a scene survives a reconnect") {
    StoryRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    c.run(1500);
    play(a, PF_InMission);
    play(b, PF_InMission | PF_Cutscene);
    c.run(1500);
    a.game.playerHealth = 0;
    c.run(800);
    CHECK(b.game.playerHealth == 200);  // waits for the end of B's scene
    a.game.playerHealth = 200;
    c.reconnect(b);
    c.run(2000);
    CHECK(b.game.playerHealth == 200);
    play(b, PF_InMission);
    c.run(500);
    CHECK(b.game.playerHealth == 0);  // the mission still restarts for B
    b.game.playerHealth = 200;
    c.run(1500);
    CHECK(a.game.playerHealth == 200);  // and that is no new failure for A

    // A pending failure is dropped when the mission ends in the meantime.
    play(b, PF_InMission | PF_Cutscene);
    c.run(1500);
    a.game.playerHealth = 0;
    c.run(800);
    a.game.playerHealth = 200;
    c.reconnect(b);
    c.run(1500);
    play(b, 0);  // the final scene ends the mission
    c.run(800);
    CHECK(b.game.playerHealth == 200);
}

COOP_TEST(testStoryIneffectiveKill, "story: a shared failure that did not kill us does not swallow our next real death") {
    StoryRig c;
    Player& a = c.join("A", true);
    c.run(200);
    Player& b = c.join("B");
    c.run(1500);
    play(a, PF_InMission);
    play(b, PF_InMission);
    c.run(1500);
    bool shielded = true;  // e.g. the game refused to kill an invincible player
    c.each = [&] {
        if (shielded) b.game.playerHealth = 200;
    };
    a.game.playerHealth = 0;
    c.run(800);
    a.game.playerHealth = 200;
    c.run(6000);
    shielded = false;
    b.game.playerHealth = 0;  // B really dies, much later
    c.run(800);
    CHECK(a.game.playerHealth == 0);
}

// ---------------------------------------------------------------- campaign saves

COOP_TEST(testSaveConcurrentUploadAndDownload, "saves: upload and download at the same time both arrive intact") {
    StoryRig c;
    c.net.latencyMs = 30;
    c.net.loss = 0.02;
    Player& a = c.join("A", true);
    a.saves.latest = randomBytes(150000, 1);
    a.saves.stamp = 1;
    c.run(6000);
    CHECK(c.storage.saves["default"].size() == 1);
    std::vector<uint8_t> campaign = a.saves.latest;

    a.saves.latest = randomBytes(400000, 2);
    a.saves.stamp = 2;
    CHECK(a.session->uploadSave());
    c.run(100);
    CHECK(a.session->requestCampaignSave());  // F10 while F11 is still running
    c.run(20000);
    CHECK(a.saves.written.size() == 1 && a.saves.written[0] == campaign);
    CHECK(c.storage.saves["default"].size() >= 2 && c.storage.saves["default"].back() == a.saves.latest);
    CHECK(a.session->status().saveProgress == -1);
}

COOP_TEST(testSaveNoUploadBack, "saves: a downloaded campaign save is never uploaded back") {
    StoryRig c;
    Player& a = c.join("A", true);
    a.saves.latest = randomBytes(20000, 3);
    a.saves.stamp = 1;
    c.run(3000);
    CHECK(c.storage.saves["default"].size() == 1);
    // A goes back to the campaign save: the copy becomes the newest file, and the game touches it
    // again a little later (loading it, profile updates): the stamp changes, the content does not.
    CHECK(a.session->requestCampaignSave());
    c.run(3000);
    CHECK(a.saves.written.size() == 1);
    a.saves.latest = a.saves.written[0];
    a.saves.stamp = 2;
    c.run(6000);
    a.saves.stamp = 3;
    c.run(6000);
    CHECK(c.storage.saves["default"].size() == 1);
    // A real new save still goes up.
    a.saves.latest[10] ^= 0x55;
    a.saves.stamp = 4;
    c.run(6000);
    CHECK(c.storage.saves["default"].size() == 2 && c.storage.saves["default"].back() == a.saves.latest);
}

COOP_TEST(testSaveUploadInterrupted, "saves: an upload cut off by a reconnect, or a save made offline, still reaches the server") {
    StoryRig c;
    Player& a = c.join("A", true);
    a.saves.latest = randomBytes(5000, 4);
    a.saves.stamp = 1;
    c.run(3000);
    CHECK(c.storage.saves["default"].size() == 1);

    a.saves.latest = randomBytes(600000, 5);
    a.saves.stamp = 2;
    bool cut = false;
    c.each = [&] {
        int progress = a.session->status().saveProgress;
        if (!cut && progress >= 20 && progress <= 70) {
            cut = true;
            c.reconnect(a);
        }
    };
    c.run(25000);
    CHECK(cut);
    CHECK(c.storage.saves["default"].size() == 2 && c.storage.saves["default"].back() == a.saves.latest);
    CHECK(c.saw(a, "again"));

    // A saves while it is offline: the save goes up once it is back.
    c.each = nullptr;
    c.net.unplug(a.sock);
    c.run(12000);
    a.saves.latest = randomBytes(8000, 6);
    a.saves.stamp = 3;
    c.run(3000);
    c.net.plug(a.sock);
    c.run(12000);
    CHECK(a.session->status().worldOwner);
    CHECK(c.storage.saves["default"].size() == 3 && c.storage.saves["default"].back() == a.saves.latest);
}

COOP_TEST(testSaveDownloadInterrupted, "saves: a download cut off by a reconnect resumes by itself") {
    StoryRig c;
    Player& a = c.join("A", true);
    a.saves.latest = randomBytes(500000, 7);
    a.saves.stamp = 1;
    c.run(200);
    Player& b = c.join("B");
    c.run(15000);
    CHECK(c.storage.saves["default"].size() == 1);
    CHECK(b.session->requestCampaignSave());
    bool cut = false;
    c.each = [&] {
        int progress = b.session->status().saveProgress;
        if (!cut && progress >= 20 && progress <= 70) {
            cut = true;
            c.reconnect(b);
        }
    };
    c.run(25000);
    CHECK(cut);
    CHECK(b.saves.written.size() == 1 && b.saves.written[0] == a.saves.latest);
    CHECK(b.session->status().saveProgress == -1);
}

COOP_TEST(testSaveNonOwnerUpload, "saves: a player who does not own the world cannot replace the campaign save") {
    StoryRig c;
    Player& a = c.join("A", true);
    a.saves.latest = randomBytes(5000, 8);
    a.saves.stamp = 1;
    c.run(200);
    Player& b = c.join("B");
    b.saves.latest = randomBytes(5000, 9);
    b.saves.stamp = 1;
    c.run(3000);
    CHECK(c.storage.saves["default"].size() == 1);
    CHECK(!b.session->uploadSave());
    CHECK(c.saw(b, "world owner"));
    c.run(3000);
    CHECK(c.storage.saves["default"].size() == 1);
    CHECK(b.session->status().saveProgress == -1);
}

COOP_TEST(testSaveHandoverNoStaleUpload, "saves: a new world owner does not put an old save of their own on the server") {
    StoryRig c;
    Player& a = c.join("A", true);  // no save yet: the campaign starts empty
    c.run(200);
    Player& b = c.join("B");
    b.saves.latest = randomBytes(5000, 10);  // B's own, unrelated progress
    b.saves.stamp = 5;
    c.run(3000);
    CHECK(c.storage.saves["default"].empty());
    a.saves.latest = randomBytes(5000, 11);  // the owner saves: that starts the campaign
    a.saves.stamp = 1;
    c.run(7000);
    CHECK(c.storage.saves["default"].size() == 1);
    dropPlayer(c, a);  // the world goes to B
    c.run(8000);
    CHECK(b.session->status().worldOwner);
    CHECK(c.storage.saves["default"].size() == 1 && c.storage.saves["default"].back() == a.saves.latest);
    b.saves.latest = randomBytes(5000, 12);  // B's next save as the owner does go up
    b.saves.stamp = 6;
    c.run(7000);
    CHECK(c.storage.saves["default"].size() == 2 && c.storage.saves["default"].back() == b.saves.latest);
}

COOP_TEST(testSaveOwnershipLostMidUpload, "saves: an upload stops with a notice when the world changes hands") {
    StoryRig c;
    Player& a = c.join("A", false);  // owns the world only until someone who asks for it joins
    a.saves.latest = randomBytes(5000, 13);
    a.saves.stamp = 1;
    c.run(3000);
    CHECK(c.storage.saves["default"].size() == 1);
    a.saves.latest = randomBytes(800000, 14);
    a.saves.stamp = 2;
    bool joined = false;
    c.each = [&] {
        if (!joined && a.session->status().saveProgress >= 10) {
            joined = true;
            c.join("W", true);
        }
    };
    c.run(20000);
    CHECK(joined);
    CHECK(!a.session->status().worldOwner);
    CHECK(c.saw(a, "not uploaded"));
    CHECK(a.session->status().saveProgress == -1);
    CHECK(c.storage.saves["default"].size() == 1);
}

COOP_TEST(testSaveLargeTransfer, "saves: large saves (throughput, too large to upload)") {
    StoryRig c;
    // (No jitter here: the link's reliable channel drops records that arrive out of order, so jitter
    // that reorders the datagrams of one burst costs far more than loss; that is the link's to fix.)
    c.net.latencyMs = 40;
    c.net.loss = 0.01;
    Player& a = c.join("A", true);
    a.saves.latest = randomBytes(4u << 20, 15);
    a.saves.stamp = 1;
    c.run(200);
    Player& b = c.join("B");
    uint32_t start = c.now, uploaded = 0;
    while (c.now - start < 120000 && !uploaded) {
        c.run(250);
        if (!c.storage.saves["default"].empty()) uploaded = c.now - start;
    }
    CHECK(uploaded && c.storage.saves["default"].back() == a.saves.latest);
    CHECK(b.session->requestCampaignSave());
    start = c.now;
    uint32_t downloaded = 0;
    while (c.now - start < 120000 && !downloaded) {
        c.run(250);
        if (!b.saves.written.empty()) downloaded = c.now - start;
    }
    CHECK(downloaded && b.saves.written[0] == a.saves.latest);
    std::printf("  4 MB save, 40 ms latency, 1%% loss: upload %.1f s (%.0f KB/s), download %.1f s (%.0f KB/s)\n",
                uploaded / 1000.0, 4096.0 / (uploaded / 1000.0), downloaded / 1000.0, 4096.0 / (downloaded / 1000.0));

    a.saves.latest.assign(kMaxSaveBytes + 1, 1);
    a.saves.stamp = 2;
    c.run(6000);
    CHECK(c.saw(a, "too large"));
    CHECK(c.storage.saves["default"].size() == 1);
}
