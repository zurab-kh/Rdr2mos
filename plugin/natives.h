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

template <typename T>
inline void push(T value) {
    static_assert(sizeof(T) <= sizeof(uint64_t), "native arguments are 64-bit slots");
    uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(T));
    nativePush64(raw);
}
template <typename R, typename... Args>
inline R invoke(uint64_t hash, Args... args) {
    nativeInit(hash);
    (push(args), ...);
    return *reinterpret_cast<R*>(nativeCall());
}
template <typename... Args>
inline void call(uint64_t hash, Args... args) {
    nativeInit(hash);
    (push(args), ...);
    nativeCall();
}
template <typename... Args>
inline bool test(uint64_t hash, Args... args) { return invoke<int>(hash, args...) != 0; }

// --- PLAYER
inline int PLAYER_ID() { return invoke<int>(0x217E9DC48139933D); }
inline Ped PLAYER_PED_ID() { return invoke<Ped>(0x096275889B8E0EE0); }
inline bool IS_PLAYER_FREE_AIMING(int player) { return test(0x936F967D4BE1CE9D, player); }
inline Ped GET_MOUNT_OWNED_BY_PLAYER(int player) { return invoke<Ped>(0xF49F14462F0AE27C, player); }
inline Ped GET_SADDLE_HORSE_FOR_PLAYER(int player) { return invoke<Ped>(0xB48050D326E9A2F3, player); }

// --- ENTITY
inline bool DOES_ENTITY_EXIST(Entity e) { return test(0xD42BD6EB2E0F1677, e); }
inline bool IS_ENTITY_DEAD(Entity e) { return test(0x7D5B1F88E7504BBA, e); }
inline coop::Vec3 GET_ENTITY_COORDS(Entity e) { return invoke<Vector3>(0xA86D5F069399F44D, e, 1, 1).vec(); }
inline float GET_ENTITY_HEADING(Entity e) { return invoke<float>(0xC230DD956E2F5507, e); }
inline coop::Vec3 GET_ENTITY_VELOCITY(Entity e) { return invoke<Vector3>(0x4805D2B1D8CF94A9, e, 0).vec(); }
inline float GET_ENTITY_SPEED(Entity e) { return invoke<float>(0xFB6BA510A533DF81, e); }
inline int GET_ENTITY_HEALTH(Entity e) { return invoke<int>(0x82368787EA73C0F7, e); }
inline int GET_ENTITY_MAX_HEALTH(Entity e) { return invoke<int>(0x15D757606D170C3C, e, 0); }
inline Hash GET_ENTITY_MODEL(Entity e) { return invoke<Hash>(0xDA76A9F39210D365, e); }
inline void SET_ENTITY_HEALTH(Entity e, int health, Entity killer) { call(0xAC2767ED8BDFAB15, e, health, killer); }
inline void SET_ENTITY_MAX_HEALTH(Entity e, int value) { call(0x166E7CF68597D8B5, e, value); }
inline void SET_ENTITY_COORDS(Entity e, coop::Vec3 p) { call(0x06843DA7060A026B, e, p.x, p.y, p.z, 0, 0, 0, 0); }
inline void SET_ENTITY_COORDS_NO_OFFSET(Entity e, coop::Vec3 p) { call(0x239A3351AC1DA385, e, p.x, p.y, p.z, 0, 0, 0); }
inline void SET_ENTITY_HEADING(Entity e, float heading) { call(0xCF2B9C0645C4651B, e, heading); }
inline void SET_ENTITY_AS_MISSION_ENTITY(Entity e) { call(0xDC19C288082E586E, e, 1, 1); }
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
inline void SET_RANDOM_OUTFIT_VARIATION(Ped ped) { call(0x283978A15512B2FE, ped, 1); }
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
inline void SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(float m) { call(0x606374EBFC27B133, m); }
inline void SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(float m) { call(0x1F91D44490E1EA0C, m); }
inline void SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(float m) { call(0xFEDFA97638D61D4A, m); }

// --- TASK
inline void CLEAR_PED_TASKS(Ped ped) { call(0xE1EF3C1216AFF2CD, ped, 1, 1); }
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
inline coop::Vec3 GET_GAMEPLAY_CAM_COORD() { return invoke<Vector3>(0x595320200B98596E).vec(); }
inline coop::Vec3 GET_GAMEPLAY_CAM_ROT() { return invoke<Vector3>(0x0252D2B5582957A6, 2).vec(); }
inline int GET_CLOCK_HOURS() { return invoke<int>(0xC82CF208C2B19199); }
inline int GET_CLOCK_MINUTES() { return invoke<int>(0x4E162231B823DBBF); }
inline int GET_CLOCK_SECONDS() { return invoke<int>(0xB6101ABE62B5F080); }
inline void SET_CLOCK_TIME(int h, int m, int s) { call(0x3A52C59FFB2DEED8, h, m, s); }
inline bool GET_MISSION_FLAG() { return test(0xB15CD1CF58771DE1); }
inline bool GET_IS_LOADING_SCREEN_ACTIVE() { return test(0x71D4BF5890659B0C); }
inline const char* LITERAL_STRING(const char* text) { return invoke<const char*>(0xFA925AC00EB830B9, 10, "LITERAL_STRING", text); }

// --- MAP / HUD
inline int BLIP_ADD_FOR_ENTITY(Hash style, Entity e) { return invoke<int>(0x23F74C2FDA6E7C61, style, e); }
inline void SET_BLIP_NAME(int blip, const char* varString) { call(0x9CB1A1623062F402, blip, varString); }
inline void REMOVE_BLIP(int* blip) { call(0xF2C3C9DA47AAA54A, blip); }
inline void SET_TEXT_SCALE(float x, float y) { call(0x4170B650590B3B00, x, y); }
inline void SET_TEXT_COLOR(int r, int g, int b, int a) { call(0x50A41AD966910F03, r, g, b, a); }
inline void DISPLAY_TEXT(const char* varString, float x, float y) { call(0xD79334A4BB99BAD1, varString, x, y); }

}  // namespace native
