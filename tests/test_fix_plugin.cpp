// Fixes in the game plugin: the connection survives Script Hook's script restart (no ghost of the
// player on the server), cached deadlines that never came due after 24.8 days of uptime, the save
// folder a download goes to, and wagons the guest adopted.
#include "harness.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "../plugin/connection.h"
#include "../plugin/rdr2_game.h"
#include "../plugin/save_files.h"
#include "../plugin/scripthook.h"

namespace {

// ---------------------------------------------------------------- script restart

// The plugin's side of a player: the game, the session, and a ServerConnection on one socket that
// outlives the session, as plugin/main.cpp keeps g_socket and g_connection outside the App.
struct PluginPlayer {
    Player* p = nullptr;  // game, saves and session (its own Link stays unused)
    ServerConnection conn;
    LinkConfig config;

    void connect(Cluster& c) {
        c.startServer();
        conn.start(p->sock, config, c.now);
        p->session = std::make_unique<CoopSession>(p->game, conn.link(), c.config);
        p->session->setSaveStore(&p->saves);
    }
    // Script Hook restarts the script: the old App (and with it the session) is dropped, then
    // scriptMain() retires the session it left behind; a reconnect follows (g_reconnect), so no
    // Bye is sent. `cutOff` loses anything the socket would send meanwhile.
    void restart(Cluster& c, bool cutOff) {
        p->session.reset();
        if (cutOff) c.net.unplug(p->sock);
        conn.retire(p->sock, true);
        if (cutOff) c.net.plug(p->sock);
    }
};

PluginPlayer& joinPlugin(Cluster& c, std::vector<std::unique_ptr<PluginPlayer>>& keep, const std::string& name,
                         bool wantsWorld) {
    c.extra.push_back(std::make_unique<Player>());
    Player& p = *c.extra.back();
    p.sock = c.net.open(0x0A000100u + static_cast<uint32_t>(c.extra.size()), 40000);
    keep.push_back(std::make_unique<PluginPlayer>());
    PluginPlayer& pp = *keep.back();
    pp.p = &p;
    pp.config.remote = kServerAddr;
    pp.config.local.role = Role::Guest;
    pp.config.local.name = name;
    pp.config.local.avatar = "player_" + name;
    pp.config.local.campaign = "default";
    pp.config.local.wantsWorld = wantsWorld ? 1 : 0;
    pp.connect(c);
    c.players.push_back(&p);
    return pp;
}

bool anyNameWithSuffix(const std::vector<std::string>& lines) {
    for (const auto& l : lines)
        if (l.find(" 2") != std::string::npos) return true;
    return false;
}

}  // namespace

// Review finding: the world owner loading a save left a ghost connection behind (duplicate "A 2",
// the world stuck with a silent owner, A's town gone at B). Same scenario, with the plugin's
// restart as it is now: same socket, the old session ended with a Bye.
COOP_TEST(testFixPluginSaveLoadKeepsWorldAndName, "fix plugin: the world owner loading a save (script restart) keeps the world and its name") {
    for (uint32_t seed : {1u, 2u, 3u, 4u, 5u}) {
        Cluster c;
        c.net.rng.seed(seed);
        c.net.latencyMs = 30;
        c.net.jitterMs = seed == 1 ? 0 : 15;
        c.net.loss = seed == 1 ? 0.0 : 0.03;
        std::vector<std::unique_ptr<PluginPlayer>> keep;
        const uint32_t kFolk = joaat("a_m_m_townfolk");
        PluginPlayer& a = joinPlugin(c, keep, "A", true);
        c.run(300);
        Player& b = c.join("B");
        a.p->game.player.pos = {0, 0, 0};
        b.game.player.pos = {4, 0, 0};
        for (int i = 0; i < 5; ++i) a.p->game.addPed(kFolk, {float(6 + i), 6, 0});
        c.run(4000);
        CHECK(mirrorOf(b.game, kFolk) != 0);
        const Endpoint before = a.p->sock->self();

        a.restart(c, false);
        c.run(48);
        size_t duringLoad = c.server->playerCount();
        c.run(1500);  // the new script waits for the game to be ready
        a.connect(c);
        c.run(4000);
        std::vector<std::string> lines = c.server->describe();
        std::printf("    seed %u: %s | players while loading %zu, B sees %d players, owner \"%s\", A owns the world: %d, "
                    "A's town at B: %d\n",
                    seed, lines.empty() ? "-" : lines[0].c_str(), duringLoad, b.session->status().players,
                    b.session->status().ownerName.c_str(), a.p->session->status().worldOwner, mirrorOf(b.game, kFolk) != 0);
        CHECK(a.p->sock->self() == before);  // the same port: the server sees A coming back
        CHECK(duringLoad == 2);  // A keeps its place while its game loads...
        CHECK(!journalHas(c, "default", "B now owns the world"));  // ...and the world is not handed to B and back
        CHECK(a.p->session->status().welcomed);
        CHECK(a.p->session->status().worldOwner);
        CHECK(b.session->status().ownerName == "A");
        CHECK(b.session->status().players == 2);
        CHECK(c.server->playerCount() == 2);
        CHECK(lines.size() == 1 && !anyNameWithSuffix(lines));
        CHECK(mirrorOf(b.game, kFolk) != 0);
        CHECK(!journalHas(c, "default", "A 2"));
    }
}

// Review finding 0: a reconnect after the restart was a stranger next to its own ghost. The Bye can
// be lost; the reconnect from the same port still replaces the old session at once, keeps the name,
// and fits a campaign that is full.
COOP_TEST(testFixPluginRestartWithLostBye, "fix plugin: a restart whose goodbye is lost still replaces the old session (full campaign)") {
    for (uint32_t seed : {11u, 12u, 13u, 14u, 15u}) {
        Cluster c;
        c.net.rng.seed(seed);
        c.net.latencyMs = 20;
        c.net.jitterMs = 10;
        c.net.loss = seed == 11 ? 0.0 : 0.03;
        c.serverConfig.maxPlayers = 2;
        std::vector<std::unique_ptr<PluginPlayer>> keep;
        PluginPlayer& a = joinPlugin(c, keep, "A", true);
        c.run(300);
        Player& b = c.join("B");
        c.run(2000);
        CHECK(c.server->playerCount() == 2 && a.p->session->status().worldOwner);

        a.restart(c, true);
        c.run(200);
        CHECK(c.server->playerCount() == 2);  // nothing told the server yet
        a.connect(c);
        c.run(2500);  // well inside the 10 s link timeout: only the reconnect can have cleared the ghost
        std::vector<std::string> lines = c.server->describe();
        std::printf("    seed %u: %s | A welcomed %d, denied \"%s\", A owns the world %d, B sees %d players\n", seed,
                    lines.empty() ? "-" : lines[0].c_str(), a.p->session->status().welcomed,
                    a.p->session->status().denied.c_str(), a.p->session->status().worldOwner, b.session->status().players);
        CHECK(a.p->session->status().welcomed);
        CHECK(a.p->session->status().denied.empty());  // not "campaign is full"
        CHECK(c.server->playerCount() == 2);
        CHECK(lines.size() == 1 && !anyNameWithSuffix(lines));
        CHECK(a.p->session->status().worldOwner);
        CHECK(b.session->status().players == 2 && b.session->status().ownerName == "A");
    }
}

COOP_TEST(testFixPluginGuestRestart, "fix plugin: a guest loading a save comes back under its own name, no frozen copy left") {
    for (uint32_t seed : {21u, 22u, 23u, 24u, 25u}) {
        Cluster c;
        c.net.rng.seed(seed);
        c.net.latencyMs = 25;
        c.net.jitterMs = 10;
        c.net.loss = seed == 21 ? 0.0 : 0.03;
        std::vector<std::unique_ptr<PluginPlayer>> keep;
        Player& a = c.join("A", true);
        c.run(300);
        PluginPlayer& b = joinPlugin(c, keep, "B", false);
        c.run(2000);
        CHECK(a.session->status().players == 2);
        b.restart(c, false);
        c.run(500);
        int playersWhileLoading = a.session->status().players;
        b.connect(c);
        c.run(2500);
        std::vector<std::string> lines = c.server->describe();
        std::printf("    seed %u: %s | A sees %d players while B loads, %d after\n", seed, lines.empty() ? "-" : lines[0].c_str(),
                    playersWhileLoading, a.session->status().players);
        CHECK(playersWhileLoading == 2);  // B keeps its place while its game loads
        CHECK(b.p->session->status().welcomed);
        CHECK(a.session->status().players == 2 && c.server->playerCount() == 2);
        CHECK(lines.size() == 1 && !anyNameWithSuffix(lines));
        CHECK(a.session->status().worldOwner);
    }
}

COOP_TEST(testFixPluginRestartDropsStaleDatagrams, "fix plugin: datagrams for the old session are not taken for the new one") {
    Cluster c;
    std::vector<std::unique_ptr<PluginPlayer>> keep;
    PluginPlayer& a = joinPlugin(c, keep, "A", true);
    c.run(1000);
    CHECK(a.p->session->status().welcomed);
    a.restart(c, false);
    // A server Hello of a session long gone is still waiting on the socket when the new script
    // connects. Taken as the answer, it would make the link "connected" to a session the server
    // does not have, and nothing would arrive until the link timed out.
    HelloInfo stale;
    stale.role = Role::Host;
    stale.name = "server";
    stale.nonce = 0x5EED5EEDu;
    ByteWriter w;
    writePacketHeader(w, PacketType::Hello);
    write(w, stale);
    c.net.inject(kServerAddr, a.p->sock->self(), w.data.data(), w.data.size());
    c.run(16);
    a.connect(c);
    c.run(1500);
    std::printf("    welcomed again within 1.5 s: %d\n", a.p->session->status().welcomed);
    CHECK(a.p->session->status().welcomed);
    CHECK(c.server->playerCount() == 1);
}

// ---------------------------------------------------------------- a minimal Script Hook

namespace fixhook {

// Just enough of the game for the tests below: entities that exist or not, the player on a mount
// or in a vehicle, draft teams, and the mission flag that SET_ENTITY_AS_(MISSION / NO_LONGER_NEEDED)
// change. Everything else answers 0.
struct Mini {
    struct Ent {
        bool vehicle = false, mission = false;
        uint32_t model = 0x1234;
        int draftOf = 0;
        int harness[6] = {};
    };
    uint64_t hash = 0;
    std::vector<uint64_t> args;
    uint64_t ret[4] = {};
    int playerPed = 1, mount = 0, vehicle = 0, lastMount = 0, next = 7;
    std::map<int, Ent> ents{{1, Ent{}}};
    int32_t arg(size_t k) const { return static_cast<int32_t>(static_cast<uint32_t>(args.at(k))); }
    int* ptr(size_t k) const { return reinterpret_cast<int*>(static_cast<uintptr_t>(args.at(k))); }
    Ent* get(int h) {
        auto it = ents.find(h);
        return it == ents.end() ? nullptr : &it->second;
    }
};
Mini* g_mini = nullptr;

void init(uint64_t hash) {
    if (!g_mini) return;
    g_mini->hash = hash;
    g_mini->args.clear();
}
void push(uint64_t v) {
    if (g_mini) g_mini->args.push_back(v);
}
uint64_t* call() {
    static uint64_t zeros[4];
    if (!g_mini) {
        std::memset(zeros, 0, sizeof zeros);
        return zeros;
    }
    Mini& m = *g_mini;
    std::memset(m.ret, 0, sizeof m.ret);
    auto retI = [&](int32_t v) { std::memcpy(&m.ret[0], &v, 4); };
    Mini::Ent* e0 = m.args.empty() ? nullptr : m.get(m.arg(0));
    switch (m.hash) {
        case 0x096275889B8E0EE0: retI(m.playerPed); break;                                    // PLAYER_PED_ID
        case 0xD42BD6EB2E0F1677: retI(e0 ? 1 : 0); break;                                     // DOES_ENTITY_EXIST
        case 0x82368787EA73C0F7: retI(300); break;                                            // GET_ENTITY_HEALTH
        case 0x15D757606D170C3C: retI(300); break;                                            // GET_ENTITY_MAX_HEALTH
        case 0x460BC76A0E10655E: retI(m.arg(0) == m.playerPed && m.mount ? 1 : 0); break;     // IS_PED_ON_MOUNT
        case 0xE7E11B8DCBED1058: retI(m.arg(0) == m.playerPed ? m.mount : 0); break;          // GET_MOUNT
        case 0x4C8B59171957BCF7: retI(m.arg(0) == m.playerPed ? m.lastMount : 0); break;      // _GET_LAST_MOUNT
        case 0x997ABD671D25CA0B: retI(m.arg(0) == m.playerPed && m.vehicle ? 1 : 0); break;   // IS_PED_IN_ANY_VEHICLE
        case 0x9A9112A0FE9A4713: retI(m.arg(0) == m.playerPed ? m.vehicle : 0); break;        // GET_VEHICLE_PED_IS_IN
        case 0xB980061DA992779D: retI(m.arg(0) == m.playerPed ? 1 : 0); break;                // IS_PED_HUMAN
        case 0x12534C348C6CB68B: retI(m.arg(0) == m.playerPed ? 1 : 0); break;                // IS_PED_A_PLAYER
        case 0xDA76A9F39210D365: retI(e0 ? static_cast<int32_t>(e0->model) : 0); break;       // GET_ENTITY_MODEL
        case 0x392C8D8E07B70EFC: retI(1); break;                                              // IS_MODEL_VALID
        case 0x1283B8B89DD5D1B6: retI(1); break;                                              // HAS_MODEL_LOADED
        case 0x138190F64DB4BBD1: retI(e0 && e0->mission ? 1 : 0); break;                      // IS_ENTITY_A_MISSION_ENTITY
        case 0xCF8176912DDA4EA5: retI(e0 && !e0->vehicle ? 1 : 0); break;                     // IS_ENTITY_A_PED
        case 0xE4770DA1B8FF4FD1: retI(e0 ? e0->draftOf : 0); break;                           // _GET_VEHICLE_DRAFT_HORSE_IS_ATTACHED_TO
        case 0xA8BA0BAE0173457B: {                                                            // _GET_PED_IN_DRAFT_HARNESS
            int slot = m.arg(1);
            retI(e0 && slot >= 0 && slot < 6 ? e0->harness[slot] : 0);
            break;
        }
        case 0xDC19C288082E586E: if (e0) e0->mission = true; break;                           // SET_ENTITY_AS_MISSION_ENTITY
        case 0x4971D2F8162B9674:                                                              // SET_ENTITY_AS_NO_LONGER_NEEDED
            if (Mini::Ent* e = m.get(*m.ptr(0))) e->mission = false;
            break;
        case 0xD49F9B0955C367DE:                                                              // CREATE_PED
            m.ents[m.next] = Mini::Ent{};
            retI(m.next++);
            break;
        case 0xAF35D0D2583051B0: {                                                            // CREATE_VEHICLE: with its team
            int veh = m.next++;
            m.ents[veh].vehicle = true;
            for (int k = 0; k < 2; ++k) {
                int horse = m.next++;
                m.ents[horse].draftOf = veh;
                m.ents[veh].harness[k] = horse;
            }
            retI(veh);
            break;
        }
        case 0xCC0EF140F99365C5:                                                              // DELETE_PED
        case 0xE20A909D8C4A70F8: {                                                            // DELETE_VEHICLE
            int* h = m.ptr(0);
            m.ents.erase(*h);
            for (auto& [k, e] : m.ents) {
                if (e.draftOf == *h) e.draftOf = 0;
                for (int& s : e.harness)
                    if (s == *h) s = 0;
            }
            *h = 0;
            break;
        }
        default: break;
    }
    return m.ret;
}
int peds(int* out, int cap) {
    int n = 0;
    for (auto& [h, e] : g_mini->ents)
        if (!e.vehicle && n < cap) out[n++] = h;
    return n;
}
int vehicles(int* out, int cap) {
    int n = 0;
    for (auto& [h, e] : g_mini->ents)
        if (e.vehicle && n < cap) out[n++] = h;
    return n;
}
struct Installed {
    explicit Installed(Mini& m) {
        g_mini = &m;
        scripthook::g_api.nativeInit = init;
        scripthook::g_api.nativePush64 = push;
        scripthook::g_api.nativeCall = call;
        scripthook::g_api.worldGetAllPeds = peds;
        scripthook::g_api.worldGetAllVehicles = vehicles;
    }
    ~Installed() { g_mini = nullptr; }
};

// What the session's foreign-population pass does for a guest (session.cpp, clearForeign): any
// local entity that is not the player's, nor pulling something of theirs, is removed.
void clearForeign(Rdr2Game& game) {
    std::vector<ForeignEntity> foreign;
    game.listForeign(foreign);
    for (const ForeignEntity& f : foreign) {
        if (game.belongsToLocalPlayer(f.handle)) continue;
        if (f.draftOf && game.belongsToLocalPlayer(f.draftOf)) continue;
        (void)game.removeForeign(f.handle);
    }
}

}  // namespace fixhook

// ---------------------------------------------------------------- 24.8 days of uptime

// Review finding 13 (the reviewers' repro): the clock is the performance counter since boot, cut to
// 32 bits; past 2^31 ms a deadline of 0 is "in the future" for the wrap-safe comparison.
COOP_TEST(testFixPluginDeadlinesAfter25DaysUptime, "fix plugin: zero deadlines come due after 24.8 days of uptime too") {
    for (uint32_t start : {1000u, 0x80000000u + 1000u, 0xFFFFF000u}) {
        fixhook::Mini m;
        fixhook::Installed hook(m);
        Rdr2Game game;
        uint32_t now = start;
        game.frame(now, true);
        LocalPlayer lp = game.readLocalPlayer();
        std::printf("    clock %u ms: max health reported %u (game: 300)\n", start, lp.state.maxHealth);
        CHECK(lp.state.maxHealth == 300);
        EntityHandle horse = game.spawn(EntityKind::Ped, 0x1234, {}, 0, SpawnRole::Mirror);
        CHECK(horse != 0);
        m.mount = horse;
        now += 16;
        game.frame(now, true);
        (void)game.readLocalPlayer();  // learns the horse's model: its cached facts are created here
        m.ents.erase(horse);           // the horse is gone
        m.mount = 0;
        for (int frame = 0; frame < 120; ++frame) {
            now += 16;
            game.frame(now, true);
        }
        bool exists = game.exists(horse);
        std::printf("    clock %u ms: deleted horse still exists for the mod: %d\n", start, exists ? 1 : 0);
        CHECK(!exists);
        // A horse that is there is still only asked about every kAliveMs, not every frame.
        EntityHandle other = game.spawn(EntityKind::Ped, 0x1234, {}, 0, SpawnRole::Mirror);
        now += 16;
        game.frame(now, true);
        CHECK(game.exists(other));
        m.ents.erase(other);
        now += 16;
        game.frame(now, true);
        CHECK(game.exists(other));  // cached for a moment: the cache is real
        for (int frame = 0; frame < 20; ++frame) {
            now += 16;
            game.frame(now, true);
        }
        CHECK(!game.exists(other));
    }
}

// ---------------------------------------------------------------- adopted wagons

// Review finding 15: the reviewers' repro drives FakeGame (tests/harness.h), which only models
// Rdr2Game; this is the same scenario on Rdr2Game itself, with the session's foreign pass.
COOP_TEST(testFixPluginAdoptedWagonSurvivesStepOff, "fix plugin: a wagon the guest adopted is not deleted when they step off") {
    fixhook::Mini m;
    fixhook::Installed hook(m);
    Rdr2Game game;
    uint32_t now = 5000;
    game.frame(now, true);
    EntityHandle wagon = game.spawn(EntityKind::Vehicle, 0x1234, {10, 2, 0}, 0, SpawnRole::Mirror);
    CHECK(wagon != 0 && m.get(wagon) && m.get(wagon)->mission);
    const int team0 = m.get(wagon)->harness[0], team1 = m.get(wagon)->harness[1];
    EntityHandle ambient = game.spawn(EntityKind::Vehicle, 0x1234, {40, 2, 0}, 0, SpawnRole::Mirror);
    CHECK(game.adopt(ambient));  // adopted, but then deleted by the game below: forgotten
    m.vehicle = wagon;           // the guest drives the mirror
    for (int frame = 0; frame < 30; ++frame) {
        now += 16;
        game.frame(now, true);
    }
    CHECK(game.belongsToLocalPlayer(wagon));
    // The owner's game culled the original: the mirror becomes an ordinary entity of this world.
    CHECK(game.adopt(wagon));
    CHECK(!m.get(wagon)->mission);
    now += 16;
    game.frame(now, true);
    fixhook::clearForeign(game);
    CHECK(m.get(wagon) != nullptr);
    m.vehicle = 0;  // the guest steps off to loot
    for (int frame = 0; frame < 64; ++frame) {  // about a second of foreign passes
        now += 16;
        game.frame(now, true);
        if (frame % 8 == 0) fixhook::clearForeign(game);
    }
    bool still = m.get(wagon) != nullptr;
    bool teamStill = m.get(team0) != nullptr && m.get(team1) != nullptr;
    std::printf("    adopted wagon one second after stepping off: %s, its team: %s\n", still ? "still there" : "deleted",
                teamStill ? "still there" : "deleted");
    CHECK(still);
    CHECK(teamStill);
    CHECK(game.belongsToLocalPlayer(wagon));

    // A local wagon that was never the player's is still foreign population.
    int stranger = m.next++;
    m.ents[stranger].vehicle = true;
    now += 16;
    game.frame(now, true);
    fixhook::clearForeign(game);
    CHECK(m.get(stranger) == nullptr);

    // Once the game cleans the adopted wagon up, its handle is not kept as the player's.
    m.ents.erase(ambient);
    m.ents.erase(wagon);
    for (int frame = 0; frame < 40; ++frame) {
        now += 16;
        game.frame(now, true);
    }
    CHECK(!game.belongsToLocalPlayer(wagon));
    CHECK(!game.belongsToLocalPlayer(ambient));
}

// ---------------------------------------------------------------- save folders

namespace {
namespace fs = std::filesystem;
void putSave(const fs::path& file, int minutesAgo, size_t size = 1000) {
    { std::ofstream(file, std::ios::binary) << std::string(size, 'x'); }
    fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::minutes(minutesAgo));
}
}  // namespace

// Review finding 14 (the reviewers' repro, plus the tie and the uploads).
COOP_TEST(testFixPluginSaveCopyGoesToTheProfile, "fix plugin: a 'backup' folder never takes the campaign save or the upload") {
    const fs::path root = "coop-fix-plugin-saves";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "D4E5F6A7");  // the real Rockstar profile
    fs::create_directories(root / "backup");    // the player's own backup of their saves
    putSave(root / "D4E5F6A7" / "SRDR30000", 60, 1000);  // the player's save
    putSave(root / "backup" / "SRDR30000", 30, 2000);    // copied into the backup folder afterwards
    {
        saves::SaveFolder folder(root);
        std::vector<uint8_t> data(3000, 7);
        std::string placed, error;
        CHECK(folder.writeCopy(data, placed, error));
        bool inProfile = fs::exists(root / "D4E5F6A7" / placed);
        bool inBackup = fs::exists(root / "backup" / placed);
        std::printf("    campaign save stored as %s: in the profile %d, in the backup folder %d\n", placed.c_str(), inProfile,
                    inBackup);
        CHECK(inProfile);
        CHECK(!inBackup);
        fs::remove(root / "D4E5F6A7" / placed, ec);
    }
    {
        // What the owner uploads is the profile's save, not the newer backup.
        saves::SaveFolder folder(root);
        std::vector<uint8_t> data;
        std::string name;
        CHECK(folder.readLatest(data, name));
        CHECK(data.size() == 1000);
        saves::Found f;
        CHECK(saves::findLatest(root, f) && f.profile.filename() == "D4E5F6A7");
    }
    {
        // A tie (an Explorer copy keeps the time): the profile, whichever folder is listed first.
        fs::last_write_time(root / "backup" / "SRDR30000", fs::last_write_time(root / "D4E5F6A7" / "SRDR30000"));
        saves::Found f;
        CHECK(saves::findLatest(root, f) && f.profile.filename() == "D4E5F6A7" && f.size == 1000);
    }
    {
        // Within the profiles the newest save still wins.
        fs::create_directories(root / "0A0B0C0D");
        putSave(root / "0A0B0C0D" / "SRDR30003", 5, 1500);
        saves::Found f;
        CHECK(saves::findLatest(root, f) && f.profile.filename() == "0A0B0C0D" && f.size == 1500);
        fs::remove_all(root / "0A0B0C0D", ec);
    }
    {
        // No profile holds a save yet, only the backup: uploads may use it (nothing better), but a
        // download goes to the profile folder the game reads.
        fs::remove(root / "D4E5F6A7" / "SRDR30000", ec);
        saves::SaveFolder folder(root);
        std::vector<uint8_t> data;
        std::string name;
        CHECK(folder.readLatest(data, name) && data.size() == 2000);
        std::vector<uint8_t> incoming(500, 1);
        std::string placed, error;
        CHECK(folder.writeCopy(incoming, placed, error));
        CHECK(fs::exists(root / "D4E5F6A7" / placed) && fs::file_size(root / "D4E5F6A7" / placed, ec) == 500);
    }
    fs::remove_all(root, ec);
}
