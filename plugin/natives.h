// Thin typed wrappers over the RDR2 script natives the plugin calls.
// Hashes and signatures follow the community native database (alloc8or/rdr3-nativedb-data).
#pragma once
#include <cstring>
#include "bytes.h"
#include "scripthook.h"

namespace native {

using Entity = int;
using Ped = int;
using Hash = uint32_t;

// Natives return vectors with every component padded to 8 bytes.
struct Vector3 {
    float x; uint32_t padX;
    float y; uint32_t padY;
    float z; uint32_t padZ;
    coop::Vec3 vec() const { return {x, y, z}; }
};

// Every native call goes through Script Hook, which looks the handler up by hash each time.
// Counted so the HUD can show how much work the mod does per frame.
inline uint32_t g_nativeCalls = 0;

template <typename T>
inline void push(T value) {
    static_assert(sizeof(T) <= sizeof(uint64_t), "native arguments are 64-bit slots");
    uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(T));
    nativePush64(raw);
}
template <typename R, typename... Args>
inline R invoke(uint64_t hash, Args... args) {
    ++g_nativeCalls;
    nativeInit(hash);
    (push(args), ...);
    return *reinterpret_cast<R*>(nativeCall());
}
template <typename... Args>
inline void call(uint64_t hash, Args... args) {
    ++g_nativeCalls;
    nativeInit(hash);
    (push(args), ...);
    nativeCall();
}
template <typename... Args>
inline bool test(uint64_t hash, Args... args) { return invoke<int>(hash, args...) != 0; }

// --- PLAYER
inline int PLAYER_ID() { return invoke<int>(0x217E9DC48139933D); }
inline Ped PLAYER_PED_ID() { return invoke<Ped>(0x096275889B8E0EE0); }
// Gives the player a new ped of that model (the old one is gone afterwards).
inline void SET_PLAYER_MODEL(int player, Hash model) { call(0xED40380076A31506, player, model, 0); }
inline bool IS_PLAYER_FREE_AIMING(int player) { return test(0x936F967D4BE1CE9D, player); }
// Aiming with lock-on (the default with a controller) is not free aiming.
inline bool IS_PLAYER_TARGETTING_ANYTHING(int player) { return test(0x4605C66E0F935F83, player); }
inline bool IS_PLAYER_CONTROL_ON(int player) { return test(0x7964097FCE4C244B, player); }
inline Ped GET_MOUNT_OWNED_BY_PLAYER(int player) { return invoke<Ped>(0xF49F14462F0AE27C, player); }
inline Ped GET_SADDLE_HORSE_FOR_PLAYER(int player) { return invoke<Ped>(0xB48050D326E9A2F3, player); }

// --- ENTITY
inline bool DOES_ENTITY_EXIST(Entity e) { return test(0xD42BD6EB2E0F1677, e); }
inline bool IS_ENTITY_DEAD(Entity e) { return test(0x7D5B1F88E7504BBA, e); }
inline coop::Vec3 GET_ENTITY_COORDS(Entity e) { return invoke<Vector3>(0xA86D5F069399F44D, e, 1, 1).vec(); }
inline float GET_ENTITY_HEADING(Entity e) { return invoke<float>(0xC230DD956E2F5507, e); }
inline coop::Vec3 GET_ENTITY_VELOCITY(Entity e) { return invoke<Vector3>(0x4805D2B1D8CF94A9, e, 0).vec(); }
inline int GET_ENTITY_HEALTH(Entity e) { return invoke<int>(0x82368787EA73C0F7, e); }
inline int GET_ENTITY_MAX_HEALTH(Entity e) { return invoke<int>(0x15D757606D170C3C, e, 0); }
inline Hash GET_ENTITY_MODEL(Entity e) { return invoke<Hash>(0xDA76A9F39210D365, e); }
inline void SET_ENTITY_HEALTH(Entity e, int health, Entity killer) { call(0xAC2767ED8BDFAB15, e, health, killer); }
inline void SET_ENTITY_MAX_HEALTH(Entity e, int value) { call(0x166E7CF68597D8B5, e, value); }
inline void SET_ENTITY_COORDS(Entity e, coop::Vec3 p) { call(0x06843DA7060A026B, e, p.x, p.y, p.z, 0, 0, 0, 0); }
inline void SET_ENTITY_COORDS_NO_OFFSET(Entity e, coop::Vec3 p) { call(0x239A3351AC1DA385, e, p.x, p.y, p.z, 0, 0, 0); }
inline void SET_ENTITY_COORDS_AND_HEADING_NO_OFFSET(Entity e, coop::Vec3 p, float heading) {
    call(0x0918E3565C20F03C, e, p.x, p.y, p.z, heading, 0, 0);
}
inline coop::Vec3 GET_ENTITY_ROTATION(Entity e) { return invoke<Vector3>(0xE09CAF86C32CB48F, e, 2).vec(); }
inline void SET_ENTITY_ROTATION(Entity e, coop::Vec3 r) { call(0x9CC8314DFEDE441E, e, r.x, r.y, r.z, 2, 1); }
inline void SET_ENTITY_VELOCITY(Entity e, coop::Vec3 v) { call(0x1C99BB7B6E96D16F, e, v.x, v.y, v.z); }
inline bool IS_ENTITY_A_MISSION_ENTITY(Entity e) { return test(0x138190F64DB4BBD1, e); }
inline bool IS_ENTITY_A_PED(Entity e) { return test(0xCF8176912DDA4EA5, e); }
inline Hash GET_ENTITY_SCRIPT(Entity e) {
    int args[8] = {};
    return invoke<Hash>(0x2A08A32B6D49906F, e, args);
}
inline void SET_ENTITY_AS_MISSION_ENTITY(Entity e) { call(0xDC19C288082E586E, e, 1, 1); }
inline void SET_ENTITY_AS_NO_LONGER_NEEDED(Entity* e) { call(0x4971D2F8162B9674, e); }
inline void SET_ENTITY_INVINCIBLE(Entity e, bool on) { call(0xA5C38736C426FCB8, e, on ? 1 : 0); }
inline void SET_ENTITY_VISIBLE(Entity e, bool on) { call(0x1794B4FCC84D812F, e, on ? 1 : 0); }
inline void SET_ENTITY_COLLISION(Entity e, bool on) { call(0xF66F820909453B8C, e, on ? 1 : 0, 0); }
inline void FREEZE_ENTITY_POSITION(Entity e, bool on) { call(0x7D9EFB7AD6B19754, e, on ? 1 : 0); }
inline bool HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(Entity victim, Entity attacker) {
    return test(0x7B6E7BEC1143AC86, victim, attacker, 1, 0);
}
inline void CLEAR_ENTITY_LAST_DAMAGE_ENTITY(Entity e) { call(0xBB19AC7D4DCEFD0F, e); }

// --- PED
inline Ped CREATE_PED(Hash model, coop::Vec3 p, float heading) {
    return invoke<Ped>(0xD49F9B0955C367DE, model, p.x, p.y, p.z, heading, 0, 0, 0, 0);
}
inline void DELETE_PED(Ped* ped) { call(0xCC0EF140F99365C5, ped); }
inline void RESURRECT_PED(Ped ped) { call(0x71BC8E838B9C6035, ped); }
inline void REVIVE_INJURED_PED(Ped ped) { call(0x8D8ACD8388CD99CE, ped); }
inline Entity GET_CURRENT_TARGET_FOR_PED(Ped ped) { return invoke<Entity>(0xCD66FEA29400A0B5, ped); }
inline void SET_PED_INTO_VEHICLE(Ped ped, int vehicle, int seat) { call(0xF75B0D629E1C063D, ped, vehicle, seat); }
inline int GET_VEHICLE_PED_IS_IN(Ped ped) { return invoke<int>(0x9A9112A0FE9A4713, ped, 0); }
inline void SET_PED_CROUCH_MOVEMENT(Ped ped, bool on) { call(0x7DE9692C6F64CFE8, ped, on ? 1 : 0, 0, 0); }
inline void SET_PED_TO_RAGDOLL(Ped ped, int minMs, int maxMs) { call(0xAE99FB955581844A, ped, minMs, maxMs, 0, 0, 0, static_cast<const char*>(nullptr)); }
inline int GET_VEHICLE_DRAFT_HORSE_IS_ATTACHED_TO(Ped horse) { return invoke<int>(0xE4770DA1B8FF4FD1, horse); }
inline void SET_RANDOM_OUTFIT_VARIATION(Ped ped) { call(0x283978A15512B2FE, ped, 1); }
inline Hash GET_PED_META_OUTFIT_HASH(Ped ped) { return invoke<Hash>(0x30569F348D126A5A, ped); }
inline void EQUIP_META_PED_OUTFIT(Ped ped, Hash outfit) { call(0x1902C4CFCC5BE57C, ped, outfit); }
// Outfit presets of the ped's model: how many, and wearing one (`p2` false: keep accessories as they are).
inline int GET_NUM_META_PED_OUTFITS(Ped ped) { return invoke<int>(0x10C70A515BC03707, ped); }
inline void EQUIP_META_PED_OUTFIT_PRESET(Ped ped, int preset) { call(0x77FF8D35EEC6BBC4, ped, preset, 0); }
// The shop items a ped wears (what the stores sell), one by one, and how each is worn.
inline int GET_NUM_COMPONENTS_IN_PED(Ped ped) { return invoke<int>(0x90403E8107B60E81, ped); }
inline Hash GET_SHOP_ITEM_COMPONENT_AT_INDEX(Ped ped, int index, Hash* wearableState) {
    // Script variables are 8-byte slots: the outputs get slots of their own, read afterwards.
    uint64_t status = 0, wearable = 0;
    Hash item = invoke<Hash>(0x77BA37622E22023B, ped, index, 1, &status, &wearable);
    *wearableState = static_cast<Hash>(wearable);
    return item;
}
inline void APPLY_SHOP_ITEM_TO_PED(Ped ped, Hash item) { call(0xD3A7B003ED343FD9, ped, item, 1, 0, 0); }
inline void REMOVE_SHOP_ITEM_FROM_PED(Ped ped, Hash item) { call(0x0D7FFA1B2F69ED82, ped, item, 0, 0); }
inline void UPDATE_SHOP_ITEM_WEARABLE_STATE(Ped ped, Hash item, Hash wearableState) {
    call(0x66B957AAC2EAAEAB, ped, item, wearableState, 0, 0, 1);
}
// Builds the ped's look from the components it was given (after any of the calls above).
inline void UPDATE_PED_VARIATION(Ped ped) { call(0xCC8CA3E88256E58F, ped, 0, 1, 1, 1, 0); }
inline bool IS_PED_A_PLAYER(Ped ped) { return test(0x12534C348C6CB68B, ped); }
inline bool IS_PED_HUMAN(Ped ped) { return test(0xB980061DA992779D, ped); }
inline bool IS_PED_ON_MOUNT(Ped ped) { return test(0x460BC76A0E10655E, ped); }
inline Ped GET_MOUNT(Ped ped) { return invoke<Ped>(0xE7E11B8DCBED1058, ped); }
inline Ped GET_LAST_MOUNT(Ped ped) { return invoke<Ped>(0x4C8B59171957BCF7, ped); }
inline void SET_PED_ONTO_MOUNT(Ped ped, Ped mount) { call(0x028F76B6E78246EB, ped, mount, -1, 1); }
inline void REMOVE_PED_FROM_MOUNT(Ped ped) { call(0x5337B721C51883A9, ped, 1, 1); }
inline bool IS_PED_SHOOTING(Ped ped) { return test(0x34616828CD07F1A1, ped); }
inline bool IS_PED_RAGDOLL(Ped ped) { return test(0x47E4E977581C5B55, ped); }
inline bool IS_PED_IN_ANY_VEHICLE(Ped ped) { return test(0x997ABD671D25CA0B, ped, 0); }
inline bool IS_PED_IN_COMBAT(Ped ped, Ped target) { return test(0x4859F1FC66A6278E, ped, target); }
inline bool GET_PED_CROUCH_MOVEMENT(Ped ped) { return test(0xD5FE956C70FF370B, ped); }
inline Hash GET_PED_RELATIONSHIP_GROUP_HASH(Ped ped) { return invoke<Hash>(0x7DBDD04862D95F04, ped); }
inline void SET_PED_RELATIONSHIP_GROUP_HASH(Ped ped, Hash group) { call(0xC80A74AC829DDD92, ped, group); }
inline void SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(Ped ped, bool on) { call(0x9F8AA94D6D97DBF4, ped, on ? 1 : 0); }
inline void SET_PED_KEEP_TASK(Ped ped, bool on) { call(0x971D38760FBC02EF, ped, on ? 1 : 0); }
inline void SET_PED_TO_PLAYER_WEAPON_DAMAGE_MODIFIER(Ped ped, float m) { call(0xD77AE48611B7B10A, ped, m); }
inline void SET_PED_DESIRED_HEADING(Ped ped, float heading) { call(0xAA5A7ECE2AA8FE70, ped, heading); }
inline void APPLY_DAMAGE_TO_PED(Ped ped, int amount, Ped attacker) { call(0x697157CED63F18D4, ped, amount, 0, 0, attacker); }
inline void SET_PED_PROMPT_NAME(Ped ped, const char* name) { call(0x4A48B6E03BABB4AC, ped, name); }
inline void SET_AMBIENT_PED_DENSITY_MULTIPLIER_THIS_FRAME(float m) { call(0xAB0D553FE20A6E25, m); }
inline void SET_AMBIENT_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME(float m) { call(0xBA0980B5C0A11924, m); }
inline void SET_AMBIENT_ANIMAL_DENSITY_MULTIPLIER_THIS_FRAME(float m) { call(0xC0258742B034DFAF, m); }
inline void SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME(float m) { call(0x7A556143A1C03898, m); }
inline void SET_SCENARIO_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME(float m) { call(0x28CB6391ACEDD9DB, m); }
inline void SET_SCENARIO_ANIMAL_DENSITY_MULTIPLIER_THIS_FRAME(float m) { call(0xDB48E99F8E064E56, m); }

// --- VEHICLE
inline int CREATE_VEHICLE(Hash model, coop::Vec3 p, float heading) {
    return invoke<int>(0xAF35D0D2583051B0, model, p.x, p.y, p.z, heading, 0, 0, 0, 0);
}
inline void DELETE_VEHICLE(int* vehicle) { call(0xE20A909D8C4A70F8, vehicle); }
inline Ped GET_PED_IN_VEHICLE_SEAT(int vehicle, int seat) { return invoke<Ped>(0xBB40DD2270B65366, vehicle, seat); }
inline Ped GET_PED_IN_DRAFT_HARNESS(int vehicle, int harness) { return invoke<Ped>(0xA8BA0BAE0173457B, vehicle, harness); }
inline bool IS_THIS_MODEL_A_TRAIN(Hash model) { return test(0xFC08C8F8C1EDF174, model); }
inline void SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(float m) { call(0x606374EBFC27B133, m); }
inline void SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(float m) { call(0x1F91D44490E1EA0C, m); }
inline void SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(float m) { call(0xFEDFA97638D61D4A, m); }

// --- TASK
inline void CLEAR_PED_TASKS(Ped ped) { call(0xE1EF3C1216AFF2CD, ped, 1, 1); }
inline void CLEAR_PED_TASKS_IMMEDIATELY(Ped ped) { call(0xAAA34F8A7CB32098, ped, 0, 0); }
inline void TASK_STAND_STILL(Ped ped, int timeMs) { call(0x919BE13EED931959, ped, timeMs); }
inline void TASK_GO_STRAIGHT_TO_COORD(Ped ped, coop::Vec3 p, float moveBlend, float finalHeading) {
    call(0xD76B57B44F1E6F8B, ped, p.x, p.y, p.z, moveBlend, -1, finalHeading, 0.2f, 0);
}
inline void TASK_AIM_GUN_AT_COORD(Ped ped, coop::Vec3 p, int timeMs) { call(0x6671F3EEC681BDA1, ped, p.x, p.y, p.z, timeMs, 0, 0); }
inline void TASK_SHOOT_AT_COORD(Ped ped, coop::Vec3 p, int timeMs, Hash firingPattern) {
    call(0x46A6CC01E0826106, ped, p.x, p.y, p.z, timeMs, firingPattern, 0);
}

// --- WEAPON
inline Hash GET_PED_CURRENT_HELD_WEAPON(Ped ped) { return invoke<Hash>(0x8425C5F057012DAB, ped); }
inline void GIVE_WEAPON_TO_PED(Ped ped, Hash weapon, int ammo, Hash addReason) {
    call(0x5E3BDDBCB83F3D84, ped, weapon, ammo, 1, 0, 0, 0, 0.5f, 1.0f, addReason, 0, 0.0f, 0);
}
inline void SET_CURRENT_PED_WEAPON(Ped ped, Hash weapon) { call(0xADF692B254977C0C, ped, weapon, 1, 0, 0, 0); }
inline void SET_PED_INFINITE_AMMO(Ped ped, bool on, Hash weapon) { call(0x3EDCB0505123623B, ped, on ? 1 : 0, weapon); }
inline bool GET_PED_LAST_WEAPON_IMPACT_COORD(Ped ped, Vector3* out) { return test(0x6C4D0409BA1A2BC2, ped, out); }

// --- STREAMING
inline bool IS_MODEL_VALID(Hash model) { return test(0x392C8D8E07B70EFC, model); }
inline void REQUEST_MODEL(Hash model) { call(0xFA28FE3A6246FC30, model, 0); }
inline bool HAS_MODEL_LOADED(Hash model) { return test(0x1283B8B89DD5D1B6, model); }
inline void SET_MODEL_AS_NO_LONGER_NEEDED(Hash model) { call(0x4AD96EF928BD4F9A, model); }

// --- CAMERA / CLOCK / MISC
inline coop::Vec3 GET_FINAL_RENDERED_CAM_COORD() { return invoke<Vector3>(0x5352E025EC2B416F).vec(); }
inline coop::Vec3 GET_FINAL_RENDERED_CAM_ROT() { return invoke<Vector3>(0x602685BD85DD26CA, 2).vec(); }
inline float GET_FINAL_RENDERED_CAM_FOV() { return invoke<float>(0x04AF77971E508F6A); }
inline int CREATE_CAM(const char* name) { return invoke<int>(0xE72CDBA7F0A02DD6, name, 1); }
inline void DESTROY_CAM(int cam) { call(0x4E67E0B6D7FD5145, cam, 0); }
inline void SET_CAM_ACTIVE(int cam, bool on) { call(0x87295BCA613800C8, cam, on ? 1 : 0); }
inline void RENDER_SCRIPT_CAMS(bool on, int easeMs) { call(0x33281167E4942E4F, on ? 1 : 0, easeMs > 0 ? 1 : 0, easeMs, 1, 1, 0); }
inline void SET_CAM_COORD(int cam, coop::Vec3 p) { call(0xF9EE7D419EE49DE6, cam, p.x, p.y, p.z); }
inline void SET_CAM_ROT(int cam, coop::Vec3 r) { call(0x63DFA6810AD78719, cam, r.x, r.y, r.z, 2); }
inline void SET_CAM_FOV(int cam, float fov) { call(0x27666E5988D9D429, cam, fov); }
inline void DO_SCREEN_FADE_OUT(int ms) { call(0x40C719A5E410B9E4, ms); }
inline void DO_SCREEN_FADE_IN(int ms) { call(0x6A053CF596F67DF7, ms); }
inline void SET_TIME_SCALE(float scale) { call(0x9682AF6050854856, scale); }
inline void DISABLE_ALL_CONTROL_ACTIONS(int control) { call(0x5F4B6931816E599B, control); }
inline void SET_FOCUS_POS_AND_VEL(coop::Vec3 p) { call(0x25F6EF88664540E2, p.x, p.y, p.z, 0.0f, 0.0f, 0.0f); }
inline void CLEAR_FOCUS() { call(0x86CCAF7CE493EFBE); }
inline coop::Vec3 GET_GAMEPLAY_CAM_COORD() { return invoke<Vector3>(0x595320200B98596E).vec(); }
inline coop::Vec3 GET_GAMEPLAY_CAM_ROT() { return invoke<Vector3>(0x0252D2B5582957A6, 2).vec(); }
inline int GET_CLOCK_HOURS() { return invoke<int>(0xC82CF208C2B19199); }
inline int GET_CLOCK_MINUTES() { return invoke<int>(0x4E162231B823DBBF); }
inline int GET_CLOCK_SECONDS() { return invoke<int>(0xB6101ABE62B5F080); }
inline void SET_CLOCK_TIME(int h, int m, int s) { call(0x3A52C59FFB2DEED8, h, m, s); }
inline bool GET_MISSION_FLAG() { return test(0xB15CD1CF58771DE1); }
inline bool GET_IS_LOADING_SCREEN_ACTIVE() { return test(0x71D4BF5890659B0C); }
// The weather blends from "prev" to "next"; without a transition both are the current weather.
inline uint32_t GET_PREV_WEATHER_TYPE_HASH_NAME() { return invoke<uint32_t>(0x4BEB42AEBCA732E9); }
inline uint32_t GET_NEXT_WEATHER_TYPE_HASH_NAME() { return invoke<uint32_t>(0x51021D36F62AAA83); }
inline void SET_WEATHER_TYPE(Hash weather, float transitionSeconds) {
    call(0x59174F1AFE095B5A, weather, 1, 1, transitionSeconds > 0.0f ? 1 : 0, transitionSeconds, 0);
}
// MISC::VAR_STRING(flags, "LITERAL_STRING", text): the game's way to show arbitrary text. The result
// lives in a short-lived game buffer: pass it straight to the native that uses it.
inline const char* LITERAL_STRING(const char* text) { return invoke<const char*>(0xFA925AC00EB830B9, 10, "LITERAL_STRING", text); }

// --- MAP / HUD
inline int BLIP_ADD_FOR_ENTITY(Hash style, Entity e) { return invoke<int>(0x23F74C2FDA6E7C61, style, e); }
inline void SET_BLIP_NAME(int blip, const char* varString) { call(0x9CB1A1623062F402, blip, varString); }
inline void REMOVE_BLIP(int* blip) { call(0xF2C3C9DA47AAA54A, blip); }
inline int BLIP_ADD_FOR_COORDS(Hash style, coop::Vec3 p) { return invoke<int>(0x554D9D53F696D002, style, p.x, p.y, p.z); }
inline void SET_BLIP_COORDS(int blip, coop::Vec3 p) { call(0x4FF674F5E23D49CE, blip, p.x, p.y, p.z); }
inline bool IS_WAYPOINT_ACTIVE() { return test(0x202B1BBFC6AB5EE4); }
inline coop::Vec3 GET_WAYPOINT_COORDS() { return invoke<Vector3>(0x29B30D07C3F7873B).vec(); }
// On-screen text. HUD::_SET_TEXT_COLOR / _DISPLAY_TEXT are empty since game build 1436 (and the
// text scale native has no HUD:: counterpart at all): the working ones live in UIDEBUG. They
// draw for one frame, so they are called every frame. The text must come from VAR_STRING.
inline void BG_SET_TEXT_SCALE(float x, float y) { call(0xA1253A3C870B6843, x, y); }
inline void BG_SET_TEXT_COLOR(int r, int g, int b, int a) { call(0x16FA5CE47F184F1E, r, g, b, a); }
inline void BG_DISPLAY_TEXT(const char* varString, float x, float y) { call(0x16794E044C9EFB58, varString, x, y); }

}  // namespace native
