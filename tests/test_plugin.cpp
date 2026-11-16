// The game plugin's native layer, tested without the game: Rdr2Game is built against a simulated
// Script Hook that answers natives from a small world model. Every call is checked against the
// community native database (hash known, argument count, not one of the dead natives), and
// counted, so the cost of the mod per frame can be measured.
#include "harness.h"

#include <cstring>
#include <deque>
#include <set>
#include <unordered_map>

#include "../plugin/rdr2_game.cpp"
#include "../plugin/save_files.h"
#include "../plugin/settings.h"

scripthook::Api scripthook::g_api;

namespace fakehook {

// Every native the plugin may call: hash, name and parameter count from alloc8or/rdr3-nativedb-data
// (natives.json), plus the HUD natives that turned into empty stubs in game build 1436.
struct Spec {
    uint64_t hash;
    const char* name;
    int argc;   // -1: variadic
    bool dead;  // the game ignores the call
};
const Spec kNativeDb[] = {
    {0x0252D2B5582957A6, "CAMERA::GET_GAMEPLAY_CAM_ROT", 1, false},
    {0x028F76B6E78246EB, "PED::SET_PED_ONTO_MOUNT", 4, false},
    {0x04AF77971E508F6A, "CAMERA::GET_FINAL_RENDERED_CAM_FOV", 0, false},
    {0x06843DA7060A026B, "ENTITY::SET_ENTITY_COORDS", 8, false},
    {0x0918E3565C20F03C, "ENTITY::_SET_ENTITY_COORDS_AND_HEADING_NO_OFFSET", 7, false},
    {0x096275889B8E0EE0, "PLAYER::PLAYER_PED_ID", 0, false},
    {0x12534C348C6CB68B, "PED::IS_PED_A_PLAYER", 1, false},
    {0x1283B8B89DD5D1B6, "STREAMING::HAS_MODEL_LOADED", 1, false},
    {0x138190F64DB4BBD1, "ENTITY::IS_ENTITY_A_MISSION_ENTITY", 1, false},
    {0x15D757606D170C3C, "ENTITY::GET_ENTITY_MAX_HEALTH", 2, false},
    {0x166E7CF68597D8B5, "ENTITY::SET_ENTITY_MAX_HEALTH", 2, false},
    {0x16794E044C9EFB58, "UIDEBUG::_BG_DISPLAY_TEXT", 3, false},
    {0x16FA5CE47F184F1E, "UIDEBUG::_BG_SET_TEXT_COLOR", 4, false},
    {0x1794B4FCC84D812F, "ENTITY::SET_ENTITY_VISIBLE", 2, false},
    {0x1902C4CFCC5BE57C, "PED::_EQUIP_META_PED_OUTFIT", 2, false},
    {0x1C99BB7B6E96D16F, "ENTITY::SET_ENTITY_VELOCITY", 4, false},
    {0x1F91D44490E1EA0C, "VEHICLE::SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME", 1, false},
    {0x202B1BBFC6AB5EE4, "MAP::IS_WAYPOINT_ACTIVE", 0, false},
    {0x217E9DC48139933D, "PLAYER::PLAYER_ID", 0, false},
    {0x239A3351AC1DA385, "ENTITY::SET_ENTITY_COORDS_NO_OFFSET", 7, false},
    {0x23F74C2FDA6E7C61, "MAP::BLIP_ADD_FOR_ENTITY", 2, false},
    {0x25F6EF88664540E2, "STREAMING::SET_FOCUS_POS_AND_VEL", 6, false},
    {0x27666E5988D9D429, "CAMERA::SET_CAM_FOV", 2, false},
    {0x283978A15512B2FE, "PED::_SET_RANDOM_OUTFIT_VARIATION", 2, false},
    {0x28CB6391ACEDD9DB, "PED::_SET_SCENARIO_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME", 1, false},
    {0x29B30D07C3F7873B, "MAP::_GET_WAYPOINT_COORDS", 0, false},
    {0x2A08A32B6D49906F, "ENTITY::_GET_ENTITY_SCRIPT", 2, false},
    {0x30569F348D126A5A, "PED::_GET_PED_META_OUTFIT_HASH", 1, false},
    {0x33281167E4942E4F, "CAMERA::RENDER_SCRIPT_CAMS", 6, false},
    {0x34616828CD07F1A1, "PED::IS_PED_SHOOTING", 1, false},
    {0x392C8D8E07B70EFC, "STREAMING::IS_MODEL_VALID", 1, false},
    {0x3A52C59FFB2DEED8, "CLOCK::SET_CLOCK_TIME", 3, false},
    {0x3EDCB0505123623B, "WEAPON::SET_PED_INFINITE_AMMO", 3, false},
    {0x40C719A5E410B9E4, "CAMERA::DO_SCREEN_FADE_OUT", 1, false},
    {0x4605C66E0F935F83, "PLAYER::IS_PLAYER_TARGETTING_ANYTHING", 1, false},
    {0x460BC76A0E10655E, "PED::IS_PED_ON_MOUNT", 1, false},
    {0x46A6CC01E0826106, "TASK::TASK_SHOOT_AT_COORD", 7, false},
    {0x47E4E977581C5B55, "PED::IS_PED_RAGDOLL", 1, false},
    {0x4805D2B1D8CF94A9, "ENTITY::GET_ENTITY_VELOCITY", 2, false},
    {0x4859F1FC66A6278E, "PED::IS_PED_IN_COMBAT", 2, false},
    {0x4A48B6E03BABB4AC, "PED::_SET_PED_PROMPT_NAME", 2, false},
    {0x4AD96EF928BD4F9A, "STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED", 1, false},
    {0x4BEB42AEBCA732E9, "MISC::_GET_PREV_WEATHER_TYPE_HASH_NAME", 0, false},
    {0x4C8B59171957BCF7, "PED::_GET_LAST_MOUNT", 1, false},
    {0x4E162231B823DBBF, "CLOCK::GET_CLOCK_MINUTES", 0, false},
    {0x4E67E0B6D7FD5145, "CAMERA::DESTROY_CAM", 2, false},
    {0x4FF674F5E23D49CE, "MAP::SET_BLIP_COORDS", 4, false},
    {0x50A41AD966910F03, "HUD::_SET_TEXT_COLOR", 4, true},
    {0x51021D36F62AAA83, "MISC::_GET_NEXT_WEATHER_TYPE_HASH_NAME", 0, false},
    {0x5337B721C51883A9, "PED::_REMOVE_PED_FROM_MOUNT", 3, false},
    {0x5352E025EC2B416F, "CAMERA::GET_FINAL_RENDERED_CAM_COORD", 0, false},
    {0x554D9D53F696D002, "MAP::BLIP_ADD_FOR_COORDS", 4, false},
    {0x59174F1AFE095B5A, "MISC::SET_WEATHER_TYPE", 6, false},
    {0x595320200B98596E, "CAMERA::GET_GAMEPLAY_CAM_COORD", 0, false},
    {0x5E3BDDBCB83F3D84, "WEAPON::GIVE_WEAPON_TO_PED", 13, false},
    {0x5F4B6931816E599B, "PAD::DISABLE_ALL_CONTROL_ACTIONS", 1, false},
    {0x602685BD85DD26CA, "CAMERA::GET_FINAL_RENDERED_CAM_ROT", 1, false},
    {0x606374EBFC27B133, "VEHICLE::SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME", 1, false},
    {0x63DFA6810AD78719, "CAMERA::SET_CAM_ROT", 5, false},
    {0x6671F3EEC681BDA1, "TASK::TASK_AIM_GUN_AT_COORD", 7, false},
    {0x697157CED63F18D4, "PED::APPLY_DAMAGE_TO_PED", 5, false},
    {0x6A053CF596F67DF7, "CAMERA::DO_SCREEN_FADE_IN", 1, false},
    {0x6C4D0409BA1A2BC2, "WEAPON::GET_PED_LAST_WEAPON_IMPACT_COORD", 2, false},
    {0x71BC8E838B9C6035, "PED::RESURRECT_PED", 1, false},
    {0x71D4BF5890659B0C, "DLC::GET_IS_LOADING_SCREEN_ACTIVE", 0, false},
    {0x7964097FCE4C244B, "PLAYER::IS_PLAYER_CONTROL_ON", 1, false},
    {0x7A556143A1C03898, "PED::SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME", 1, false},
    {0x7B6E7BEC1143AC86, "ENTITY::HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY", 4, false},
    {0x7D5B1F88E7504BBA, "ENTITY::IS_ENTITY_DEAD", 1, false},
    {0x7D9EFB7AD6B19754, "ENTITY::FREEZE_ENTITY_POSITION", 2, false},
    {0x7DBDD04862D95F04, "PED::GET_PED_RELATIONSHIP_GROUP_HASH", 1, false},
    {0x7DE9692C6F64CFE8, "PED::_SET_PED_CROUCH_MOVEMENT", 4, false},
    {0x82368787EA73C0F7, "ENTITY::GET_ENTITY_HEALTH", 1, false},
    {0x8425C5F057012DAB, "WEAPON::_GET_PED_CURRENT_HELD_WEAPON", 1, false},
    {0x86CCAF7CE493EFBE, "STREAMING::CLEAR_FOCUS", 0, false},
    {0x87295BCA613800C8, "CAMERA::SET_CAM_ACTIVE", 2, false},
    {0x8D8ACD8388CD99CE, "PED::REVIVE_INJURED_PED", 1, false},
    {0x919BE13EED931959, "TASK::TASK_STAND_STILL", 2, false},
    {0x936F967D4BE1CE9D, "PLAYER::IS_PLAYER_FREE_AIMING", 1, false},
    {0x9682AF6050854856, "MISC::SET_TIME_SCALE", 1, false},
    {0x971D38760FBC02EF, "PED::SET_PED_KEEP_TASK", 2, false},
    {0x997ABD671D25CA0B, "PED::IS_PED_IN_ANY_VEHICLE", 2, false},
    {0x9A9112A0FE9A4713, "PED::GET_VEHICLE_PED_IS_IN", 2, false},
    {0x9CB1A1623062F402, "MAP::_SET_BLIP_NAME", 2, false},
    {0x9CC8314DFEDE441E, "ENTITY::SET_ENTITY_ROTATION", 6, false},
    {0x9F8AA94D6D97DBF4, "PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS", 2, false},
    {0xA1253A3C870B6843, "UIDEBUG::_BG_SET_TEXT_SCALE", 2, false},
    {0xA5C38736C426FCB8, "ENTITY::SET_ENTITY_INVINCIBLE", 2, false},
    {0xA86D5F069399F44D, "ENTITY::GET_ENTITY_COORDS", 3, false},
    {0xA8BA0BAE0173457B, "VEHICLE::_GET_PED_IN_DRAFT_HARNESS", 2, false},
    {0xAA5A7ECE2AA8FE70, "PED::SET_PED_DESIRED_HEADING", 2, false},
    {0xAAA34F8A7CB32098, "TASK::CLEAR_PED_TASKS_IMMEDIATELY", 3, false},
    {0xAB0D553FE20A6E25, "PED::_SET_AMBIENT_PED_DENSITY_MULTIPLIER_THIS_FRAME", 1, false},
    {0xAC2767ED8BDFAB15, "ENTITY::SET_ENTITY_HEALTH", 3, false},
    {0xADF692B254977C0C, "WEAPON::SET_CURRENT_PED_WEAPON", 6, false},
    {0xAE99FB955581844A, "PED::SET_PED_TO_RAGDOLL", 7, false},
    {0xAF35D0D2583051B0, "VEHICLE::CREATE_VEHICLE", 9, false},
    {0xB15CD1CF58771DE1, "MISC::GET_MISSION_FLAG", 0, false},
    {0xB48050D326E9A2F3, "PLAYER::_GET_SADDLE_HORSE_FOR_PLAYER", 1, false},
    {0xB6101ABE62B5F080, "CLOCK::GET_CLOCK_SECONDS", 0, false},
    {0xB980061DA992779D, "PED::IS_PED_HUMAN", 1, false},
    {0xBA0980B5C0A11924, "PED::_SET_AMBIENT_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME", 1, false},
    {0xBB19AC7D4DCEFD0F, "ENTITY::CLEAR_ENTITY_LAST_DAMAGE_ENTITY", 1, false},
    {0xBB40DD2270B65366, "VEHICLE::GET_PED_IN_VEHICLE_SEAT", 2, false},
    {0xC0258742B034DFAF, "PED::_SET_AMBIENT_ANIMAL_DENSITY_MULTIPLIER_THIS_FRAME", 1, false},
    {0xC230DD956E2F5507, "ENTITY::GET_ENTITY_HEADING", 1, false},
    {0xC80A74AC829DDD92, "PED::SET_PED_RELATIONSHIP_GROUP_HASH", 2, false},
    {0xC82CF208C2B19199, "CLOCK::GET_CLOCK_HOURS", 0, false},
    {0xCC0EF140F99365C5, "PED::DELETE_PED", 1, false},
    {0xCC8CA3E88256E58F, "PED::_UPDATE_PED_VARIATION", 6, false},
    {0xCD66FEA29400A0B5, "PED::GET_CURRENT_TARGET_FOR_PED", 1, false},
    {0xCF8176912DDA4EA5, "ENTITY::IS_ENTITY_A_PED", 1, false},
    {0xD42BD6EB2E0F1677, "ENTITY::DOES_ENTITY_EXIST", 1, false},
    {0xD49F9B0955C367DE, "PED::CREATE_PED", 9, false},
    {0xD5FE956C70FF370B, "PED::GET_PED_CROUCH_MOVEMENT", 1, false},
    {0xD76B57B44F1E6F8B, "TASK::TASK_GO_STRAIGHT_TO_COORD", 9, false},
    {0xD77AE48611B7B10A, "PED::SET_PED_TO_PLAYER_WEAPON_DAMAGE_MODIFIER", 2, false},
    {0xD79334A4BB99BAD1, "HUD::_DISPLAY_TEXT", 3, true},
    {0xDA76A9F39210D365, "ENTITY::GET_ENTITY_MODEL", 1, false},
    {0xDB48E99F8E064E56, "PED::_SET_SCENARIO_ANIMAL_DENSITY_MULTIPLIER_THIS_FRAME", 1, false},
    {0xDC19C288082E586E, "ENTITY::SET_ENTITY_AS_MISSION_ENTITY", 3, false},
    {0xE09CAF86C32CB48F, "ENTITY::GET_ENTITY_ROTATION", 2, false},
    {0xE1EF3C1216AFF2CD, "TASK::CLEAR_PED_TASKS", 3, false},
    {0xE20A909D8C4A70F8, "VEHICLE::DELETE_VEHICLE", 1, false},
    {0xE4770DA1B8FF4FD1, "PED::_GET_VEHICLE_DRAFT_HORSE_IS_ATTACHED_TO", 1, false},
    {0xE72CDBA7F0A02DD6, "CAMERA::CREATE_CAM", 2, false},
    {0xE7E11B8DCBED1058, "PED::GET_MOUNT", 1, false},
    {0xF2C3C9DA47AAA54A, "MAP::REMOVE_BLIP", 1, false},
    {0xF49F14462F0AE27C, "PLAYER::GET_MOUNT_OWNED_BY_PLAYER", 1, false},
    {0xF66F820909453B8C, "ENTITY::SET_ENTITY_COLLISION", 3, false},
    {0xF75B0D629E1C063D, "PED::SET_PED_INTO_VEHICLE", 3, false},
    {0xF9EE7D419EE49DE6, "CAMERA::SET_CAM_COORD", 4, false},
    {0xFA28FE3A6246FC30, "STREAMING::REQUEST_MODEL", 2, false},
    {0xFA925AC00EB830B9, "MISC::VAR_STRING", -1, false},
    {0xFC08C8F8C1EDF174, "VEHICLE::IS_THIS_MODEL_A_TRAIN", 1, false},
    {0xFEDFA97638D61D4A, "VEHICLE::SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME", 1, false},
};

struct Ent {
    bool vehicle = false;
    uint32_t model = 0;
    Vec3 pos, vel, rot;  // rot: pitch, roll, heading
    Vec3 orbit;          // moving peds walk in a small circle around this point
    float orbitPhase = 0;
    int health = 150, maxHealth = 150;
    bool human = true, mission = false, ragdoll = false, combat = false, crouch = false, shooting = false;
    bool visible = true, moving = false;
    uint32_t script = 0, outfit = 0, weapon = 0;
    int mount = 0, lastMount = 0, inVehicle = 0, target = 0, draftOf = 0;
    int harness[6] = {};
    int seats[10] = {};  // seat -1..8
    int tasks = 0, coordReads = 0;
};

struct World;
using Handler = void (*)(World&);

struct World {
    std::map<int, Ent> ents;
    int next = 0x1001;
    int playerPed = 0, ownHorse = 0;
    bool missionFlag = false, controlOn = true, loading = false, freeAiming = false, targetting = false;
    bool waypointActive = false;
    Vec3 waypoint{100, 200, 0};
    int hour = 12, minute = 0, second = 0;
    uint32_t weather = joaat("SUNNY"), nextWeather = joaat("SUNNY");
    int weatherSets = 0;
    std::set<uint32_t> trains, animals;
    int nextBlip = 1, nextCam = 1;
    std::set<int> blips;
    bool renderingScriptCams = false;
    float timeScale = 1.0f;

    // the native call in progress
    const Spec* spec = nullptr;
    std::vector<uint64_t> args;
    uint64_t ret[4] = {};

    uint64_t calls = 0;
    std::map<std::string, uint64_t> byName;
    std::vector<std::string> problems;   // unknown hashes, wrong argument counts, dead natives
    std::vector<std::string> displayed;  // text drawn with UIDEBUG::_BG_DISPLAY_TEXT
    std::deque<std::string> strings;     // VAR_STRING results (kept alive like the game's buffer)

    std::unordered_map<uint64_t, const Spec*> specs;
    std::unordered_map<std::string, Handler> handlers;

    World();
    ~World();

    int add(const Ent& e) {
        int h = next++;
        ents[h] = e;
        return h;
    }
    int addPed(uint32_t model, Vec3 pos) {
        Ent e;
        e.model = model;
        e.pos = pos;
        e.human = !animals.count(model);
        return add(e);
    }
    int addVehicle(uint32_t model, Vec3 pos) {
        Ent e;
        e.vehicle = true;
        e.model = model;
        e.pos = pos;
        return add(e);
    }
    Ent* get(int h) {
        auto it = ents.find(h);
        return it == ents.end() ? nullptr : &it->second;
    }
    void seat(int ped, int vehicle, int seatIndex) {
        ents[ped].inVehicle = vehicle;
        ents[vehicle].seats[seatIndex + 1] = ped;
    }
    void ride(int rider, int horse) {
        ents[rider].mount = horse;
        ents[rider].lastMount = horse;
    }
    void erase(int h) {
        ents.erase(h);
        for (auto& [k, e] : ents) {
            if (e.mount == h) e.mount = 0;
            if (e.inVehicle == h) e.inVehicle = 0;
            for (int& s : e.seats)
                if (s == h) s = 0;
            for (int& s : e.harness)
                if (s == h) s = 0;
        }
    }
    uint64_t count(const char* name) const {
        auto it = byName.find(name);
        return it == byName.end() ? 0 : it->second;
    }

    // One game frame: moving peds walk their circle, riders and passengers follow what carries them.
    void simulate(float dt) {
        for (auto& [h, e] : ents) {
            if (!e.moving) continue;
            e.orbitPhase += dt * 0.3f;
            Vec3 to{e.orbit.x + 5.0f * std::cos(e.orbitPhase), e.orbit.y + 5.0f * std::sin(e.orbitPhase), e.orbit.z};
            e.vel = (to - e.pos) * (1.0f / dt);
            e.pos = to;
            e.rot.z = wrapHeading(e.orbitPhase * 57.2958f);
        }
        for (auto& [h, e] : ents) {
            if (e.mount && ents.count(e.mount)) e.pos = ents[e.mount].pos;
            if (e.inVehicle && ents.count(e.inVehicle)) e.pos = ents[e.inVehicle].pos;
        }
        strings.clear();
        displayed.clear();
    }

    // argument access
    int32_t i(size_t k) const { return static_cast<int32_t>(static_cast<uint32_t>(args.at(k))); }
    uint32_t u(size_t k) const { return static_cast<uint32_t>(args.at(k)); }
    float f(size_t k) const {
        uint32_t raw = u(k);
        float v;
        std::memcpy(&v, &raw, sizeof v);
        return v;
    }
    Vec3 v3(size_t k) const { return {f(k), f(k + 1), f(k + 2)}; }
    template <typename T>
    T* p(size_t k) const {
        return reinterpret_cast<T*>(static_cast<uintptr_t>(args.at(k)));
    }
    Ent* e0() { return get(i(0)); }
    void retI(int64_t v) {
        int32_t x = static_cast<int32_t>(v);
        std::memcpy(&ret[0], &x, sizeof x);
    }
    void retF(float v) { std::memcpy(&ret[0], &v, sizeof v); }
    void retV(const Vec3& v) {
        std::memcpy(&ret[0], &v.x, 4);
        std::memcpy(&ret[1], &v.y, 4);
        std::memcpy(&ret[2], &v.z, 4);
    }
    void retP(const void* ptr) { ret[0] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr)); }
};

World* g_world = nullptr;

void hookInit(uint64_t hash) {
    World& w = *g_world;
    w.args.clear();
    auto it = w.specs.find(hash);
    w.spec = it == w.specs.end() ? nullptr : it->second;
    if (!w.spec) {
        char text[64];
        std::snprintf(text, sizeof text, "unknown native 0x%016llX", static_cast<unsigned long long>(hash));
        w.problems.push_back(text);
    }
}
void hookPush(uint64_t value) { g_world->args.push_back(value); }
uint64_t* hookCall() {
    World& w = *g_world;
    std::memset(w.ret, 0, sizeof w.ret);
    ++w.calls;
    if (!w.spec) return w.ret;
    ++w.byName[w.spec->name];
    if (w.spec->dead) w.problems.push_back(std::string("dead native called: ") + w.spec->name);
    bool argsOk = w.spec->argc < 0 ? w.args.size() >= 2 : static_cast<int>(w.args.size()) == w.spec->argc;
    if (!argsOk) {
        w.problems.push_back(std::string(w.spec->name) + ": " + std::to_string(w.args.size()) + " arguments, the game takes " +
                             std::to_string(w.spec->argc));
        return w.ret;
    }
    auto h = w.handlers.find(w.spec->name);
    if (h != w.handlers.end()) h->second(w);
    return w.ret;
}
int hookPeds(int* out, int capacity) {
    int n = 0;
    for (auto& [h, e] : g_world->ents)
        if (!e.vehicle && n < capacity) out[n++] = h;
    return n;
}
int hookVehicles(int* out, int capacity) {
    int n = 0;
    for (auto& [h, e] : g_world->ents)
        if (e.vehicle && n < capacity) out[n++] = h;
    return n;
}

World::World() {
    for (const Spec& s : kNativeDb) specs[s.hash] = &s;
    auto& H = handlers;
    H["PLAYER::PLAYER_ID"] = [](World& w) { w.retI(0); };
    H["PLAYER::PLAYER_PED_ID"] = [](World& w) { w.retI(w.playerPed); };
    H["PLAYER::IS_PLAYER_FREE_AIMING"] = [](World& w) { w.retI(w.freeAiming); };
    H["PLAYER::IS_PLAYER_TARGETTING_ANYTHING"] = [](World& w) { w.retI(w.targetting); };
    H["PLAYER::IS_PLAYER_CONTROL_ON"] = [](World& w) { w.retI(w.controlOn); };
    H["PLAYER::GET_MOUNT_OWNED_BY_PLAYER"] = [](World& w) { w.retI(w.ownHorse); };
    H["PLAYER::_GET_SADDLE_HORSE_FOR_PLAYER"] = [](World& w) { w.retI(w.ownHorse); };
    H["ENTITY::DOES_ENTITY_EXIST"] = [](World& w) { w.retI(w.e0() != nullptr); };
    H["ENTITY::IS_ENTITY_DEAD"] = [](World& w) { w.retI(w.e0() && w.e0()->health <= 0); };
    H["ENTITY::GET_ENTITY_COORDS"] = [](World& w) {
        if (Ent* e = w.e0()) {
            ++e->coordReads;
            w.retV(e->pos);
        }
    };
    H["ENTITY::GET_ENTITY_HEADING"] = [](World& w) { if (Ent* e = w.e0()) w.retF(e->rot.z); };
    H["ENTITY::GET_ENTITY_VELOCITY"] = [](World& w) { if (Ent* e = w.e0()) w.retV(e->vel); };
    H["ENTITY::GET_ENTITY_SPEED"] = [](World& w) { if (Ent* e = w.e0()) w.retF(length(e->vel)); };
    H["ENTITY::GET_ENTITY_HEALTH"] = [](World& w) { if (Ent* e = w.e0()) w.retI(e->health); };
    H["ENTITY::GET_ENTITY_MAX_HEALTH"] = [](World& w) { if (Ent* e = w.e0()) w.retI(e->maxHealth); };
    H["ENTITY::GET_ENTITY_MODEL"] = [](World& w) { if (Ent* e = w.e0()) w.retI(static_cast<int32_t>(e->model)); };
    H["ENTITY::SET_ENTITY_HEALTH"] = [](World& w) { if (Ent* e = w.e0()) e->health = w.i(1); };
    H["ENTITY::SET_ENTITY_MAX_HEALTH"] = [](World& w) { if (Ent* e = w.e0()) e->maxHealth = w.i(1); };
    H["ENTITY::SET_ENTITY_COORDS"] = [](World& w) { if (Ent* e = w.e0()) e->pos = w.v3(1); };
    H["ENTITY::SET_ENTITY_COORDS_NO_OFFSET"] = [](World& w) { if (Ent* e = w.e0()) e->pos = w.v3(1); };
    H["ENTITY::_SET_ENTITY_COORDS_AND_HEADING_NO_OFFSET"] = [](World& w) {
        if (Ent* e = w.e0()) {
            e->pos = w.v3(1);
            e->rot.z = w.f(4);
        }
    };
    H["ENTITY::GET_ENTITY_ROTATION"] = [](World& w) { if (Ent* e = w.e0()) w.retV(e->rot); };
    H["ENTITY::SET_ENTITY_ROTATION"] = [](World& w) { if (Ent* e = w.e0()) e->rot = w.v3(1); };
    H["ENTITY::SET_ENTITY_VELOCITY"] = [](World& w) { if (Ent* e = w.e0()) e->vel = w.v3(1); };
    H["ENTITY::SET_ENTITY_HEADING"] = [](World& w) { if (Ent* e = w.e0()) e->rot.z = w.f(1); };
    H["ENTITY::IS_ENTITY_A_MISSION_ENTITY"] = [](World& w) { w.retI(w.e0() && w.e0()->mission); };
    H["ENTITY::IS_ENTITY_A_PED"] = [](World& w) { w.retI(w.e0() && !w.e0()->vehicle); };
    H["ENTITY::_GET_ENTITY_SCRIPT"] = [](World& w) { w.retI(w.e0() && w.e0()->mission ? static_cast<int32_t>(w.e0()->script) : 0); };
    H["ENTITY::SET_ENTITY_AS_MISSION_ENTITY"] = [](World& w) { if (Ent* e = w.e0()) e->mission = true; };
    H["ENTITY::SET_ENTITY_VISIBLE"] = [](World& w) { if (Ent* e = w.e0()) e->visible = w.i(1) != 0; };
    H["PED::CREATE_PED"] = [](World& w) {
        int h = w.addPed(w.u(0), w.v3(1));
        w.ents[h].rot.z = w.f(4);
        w.retI(h);
    };
    H["PED::DELETE_PED"] = [](World& w) {
        int* h = w.p<int>(0);
        w.erase(*h);
        *h = 0;
    };
    H["VEHICLE::CREATE_VEHICLE"] = [](World& w) {
        int h = w.addVehicle(w.u(0), w.v3(1));
        w.ents[h].rot.z = w.f(4);
        w.retI(h);
    };
    H["VEHICLE::DELETE_VEHICLE"] = [](World& w) {
        int* h = w.p<int>(0);
        w.erase(*h);
        *h = 0;
    };
    H["PED::IS_PED_A_PLAYER"] = [](World& w) { w.retI(w.i(0) == w.playerPed); };
    H["PED::IS_PED_HUMAN"] = [](World& w) { w.retI(w.e0() && w.e0()->human); };
    H["PED::IS_PED_ON_MOUNT"] = [](World& w) { w.retI(w.e0() && w.e0()->mount); };
    H["PED::GET_MOUNT"] = [](World& w) { if (Ent* e = w.e0()) w.retI(e->mount); };
    H["PED::_GET_LAST_MOUNT"] = [](World& w) { if (Ent* e = w.e0()) w.retI(e->lastMount); };
    H["PED::IS_PED_IN_ANY_VEHICLE"] = [](World& w) { w.retI(w.e0() && w.e0()->inVehicle); };
    H["PED::GET_VEHICLE_PED_IS_IN"] = [](World& w) { if (Ent* e = w.e0()) w.retI(e->inVehicle); };
    H["VEHICLE::GET_PED_IN_VEHICLE_SEAT"] = [](World& w) {
        Ent* e = w.e0();
        int seat = w.i(1);
        if (e && seat >= -1 && seat <= 8) w.retI(e->seats[seat + 1]);
    };
    H["VEHICLE::_GET_PED_IN_DRAFT_HARNESS"] = [](World& w) {
        Ent* e = w.e0();
        int slot = w.i(1);
        if (e && slot >= 0 && slot < 6) w.retI(e->harness[slot]);
    };
    H["PED::_GET_VEHICLE_DRAFT_HORSE_IS_ATTACHED_TO"] = [](World& w) { if (Ent* e = w.e0()) w.retI(e->draftOf); };
    H["VEHICLE::IS_THIS_MODEL_A_TRAIN"] = [](World& w) { w.retI(w.trains.count(w.u(0)) != 0); };
    H["PED::IS_PED_SHOOTING"] = [](World& w) { w.retI(w.e0() && w.e0()->shooting); };
    H["PED::IS_PED_RAGDOLL"] = [](World& w) { w.retI(w.e0() && w.e0()->ragdoll); };
    H["PED::IS_PED_IN_COMBAT"] = [](World& w) { w.retI(w.e0() && w.e0()->combat); };
    H["PED::GET_PED_CROUCH_MOVEMENT"] = [](World& w) { w.retI(w.e0() && w.e0()->crouch); };
    H["PED::GET_CURRENT_TARGET_FOR_PED"] = [](World& w) { if (Ent* e = w.e0()) w.retI(e->target); };
    H["PED::_GET_PED_META_OUTFIT_HASH"] = [](World& w) { if (Ent* e = w.e0()) w.retI(static_cast<int32_t>(e->outfit)); };
    H["PED::GET_PED_RELATIONSHIP_GROUP_HASH"] = [](World& w) { w.retI(static_cast<int32_t>(joaat("PLAYER"))); };
    H["WEAPON::_GET_PED_CURRENT_HELD_WEAPON"] = [](World& w) { if (Ent* e = w.e0()) w.retI(static_cast<int32_t>(e->weapon)); };
    H["STREAMING::IS_MODEL_VALID"] = [](World& w) { w.retI(w.u(0) != 0); };
    H["STREAMING::HAS_MODEL_LOADED"] = [](World& w) { w.retI(1); };
    H["TASK::TASK_GO_STRAIGHT_TO_COORD"] = [](World& w) { if (Ent* e = w.e0()) ++e->tasks; };
    H["TASK::TASK_STAND_STILL"] = [](World& w) { if (Ent* e = w.e0()) ++e->tasks; };
    H["MAP::IS_WAYPOINT_ACTIVE"] = [](World& w) { w.retI(w.waypointActive); };
    H["MAP::_GET_WAYPOINT_COORDS"] = [](World& w) { w.retV(w.waypoint); };
    H["CAMERA::GET_FINAL_RENDERED_CAM_FOV"] = [](World& w) { w.retF(50.0f); };
    H["CAMERA::CREATE_CAM"] = [](World& w) { w.retI(w.nextCam++); };
    H["CAMERA::RENDER_SCRIPT_CAMS"] = [](World& w) { w.renderingScriptCams = w.i(0) != 0; };
    H["CLOCK::GET_CLOCK_HOURS"] = [](World& w) { w.retI(w.hour); };
    H["CLOCK::GET_CLOCK_MINUTES"] = [](World& w) { w.retI(w.minute); };
    H["CLOCK::GET_CLOCK_SECONDS"] = [](World& w) { w.retI(w.second); };
    H["CLOCK::SET_CLOCK_TIME"] = [](World& w) {
        w.hour = w.i(0);
        w.minute = w.i(1);
        w.second = w.i(2);
    };
    H["MISC::GET_MISSION_FLAG"] = [](World& w) { w.retI(w.missionFlag); };
    H["MISC::SET_TIME_SCALE"] = [](World& w) { w.timeScale = w.f(0); };
    H["DLC::GET_IS_LOADING_SCREEN_ACTIVE"] = [](World& w) { w.retI(w.loading); };
    H["MISC::_GET_PREV_WEATHER_TYPE_HASH_NAME"] = [](World& w) { w.retI(static_cast<int32_t>(w.weather)); };
    H["MISC::_GET_NEXT_WEATHER_TYPE_HASH_NAME"] = [](World& w) { w.retI(static_cast<int32_t>(w.nextWeather)); };
    H["MISC::SET_WEATHER_TYPE"] = [](World& w) {
        w.nextWeather = w.u(0);
        if (w.f(4) <= 0.0f) w.weather = w.nextWeather;
        ++w.weatherSets;
    };
    H["MISC::VAR_STRING"] = [](World& w) {
        const char* label = w.p<const char>(1);
        if (w.i(0) & 1) w.problems.push_back("VAR_STRING: the first bit of the flags must not be set");
        if (!label || std::strcmp(label, "LITERAL_STRING") != 0 || w.args.size() != 3) {
            w.problems.push_back("VAR_STRING used without LITERAL_STRING + text");
            return;
        }
        w.strings.emplace_back(w.p<const char>(2));
        w.retP(w.strings.back().c_str());
    };
    H["UIDEBUG::_BG_DISPLAY_TEXT"] = [](World& w) {
        const char* text = w.p<const char>(0);
        bool fromVarString = false;
        for (const std::string& s : w.strings) fromVarString = fromVarString || s.c_str() == text;
        if (!fromVarString) w.problems.push_back("_BG_DISPLAY_TEXT needs a VAR_STRING");
        else w.displayed.emplace_back(text);
    };
    H["MAP::BLIP_ADD_FOR_ENTITY"] = [](World& w) {
        w.blips.insert(w.nextBlip);
        w.retI(w.nextBlip++);
    };
    H["MAP::BLIP_ADD_FOR_COORDS"] = [](World& w) {
        w.blips.insert(w.nextBlip);
        w.retI(w.nextBlip++);
    };
    H["MAP::REMOVE_BLIP"] = [](World& w) {
        int* b = w.p<int>(0);
        w.blips.erase(*b);
        *b = 0;
    };

    g_world = this;
    scripthook::g_api.nativeInit = hookInit;
    scripthook::g_api.nativePush64 = hookPush;
    scripthook::g_api.nativeCall = hookCall;
    scripthook::g_api.worldGetAllPeds = hookPeds;
    scripthook::g_api.worldGetAllVehicles = hookVehicles;
}
World::~World() {
    if (g_world == this) g_world = nullptr;
}

void reportProblems(const World& w) {
    std::set<std::string> unique(w.problems.begin(), w.problems.end());
    for (const std::string& p : unique) std::printf("  native problem: %s\n", p.c_str());
}

void printTop(const World& w, uint64_t frames, int n) {
    std::vector<std::pair<uint64_t, std::string>> top;
    for (const auto& [name, count] : w.byName) top.emplace_back(count, name);
    std::sort(top.rbegin(), top.rend());
    for (int k = 0; k < n && k < static_cast<int>(top.size()); ++k)
        std::printf("      %7.1f/frame  %s\n", static_cast<double>(top[k].first) / static_cast<double>(frames), top[k].second.c_str());
}

}  // namespace fakehook

namespace {

using fakehook::Ent;
using fakehook::World;

const uint32_t kHorse = joaat("a_c_horse_morgan_bay");
const uint32_t kMan = joaat("a_m_m_valfarmer_01");
const uint32_t kWagon = joaat("wagon02x");
const uint32_t kTrain = joaat("northsteamer01x");

// What the per-frame loop in plugin/main.cpp does before the session ticks.
void beginFrame(Rdr2Game& game, uint32_t now) { game.frame(now, true); }

// A busy town around the player: walking people, riders, wagons with their teams, a train,
// story NPCs, and more people out of range.
struct Town {
    int horse = 0, wagon = 0, rider = 0, mountOfRider = 0, train = 0;
    std::vector<int> mission;
    int inRangePeds = 0, inRangeVehicles = 0;
};

Town buildTown(World& w, Vec3 center, uint32_t missionScript, int people = 90) {
    Town t;
    w.animals.insert(kHorse);
    w.trains.insert(kTrain);
    w.playerPed = w.addPed(joaat("player_zero"), center);
    t.horse = w.ownHorse = w.addPed(kHorse, center + Vec3{3, 0, 0});
    w.ents[w.playerPed].lastMount = t.horse;
    std::mt19937 rng(77);
    std::uniform_real_distribution<float> spot(-110.0f, 110.0f);
    for (int k = 0; k < people; ++k) {
        int h = w.addPed(kMan, center + Vec3{spot(rng), spot(rng), 0});
        Ent& e = w.ents[h];
        e.outfit = 1000u + static_cast<uint32_t>(k % 7);
        e.weapon = joaat("WEAPON_UNARMED");
        if (k % 3 == 0) {
            e.moving = true;
            e.orbit = e.pos;
        }
        ++t.inRangePeds;
    }
    for (int k = 0; k < 6; ++k) {  // riders
        int horse = w.addPed(kHorse, center + Vec3{spot(rng), spot(rng), 0});
        int man = w.addPed(kMan, w.ents[horse].pos);
        w.ride(man, horse);
        w.ents[horse].moving = true;
        w.ents[horse].orbit = w.ents[horse].pos;
        t.inRangePeds += 2;
        t.rider = man;
        t.mountOfRider = horse;
    }
    for (int k = 0; k < 6; ++k) {  // wagons with a driver and a team of two
        int wagon = w.addVehicle(kWagon, center + Vec3{spot(rng), spot(rng), 0});
        int driver = w.addPed(kMan, w.ents[wagon].pos);
        w.seat(driver, wagon, -1);
        for (int slot = 0; slot < 2; ++slot) {
            int team = w.addPed(kHorse, w.ents[wagon].pos);
            w.ents[team].draftOf = wagon;
            w.ents[wagon].harness[slot] = team;
        }
        t.inRangePeds += 1;
        ++t.inRangeVehicles;
        t.wagon = wagon;
    }
    t.train = w.addVehicle(kTrain, center + Vec3{40, 40, 0});
    for (int k = 0; k < 8; ++k) {  // story NPCs
        int h = w.addPed(kMan, center + Vec3{10.0f + 3.0f * static_cast<float>(k), -20, 0});
        w.ents[h].mission = true;
        w.ents[h].script = missionScript;
        w.ents[h].combat = k % 2 == 0;
        w.ents[h].weapon = joaat("WEAPON_REVOLVER_CATTLEMAN");
        t.mission.push_back(h);
        ++t.inRangePeds;
    }
    for (int k = 0; k < 40; ++k)  // far away: in the ped pool, never streamed
        w.addPed(kMan, center + Vec3{400.0f + static_cast<float>(k), 300, 0});
    return t;
}

// A player whose game is the real plugin code on the simulated Script Hook.
struct HookedPlayer {
    World world;
    Rdr2Game game;
    Link link;
    MemorySaveStore saves;
    std::unique_ptr<CoopSession> session;

    void connect(Cluster& c, uint32_t ip, const char* name, bool wantsWorld) {
        c.startServer();
        LinkConfig config;
        config.remote = kServerAddr;
        config.local.role = Role::Guest;
        config.local.name = name;
        config.local.avatar = "player_zero";
        config.local.wantsWorld = wantsWorld ? 1 : 0;
        link.start(c.net.open(ip, 40000), config, c.now);
        session = std::make_unique<CoopSession>(game, link, c.config);
        session->setSaveStore(&saves);
    }
};

struct FrameCost {
    uint64_t frames = 0, calls = 0, worst = 0;
    double perFrame() const { return frames ? static_cast<double>(calls) / static_cast<double>(frames) : 0.0; }
};

// Cluster::run with the hooked player in the loop. Native calls are counted while `cost` is set.
void runHooked(Cluster& c, HookedPlayer& h, uint32_t ms, FrameCost* cost = nullptr) {
    for (uint32_t end = c.now + ms; c.now < end; c.now += 16) {
        c.net.now = c.now;
        if (c.server) c.server->pump(c.now);
        uint64_t before = h.world.calls;
        beginFrame(h.game, c.now);
        h.session->tick(c.now);
        if (cost) {
            ++cost->frames;
            cost->calls += h.world.calls - before;
            cost->worst = std::max<uint64_t>(cost->worst, h.world.calls - before);
        }
        for (Player* p : c.players)
            if (p->session) p->session->tick(c.now);
        for (Player* p : c.players) p->game.simulate(0.016f);
        h.world.simulate(0.016f);
    }
}

}  // namespace

// ---------------------------------------------------------------- measurements

COOP_TEST(testPluginOwnerFrameCost, "plugin: world owner's native calls per frame (busy town, two viewers)") {
    Cluster c;
    HookedPlayer owner;
    Town town = buildTown(owner.world, {0, 0, 0}, joaat("mission_script"));
    owner.connect(c, 0x0A0000F1, "Arthur", true);
    runHooked(c, owner, 300);
    Player& guest = c.join("John");
    Player& third = c.join("Sadie");
    guest.game.player.pos = {25, 10, 0};
    third.game.player.pos = {-30, 5, 0};
    runHooked(c, owner, 3000);
    owner.world.byName.clear();
    FrameCost cost;
    runHooked(c, owner, 5000, &cost);
    std::printf("    world owner: %.1f native calls per frame on average, %llu in the worst frame (%llu frames)\n",
                cost.perFrame(), static_cast<unsigned long long>(cost.worst), static_cast<unsigned long long>(cost.frames));
    fakehook::printTop(owner.world, cost.frames, 6);
    fakehook::reportProblems(owner.world);
    CHECK(owner.world.problems.empty());
    CHECK(owner.session->status().worldOwner);
    // Everything in range reaches the others, within the budget; nothing of the player's own.
    int peds = guest.game.count(SpawnRole::Mirror, EntityKind::Ped);
    int vehicles = guest.game.count(SpawnRole::Mirror, EntityKind::Vehicle);
    std::printf("    guest shows %d peds, %d vehicles (in range: %d, %d)\n", peds, vehicles, town.inRangePeds,
                town.inRangeVehicles);
    CHECK(peds == std::min(town.inRangePeds, c.config.maxPeds));
    CHECK(vehicles == std::min(town.inRangeVehicles, c.config.maxVehicles));
    CHECK(!mirrorOf(guest.game, kTrain, EntityKind::Vehicle));
    CHECK(guest.game.count(SpawnRole::Mirror, EntityKind::Ped) == third.game.count(SpawnRole::Mirror, EntityKind::Ped));
}

COOP_TEST(testPluginGuestFrameCost, "plugin: guest's native calls per frame (mirrors + own population removed)") {
    Cluster c;
    Player& owner = c.join("Arthur", true);
    c.run(300);
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> spot(-80.0f, 80.0f);
    for (int k = 0; k < 40; ++k) owner.game.addPed(kMan, {spot(rng), spot(rng), 0});
    HookedPlayer guest;
    Town town = buildTown(guest.world, {5, 5, 0}, joaat("mission_script"), 60);
    const int firstCreated = guest.world.next;
    guest.connect(c, 0x0A0000F2, "John", false);
    runHooked(c, guest, 4000);
    guest.world.byName.clear();
    FrameCost cost;
    runHooked(c, guest, 5000, &cost);
    std::printf("    guest: %.1f native calls per frame on average, %llu in the worst frame (%llu frames)\n",
                cost.perFrame(), static_cast<unsigned long long>(cost.worst), static_cast<unsigned long long>(cost.frames));
    fakehook::printTop(guest.world, cost.frames, 6);
    fakehook::reportProblems(guest.world);
    CHECK(guest.world.problems.empty());
    // The local ambient population is gone, story NPCs and the player's own horse stay, the
    // owner's people are shown.
    int ambient = 0, created = 0;
    for (auto& [h, e] : guest.world.ents) {
        if (h >= firstCreated && !e.vehicle) ++created;
        else if (!e.vehicle && !e.mission && h != guest.world.playerPed && h != town.horse) ++ambient;
    }
    std::printf("    guest world: %d own ambient peds left, %d mirrors created\n", ambient, created);
    CHECK(ambient == 0);
    CHECK(created >= 40);
    CHECK(guest.world.get(town.horse) != nullptr);
    for (int h : town.mission) CHECK(guest.world.get(h) != nullptr);
}

// ---------------------------------------------------------------- the native layer

namespace {

// A world with just the player standing at the origin, framed once.
struct Solo {
    World world;
    Rdr2Game game;
    uint32_t now = 1000;
    Solo() {
        world.animals.insert(kHorse);
        world.trains.insert(kTrain);
        world.playerPed = world.addPed(joaat("player_zero"), {});
        step();
    }
    void step(uint32_t ms = 16) {
        now += ms;
        world.simulate(0.016f);
        beginFrame(game, now);
    }
    uint64_t callsOf(const std::function<void()>& f) {
        uint64_t before = world.calls;
        f();
        return world.calls - before;
    }
    bool listed(int h, float radius = 150.0f) {
        std::vector<NearbyEntity> out;
        game.listNearby({}, {}, radius, out);
        return std::any_of(out.begin(), out.end(), [&](const NearbyEntity& e) { return e.handle == h; });
    }
};

}  // namespace

COOP_TEST(testPluginEveryNativeMatchesTheDatabase, "plugin: every native call matches the native database") {
    // Drive every IGame entry point (and the HUD), so each native the plugin uses is called at
    // least once and checked: known hash, right number of arguments, not a dead stub.
    Solo s;
    World& w = s.world;
    int horse = w.addPed(kHorse, {3, 0, 0});
    int wagon = w.addVehicle(kWagon, {10, 0, 0});
    int npc = w.addPed(kMan, {5, 5, 0});
    w.ents[npc].weapon = joaat("WEAPON_REVOLVER_CATTLEMAN");
    w.ents[npc].combat = true;
    w.ents[npc].target = w.playerPed;
    w.ents[npc].mission = true;
    w.ents[npc].script = 42;
    int passenger = w.addPed(kMan, {10, 0, 0});
    w.seat(passenger, wagon, 1);
    int rider = w.addPed(kMan, {3, 0, 0});
    w.ride(rider, horse);
    w.missionFlag = true;
    w.ownHorse = horse;
    s.step();
    Rdr2Game& g = s.game;
    CHECK(g.ready());
    (void)g.readLocalPlayer();
    w.ents[w.playerPed].shooting = true;
    s.step();
    (void)g.readLocalPlayer();
    w.ents[w.playerPed].shooting = false;
    w.seat(w.playerPed, wagon, -1);
    s.step();
    (void)g.readLocalPlayer();
    w.ents[wagon].seats[0] = 0;
    w.ents[w.playerPed].inVehicle = 0;
    w.ride(w.playerPed, horse);
    s.step();
    (void)g.readLocalPlayer();
    w.ents[w.playerPed].mount = 0;
    s.step();
    g.damageLocalPlayer(5);
    g.teleportLocalPlayer({1, 1, 0});
    (void)g.localOutfit();
    g.setLocalProtected(true);
    g.setLocalProtected(false);
    g.killLocalPlayer();
    w.ents[w.playerPed].health = 150;
    g.holdWorld(true);
    s.step();
    g.holdWorld(false);
    std::vector<MissionEntity> mission;
    g.listMissionEntities(mission);
    CHECK(mission.size() == 1 && mission[0].handle == npc && mission[0].script == 42);
    CHECK(g.localMissionScript() == 42);
    g.setHarmless(npc, true);
    g.revive(npc, 100);
    CameraState cam;
    CHECK(g.readCamera(cam));
    g.spectate(true, cam);
    s.step();
    g.spectate(false, cam);
    Vec3 wp;
    (void)g.readWaypoint(wp);
    g.showFriendWaypoint(true, {50, 50, 0});
    g.showFriendWaypoint(true, {60, 50, 0});
    g.showFriendWaypoint(false, {});
    std::vector<NearbyEntity> nearby;
    g.listNearby({}, {}, 100, nearby);
    EntityRead read;
    g.readEntity(npc, EntityKind::Ped, read);
    CHECK(read.state.script == 42 && (read.state.flags & NF_InCombat) && (read.state.flags & NF_Aiming));
    EntityRead seated;
    g.readEntity(passenger, EntityKind::Ped, seated);
    CHECK(seated.vehicle == wagon && seated.state.seat == 1);
    EntityRead riding;
    g.readEntity(rider, EntityKind::Ped, riding);
    CHECK(riding.mount == horse);
    g.readEntity(wagon, EntityKind::Vehicle, read);
    std::vector<ForeignEntity> foreign;
    g.listForeign(foreign);
    WorldInfo info = g.readWorldInfo();
    info.weather = joaat("RAIN");
    info.hour = static_cast<uint8_t>((info.hour + 6) % 24);
    g.applyWorldInfo(info, true, true);
    w.waypointActive = true;
    CHECK(g.readWaypoint(wp));
    EntityHandle ped = g.spawn(EntityKind::Ped, kMan, {7, 7, 0}, 90, SpawnRole::PlayerPuppet);
    EntityHandle veh = g.spawn(EntityKind::Vehicle, kWagon, {9, 9, 0}, 0, SpawnRole::Mirror);
    CHECK(ped && veh);
    g.prefetchModel(kHorse);
    g.park(ped);
    g.unpark(ped, {7, 8, 0}, 10);
    g.park(veh);
    g.unpark(veh, {9, 8, 0}, 10);
    CHECK(g.exists(ped) && !g.isDead(ped));
    (void)g.position(ped);
    (void)g.heading(ped);
    (void)g.health(ped);
    g.setHealth(ped, 120, 200);
    g.teleport(ped, {8, 8, 0}, 0);
    g.nudge(ped, {0.1f, 0, 0});
    g.moveTo(ped, {12, 8, 0}, 2.0f, 0);
    g.standStill(ped, 30);
    g.face(ped, 40);
    g.aimAt(ped, {0, 20, 0});
    g.shootAt(ped, {0, 20, 0});
    g.setWeapon(ped, joaat("WEAPON_REVOLVER_CATTLEMAN"));
    g.setWeapon(ped, 0);
    g.setMount(ped, horse);
    g.setMount(ped, 0);
    g.setVehicle(ped, veh, 0);
    g.setVehicle(ped, 0, -1);
    g.setCrouch(ped, true);
    g.ragdoll(ped);
    g.setLabel(ped, "John");
    g.setOutfit(ped, 1234);
    g.steerVehicle(veh, {1, 0, 0}, 90, 0, 0, 0.016f);
    g.damageNpc(npc, 10, ped);
    (void)g.wasDamagedByLocalPlayer(npc);
    g.suppressAmbientPopulation();
    int ambient = w.addPed(kMan, {30, 30, 0});
    s.step();
    CHECK(g.removeForeign(ambient) == ForeignAction::Deleted);
    CHECK(!w.get(ambient));
    CHECK(g.removeForeign(npc) == ForeignAction::Hidden);
    CHECK(!w.ents[npc].visible);
    g.restoreForeign(npc);
    CHECK(w.ents[npc].visible);
    g.kill(ped);
    g.remove(ped);
    g.remove(veh);
    CHECK(!w.get(ped) && !w.get(veh));
    hud::drawText("Co-op: offline", 0.01f, 0.01f, 0.3f, 200, 200, 200);

    fakehook::reportProblems(w);
    CHECK(w.problems.empty());
    // Every native in natives.h was reached.
    int unused = 0;
    for (const fakehook::Spec& spec : fakehook::kNativeDb)
        if (!spec.dead && !w.count(spec.name)) {
            std::printf("  never called: %s\n", spec.name);
            ++unused;
        }
    CHECK(unused == 0);
}

COOP_TEST(testPluginHudText, "plugin: HUD text is drawn with working natives and made safe") {
    Solo s;
    hud::drawText("Co-op \"blackwater\": 3 players", 0.01f, 0.01f, 0.3f, 140, 230, 140);
    CHECK(s.world.problems.empty());
    CHECK(s.world.count("UIDEBUG::_BG_DISPLAY_TEXT") == 1 && s.world.count("UIDEBUG::_BG_SET_TEXT_SCALE") == 1);
    CHECK(s.world.count("HUD::_DISPLAY_TEXT") == 0);  // empty since game build 1436
    CHECK(s.world.displayed.size() == 1 && s.world.displayed[0] == "Co-op \"blackwater\": 3 players");

    // A name from the network cannot inject formatting codes or break the line.
    CHECK(hud::printable("~COLOR_RED~Bill~n~") == "-COLOR_RED-Bill-n-");
    CHECK(hud::printable("a\nb\tc") == "a b c");
    CHECK(hud::printable("\xFF\xC3") == "??");                          // broken UTF-8
    CHECK(hud::printable("\xC3\xA9t\xC3\xA9") == "\xC3\xA9t\xC3\xA9");  // "ete" with accents stays
    CHECK(hud::printable("\xE0\x80\xAF") == "???");                     // overlong encoding
    // Long text is cut on a character boundary, with an ellipsis, within the limit.
    std::string cyrillic;
    for (int i = 0; i < 40; ++i) cyrillic += "\xD0\x96";  // two bytes each
    std::string cut = hud::printable(cyrillic, 25);
    CHECK(cut.size() <= 25 && cut.size() >= 23);
    CHECK(cut.compare(cut.size() - 3, 3, "...") == 0);
    std::string body = cut.substr(0, cut.size() - 3);
    CHECK(body.size() % 2 == 0 && hud::printable(body) == body);  // whole characters only
    CHECK(hud::printable(std::string(500, 'x')).size() == hud::kMaxLineBytes);
    CHECK(hud::printable("short", 25) == "short");
}

COOP_TEST(testPluginSettings, "plugin: settings - bad values, keys, names") {
    using settings::parseKey;
    CHECK(parseKey("F7") == 0x76 && parseKey("f1") == 0x70 && parseKey("F24") == 0x87);
    CHECK(parseKey("F0") == 0 && parseKey("F25") == 0 && parseKey("F7x") == 0 && parseKey("") == 0);
    CHECK(parseKey("F") == 'F');  // the letter
    CHECK(parseKey("k") == 'K' && parseKey("5") == '5' && parseKey(" F9 ") == 0x78);
    CHECK(parseKey("NUMPAD0") == 0x60 && parseKey("numpad9") == 0x69 && parseKey("NUMPAD") == 0 && parseKey("NUMPADX") == 0);
    CHECK(parseKey("Insert") == 0x2D && parseKey("Ctrl") == 0);

    auto load = [](const std::string& text) {
        IniFile ini;
        ini.parse(text);
        settings::Settings s;
        s.apply(ini);
        return s;
    };
    settings::Settings ok = load("[network]\nserver = 1.2.3.4\n[player]\nname = Arthur\n");
    CHECK(ok.problem.empty() && ok.warnings.empty());
    CHECK(ok.port == 27960 && ok.name == "Arthur" && ok.avatar == "player_three" && ok.toggleKey.vk == 0x76);
    CHECK(ok.uploadKey.vk == 0x7A && ok.uploadKey.name == "F11");

    CHECK(!load("[network]\nserver = x\nport = 27960x\n").problem.empty());
    CHECK(!load("[network]\nserver = x\nport = 70000\n").problem.empty());
    CHECK(load("[network]\nserver = x\nport = 70000\n").port == 27960);
    CHECK(!load("[network]\nport = 27960\n").problem.empty());  // nowhere to connect
    CHECK(!load("[network]\nhost_server = 1\n[server]\nmode = coop\n").problem.empty());
    CHECK(load("[network]\nserver = x\n[server]\nmode = coop\n").problem.empty());  // not hosting: unused
    CHECK(load("[network]\nhost_server = 1\n[server]\nmode = HOST\n").server.storyMode == StoryMode::HostWorld);

    settings::Settings odd = load(
        "[network]\nserver = x\n[sync]\nradius = nan\nmax_peds = lots\n[server]\nregroup_distance = inf\n"
        "[keys]\ntoggle = F99\nhud = F7\n");
    CHECK(odd.problem.empty());
    CHECK(odd.session.npcRadius == 150.0f && odd.session.maxPeds == 64 && odd.server.regroupDistance == 150.0f);
    CHECK(odd.toggleKey.vk == 0x76 && odd.toggleKey.name == "F7");  // unknown key: back to the default
    CHECK(odd.hudKey.vk == 0);                                       // the same key twice: the second is off
    CHECK(odd.warnings.size() == 5);
    CHECK(load("[network]\nserver = x\n[sync]\nradius = 5000\n").session.npcRadius == 400.0f);

    // Names: one line, no formatting codes, whole characters within the server's 24 bytes.
    std::string longName;
    for (int i = 0; i < 20; ++i) longName += "\xD0\x96";
    settings::Settings named = load("[network]\nserver = x\n[player]\nname = ~r~" + longName + "\n");
    CHECK(named.name.size() <= settings::kMaxNameBytes);
    CHECK(named.name.find('~') == std::string::npos);
    CHECK(hud::printable(named.name, 64) == named.name);  // valid UTF-8
    CHECK(load("[network]\nserver = x\n[player]\nname = ===\n").name == "Player");
    settings::Settings avatar = load("[network]\nserver = x\nworld_owner = 1\n[player]\navatar = ../../x\n");
    CHECK(avatar.avatar == "player_zero" && avatar.warnings.size() == 1);
}

COOP_TEST(testPluginSaveFiles, "plugin: save copies are atomic, never overwrite, wait for complete saves") {
    namespace fs = std::filesystem;
    const fs::path root = "coop-test-saves";
    std::error_code ec;
    fs::remove_all(root, ec);
    auto put = [](const fs::path& file, size_t size, uint8_t fill, bool old) {
        { std::ofstream(file, std::ios::binary) << std::string(size, static_cast<char>(fill)); }
        if (old) fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::hours(1));
    };
    auto read = [](const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    std::vector<uint8_t> data(3000, 7), got;
    std::string placed, error, name;

    saves::SaveFolder missing(root / "nope");
    CHECK(!missing.writeCopy(data, placed, error) && !error.empty());
    CHECK(missing.latestStamp() == 0 && !missing.readLatest(got, name));

    // No save yet: the copy goes to the folder that looks like a profile, not to a stray folder.
    fs::create_directories(root / "1A2B3C4D");
    fs::create_directories(root / "backup");  // newer, but not a profile
    { std::ofstream(root / "readme.txt") << "not a folder"; }
    saves::SaveFolder folder(root);
    CHECK(folder.writeCopy(data, placed, error) && placed == "SRDR30000");
    CHECK(read(root / "1A2B3C4D" / "SRDR30000") == data);
    CHECK(!fs::exists(root / "1A2B3C4D" / "RDR2Coop-incoming.tmp"));
    // Our own copy counts as complete at once (so it is not taken for a new save of the player).
    CHECK(folder.latestStamp() != 0);

    // Existing saves are never overwritten: the next free slot is used.
    put(root / "1A2B3C4D" / "SRDR30001", 100, 1, true);
    put(root / "1A2B3C4D" / "SRDR30003", 100, 3, true);
    CHECK(folder.writeCopy(data, placed, error) && placed == "SRDR30002");
    CHECK(read(root / "1A2B3C4D" / "SRDR30001") == std::vector<uint8_t>(100, 1));
    CHECK(read(root / "1A2B3C4D" / "SRDR30002") == data);
    for (int slot = 4; slot < saves::kSlots; ++slot) {
        char file[16];
        std::snprintf(file, sizeof file, "SRDR3%04d", slot);
        put(root / "1A2B3C4D" / file, 10, 9, true);
    }
    CHECK(!folder.writeCopy(data, placed, error) && error.find("15") != std::string::npos);
    CHECK(read(root / "1A2B3C4D" / "SRDR30014") == std::vector<uint8_t>(10, 9));
    CHECK(!fs::exists(root / "1A2B3C4D" / "RDR2Coop-incoming.tmp"));

    // A save the game is still writing is only reported once it stopped changing.
    for (const char* file : {"SRDR30000", "SRDR30002"})
        fs::last_write_time(root / "1A2B3C4D" / file, fs::file_time_type::clock::now() - std::chrono::hours(2));
    saves::SaveFolder watcher(root);
    uint64_t before = watcher.latestStamp();
    CHECK(before != 0);
    fs::remove(root / "1A2B3C4D" / "SRDR30014");
    put(root / "1A2B3C4D" / "SRDR30014", 500, 5, false);  // just written
    CHECK(watcher.latestStamp() == before);                // could still be half a file
    put(root / "1A2B3C4D" / "SRDR30014", 900, 5, false);  // still growing
    CHECK(watcher.latestStamp() == before);
    uint64_t complete = watcher.latestStamp();             // unchanged since the last look
    CHECK(complete != before && complete != 0);
    CHECK(watcher.readLatest(got, name) && name == "SRDR30014" && got.size() == 900);
    // A save written a while ago is complete straight away.
    fs::last_write_time(root / "1A2B3C4D" / "SRDR30014", fs::file_time_type::clock::now() - std::chrono::minutes(1));
    saves::SaveFolder later(root);
    CHECK(later.latestStamp() != 0 && later.latestStamp() != before);
    fs::remove_all(root, ec);
}

COOP_TEST(testPluginWorldScan, "plugin: offline frame, one world scan for all viewers, far entities, own horses") {
    Solo s;
    World& w = s.world;
    // Offline: only the player lookup and the safety checks.
    s.now += 16;
    uint64_t offline = s.callsOf([&] { s.game.frame(s.now, false); });
    std::printf("    offline frame: %llu native calls\n", static_cast<unsigned long long>(offline));
    CHECK(offline <= 2);

    int own = w.ownHorse = w.addPed(kHorse, {2, 0, 0});
    int wagon = w.addVehicle(kWagon, {20, 0, 0});
    int team[6];
    for (int slot = 0; slot < 6; ++slot) {  // a six-horse team: the middle and front pairs too
        team[slot] = w.addPed(kHorse, {20, 0, 0});
        w.ents[team[slot]].draftOf = wagon;
        w.ents[wagon].harness[slot] = team[slot];
    }
    int train = w.addVehicle(kTrain, {30, 0, 0});
    int walker = w.addPed(kMan, {10, 0, 0});
    s.step();
    CHECK(s.listed(walker) && s.listed(wagon));
    CHECK(!s.listed(own) && !s.listed(w.playerPed) && !s.listed(train));
    for (int h : team) CHECK(!s.listed(h));

    // The player drives that wagon: its whole team is the player's own.
    w.seat(w.playerPed, wagon, -1);
    s.step();
    for (int h : team) CHECK(s.game.belongsToLocalPlayer(h));
    CHECK(s.game.belongsToLocalPlayer(wagon) && s.game.belongsToLocalPlayer(own));
    LocalPlayer lp = s.game.readLocalPlayer();
    CHECK((lp.state.flags & PF_InVehicle) && lp.state.seat == -1 && lp.state.vehicleModel == kWagon);
    // Moving to another seat is noticed (the cached seat is confirmed every read).
    w.ents[wagon].seats[0] = 0;
    w.seat(w.playerPed, wagon, 2);
    s.step();
    CHECK(s.game.readLocalPlayer().state.seat == 2);
    w.ents[wagon].seats[3] = 0;
    w.ents[w.playerPed].inVehicle = 0;
    s.step();
    CHECK(!(s.game.readLocalPlayer().state.flags & PF_InVehicle));

    // Removing a vehicle we spawned takes its whole team with it.
    EntityHandle mine = s.game.spawn(EntityKind::Vehicle, kWagon, {40, 0, 0}, 0, SpawnRole::Mirror);
    for (int slot = 0; slot < 6; ++slot) {
        int horse = w.addPed(kHorse, {40, 0, 0});
        w.ents[horse].draftOf = mine;
        w.ents[mine].harness[slot] = horse;
    }
    size_t before = w.ents.size();
    s.game.remove(mine);
    CHECK(w.ents.size() == before - 7);

    // Several viewers in one frame: the pool and the positions are read once.
    for (int k = 0; k < 50; ++k) w.addPed(kMan, {static_cast<float>(k), 50, 0});
    s.step();
    std::vector<NearbyEntity> first, second;
    uint64_t one = s.callsOf([&] { s.game.listNearby({}, {}, 150, first); });
    uint64_t again = s.callsOf([&] { s.game.listNearby({0, 100, 0}, {0, 100, 0}, 150, second); });
    std::printf("    world scan: %llu native calls for the first viewer, %llu for the next\n",
                static_cast<unsigned long long>(one), static_cast<unsigned long long>(again));
    CHECK(again == 0);
    CHECK(first.size() == second.size() && !first.empty());

    // A far-away entity is read about once a second instead of every scan...
    int rider = w.addPed(kMan, {800, 0, 0});
    for (int frame = 0; frame < 60; ++frame) {  // 60 scans in one second
        s.step();
        (void)s.listed(rider);
    }
    std::printf("    a rider 800 m away: %d position reads in 60 scans\n", w.ents[rider].coordReads);
    CHECK(w.ents[rider].coordReads <= 2);
    // ...but one galloping in is listed as soon as it is in range, every time.
    bool missed = false, seen = false;
    for (int frame = 0; frame < 2500 && !seen; ++frame) {
        w.ents[rider].pos.x -= 20.0f * 0.016f;  // 20 m/s: a galloping horse
        s.step();
        seen = s.listed(rider, 150);
        if (length(w.ents[rider].pos) <= 150.0f && !seen) missed = true;
    }
    CHECK(seen && !missed);
    CHECK(w.problems.empty());
}

COOP_TEST(testPluginGunfireAndAim, "plugin: armed NPCs are watched for gunfire, aim point only while aiming") {
    Solo s;
    World& w = s.world;
    int farmer = w.addPed(kMan, {5, 0, 0});
    w.ents[farmer].weapon = joaat("WEAPON_UNARMED");
    int gunman = w.addPed(kMan, {6, 0, 0});
    w.ents[gunman].weapon = joaat("WEAPON_REVOLVER_CATTLEMAN");
    s.step();
    EntityRead read;
    s.game.readEntity(farmer, EntityKind::Ped, read);
    s.game.readEntity(gunman, EntityKind::Ped, read);
    w.byName.clear();
    for (int frame = 0; frame < 10; ++frame) s.step();
    CHECK(w.count("PED::IS_PED_SHOOTING") == 10 * 2);  // the player and the gunman, not the farmer
    // A single-frame shot is caught and held long enough for the next read.
    w.ents[gunman].shooting = true;
    s.step();
    w.ents[gunman].shooting = false;
    s.step(100);
    EntityRead after;
    s.game.readEntity(gunman, EntityKind::Ped, after);
    CHECK(after.state.flags & NF_Shooting);
    // The farmer draws a gun: watched from the next read on.
    w.ents[farmer].weapon = joaat("WEAPON_REPEATER_CARBINE");
    s.game.readEntity(farmer, EntityKind::Ped, read);
    w.byName.clear();
    s.step();
    CHECK(w.count("PED::IS_PED_SHOOTING") == 3);

    // Aim point: not looked up while just walking around; lock-on counts as aiming.
    w.byName.clear();
    LocalPlayer idle = s.game.readLocalPlayer();
    CHECK(!(idle.state.flags & PF_Aiming) && w.count("CAMERA::GET_GAMEPLAY_CAM_COORD") == 0);
    CHECK(distance(idle.state.aim, idle.state.pos) < 0.01f);
    w.targetting = true;
    LocalPlayer locked = s.game.readLocalPlayer();
    CHECK((locked.state.flags & PF_Aiming) && w.count("CAMERA::GET_GAMEPLAY_CAM_COORD") == 1);
    w.targetting = false;
    CHECK(w.problems.empty());
}

COOP_TEST(testPluginLeftoversAndBlips, "plugin: script restart leaves no frozen world or camera, blips do not leak") {
    {
        Solo s;
        s.game.holdWorld(true);
        CameraState cam;
        cam.fov = 50;
        s.game.spectate(true, cam);
        s.step();
        CHECK(s.world.timeScale == 0.0f && s.world.renderingScriptCams);
        // Script Hook restarts the script (save loaded): a new instance takes over the same game.
        Rdr2Game restarted;
        s.world.byName.clear();
        restarted.frame(s.now + 16, false);
        CHECK(s.world.timeScale == 1.0f && !s.world.renderingScriptCams);
        CHECK(s.world.count("CAMERA::DESTROY_CAM") == 1 && s.world.count("CAMERA::DO_SCREEN_FADE_IN") == 1);
        restarted.frame(s.now + 32, false);
        CHECK(s.world.count("CAMERA::DESTROY_CAM") == 1);  // once
    }
    Solo s;
    EntityHandle puppet = s.game.spawn(EntityKind::Ped, kMan, {3, 3, 0}, 0, SpawnRole::PlayerPuppet);
    s.game.setLabel(puppet, "John");
    s.game.setLabel(puppet, "John 2");
    CHECK(s.world.blips.size() == 1);
    s.game.remove(puppet);
    CHECK(s.world.blips.empty());
    CHECK(s.world.problems.empty());
}

COOP_TEST(testPluginWeatherFollowsOwner, "plugin: weather and clock follow the owner even when the local game drifts") {
    Solo s;
    World& w = s.world;
    WorldInfo owner{12, 0, 0, 1, joaat("RAIN")};
    s.game.applyWorldInfo(owner, true, true);
    CHECK(w.weather == joaat("RAIN") && w.weatherSets == 1);  // first sync: at once
    s.game.applyWorldInfo(owner, true, true);
    CHECK(w.weatherSets == 1);
    // The local game moves on to its own weather: brought back (blending in).
    w.weather = w.nextWeather = joaat("SUNNY");
    s.game.applyWorldInfo(owner, true, true);
    CHECK(w.weatherSets == 2 && w.nextWeather == joaat("RAIN"));
    // While that transition runs, it is not restarted every update.
    s.game.applyWorldInfo(owner, true, true);
    CHECK(w.weatherSets == 2);
    // Clock: small differences are left alone, real drift is corrected, odd values are kept in range.
    w.hour = 12;
    w.minute = 2;
    s.game.applyWorldInfo(owner, true, false);
    CHECK(w.minute == 2);
    w.hour = 15;
    s.game.applyWorldInfo(owner, true, false);
    CHECK(w.hour == 12 && w.minute == 0);
    WorldInfo odd{25, 61, 99, 1, 0};
    s.game.applyWorldInfo(odd, true, true);
    CHECK(w.hour == 1 && w.minute == 1 && w.second == 39);
    CHECK(w.problems.empty());
}

COOP_TEST(testPluginCalledHorseIsOwn, "plugin: a horse the player just called is the player's at once") {
    Solo s;
    World& w = s.world;
    for (int frame = 0; frame < 40; ++frame) s.step();  // the half-second refresh has just run
    // Whistle: the game creates the saddle horse right next to the player.
    int horse = w.addPed(kHorse, {4, 0, 0});
    w.ownHorse = horse;
    s.step();
    // A world owner must not stream it; a guest must not delete it as local population.
    CHECK(!s.listed(horse));
    std::vector<ForeignEntity> foreign;
    s.game.listForeign(foreign);
    CHECK(s.game.belongsToLocalPlayer(horse));
    // Some other new horse is not.
    int wild = w.addPed(kHorse, {6, 0, 0});
    s.step();
    CHECK(s.listed(wild) && !s.game.belongsToLocalPlayer(wild));
    CHECK(w.problems.empty());
}
