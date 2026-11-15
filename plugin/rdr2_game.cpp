#include "rdr2_game.h"

#include <algorithm>
#include <cmath>

#include "mathx.h"
#include "natives.h"

using namespace coop;
namespace n = native;

namespace {
constexpr uint32_t kShotHoldMs = 350;  // how long a detected gunshot keeps the "shooting" flag up
constexpr float kPi = 3.14159265f;
const uint32_t kUnarmed = joaat("WEAPON_UNARMED");
const uint32_t kFallbackAvatar = joaat("player_three");

int collectPeds(int* buffer, int capacity) {
    int count = worldGetAllPeds(buffer, capacity);
    return std::clamp(count, 0, capacity);
}
}  // namespace

void Rdr2Game::frame(uint32_t nowMs) {
    nowMs_ = nowMs;
    playerPed_ = n::PLAYER_PED_ID();
    playerMount_ = 0;
    ownPeds_.clear();
    if (!playerPed_ || !n::DOES_ENTITY_EXIST(playerPed_)) {
        playerPed_ = 0;
        return;
    }
    int player = n::PLAYER_ID();
    if (n::IS_PED_ON_MOUNT(playerPed_)) playerMount_ = n::GET_MOUNT(playerPed_);
    ownPeds_.insert(playerPed_);
    for (int horse : {playerMount_, n::GET_LAST_MOUNT(playerPed_), n::GET_MOUNT_OWNED_BY_PLAYER(player),
                      n::GET_SADDLE_HORSE_FOR_PLAYER(player)})
        if (horse) ownPeds_.insert(horse);

    if (n::IS_PED_SHOOTING(playerPed_)) {
        lastPlayerShotMs_ = nowMs;
        playerShotSeen_ = true;
    }
    for (auto it = npcShotMs_.begin(); it != npcShotMs_.end();) {
        if (!n::DOES_ENTITY_EXIST(it->first)) {
            it = npcShotMs_.erase(it);
            continue;
        }
        if (n::IS_PED_SHOOTING(it->first)) it->second = nowMs ? nowMs : 1;
        ++it;
    }
    if (puppetPed_ && !n::DOES_ENTITY_EXIST(puppetPed_)) puppetPed_ = 0;
}

bool Rdr2Game::ready() { return playerPed_ != 0 && !n::GET_IS_LOADING_SCREEN_ACTIVE(); }

PlayerState Rdr2Game::readLocalPlayer() {
    PlayerState s;
    int player = n::PLAYER_ID();
    int body = playerMount_ ? playerMount_ : playerPed_;  // while riding, report the horse's motion
    s.pos = n::GET_ENTITY_COORDS(body);
    s.vel = n::GET_ENTITY_VELOCITY(body);
    s.heading = n::GET_ENTITY_HEADING(body);
    s.health = static_cast<uint16_t>(std::clamp(n::GET_ENTITY_HEALTH(playerPed_), 0, 65535));
    s.maxHealth = static_cast<uint16_t>(std::clamp(n::GET_ENTITY_MAX_HEALTH(playerPed_), 0, 65535));
    s.weapon = n::GET_PED_CURRENT_HELD_WEAPON(playerPed_);
    if (playerMount_) {
        s.flags |= PF_OnMount;
        s.mountModel = n::GET_ENTITY_MODEL(playerMount_);
    }
    bool shooting = playerShotSeen_ && nowMs_ - lastPlayerShotMs_ < kShotHoldMs;
    if (shooting) s.flags |= PF_Shooting;
    if (n::IS_PLAYER_FREE_AIMING(player)) s.flags |= PF_Aiming;
    if (n::IS_ENTITY_DEAD(playerPed_)) s.flags |= PF_Dead;
    if (n::IS_PED_RAGDOLL(playerPed_)) s.flags |= PF_Ragdoll;
    if (n::GET_PED_CROUCH_MOVEMENT(playerPed_)) s.flags |= PF_Crouching;
    if (n::IS_PED_IN_ANY_VEHICLE(playerPed_)) s.flags |= PF_InVehicle;
    if (n::GET_MISSION_FLAG()) s.flags |= PF_InMission;

    // Aim point: where the last bullet landed, otherwise a point far along the camera direction.
    n::Vector3 impact{};
    if (shooting && n::GET_PED_LAST_WEAPON_IMPACT_COORD(playerPed_, &impact)) {
        s.aim = impact.vec();
    } else {
        Vec3 cam = n::GET_GAMEPLAY_CAM_COORD();
        Vec3 rot = n::GET_GAMEPLAY_CAM_ROT();
        float pitch = rot.x * kPi / 180.0f, yaw = rot.z * kPi / 180.0f;
        Vec3 dir{-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)};
        s.aim = cam + dir * 60.0f;
    }
    return s;
}

void Rdr2Game::damageLocalPlayer(int amount) {
    if (!playerPed_ || n::IS_ENTITY_DEAD(playerPed_)) return;
    int health = n::GET_ENTITY_HEALTH(playerPed_);
    n::SET_ENTITY_HEALTH(playerPed_, std::max(health - amount, 0), 0);
}

void Rdr2Game::teleportLocalPlayer(const Vec3& pos) {
    if (!playerPed_) return;
    n::SET_ENTITY_COORDS(playerMount_ ? playerMount_ : playerPed_, pos);
}

bool Rdr2Game::belongsToLocalPlayer(EntityHandle h) { return ownPeds_.count(h) != 0; }

void Rdr2Game::listNpcs(const Vec3& a, const Vec3& b, float radius, const std::vector<EntityHandle>& exclude,
                        std::vector<LocalNpc>& out) {
    static int handles[1024];
    int count = collectPeds(handles, 1024);
    std::map<int, uint32_t> shots;
    for (int i = 0; i < count; ++i) {
        int ped = handles[i];
        if (!ped || ownPeds_.count(ped) || !n::DOES_ENTITY_EXIST(ped) || n::IS_PED_A_PLAYER(ped)) continue;
        if (std::find(exclude.begin(), exclude.end(), ped) != exclude.end()) continue;
        Vec3 pos = n::GET_ENTITY_COORDS(ped);
        if (distance(pos, a) > radius && distance(pos, b) > radius) continue;

        LocalNpc npc;
        npc.handle = ped;
        npc.state.model = n::GET_ENTITY_MODEL(ped);
        npc.state.pos = pos;
        npc.state.heading = n::GET_ENTITY_HEADING(ped);
        npc.state.speed = n::GET_ENTITY_SPEED(ped);
        npc.state.health = static_cast<int16_t>(std::clamp(n::GET_ENTITY_HEALTH(ped), 0, 32767));
        bool dead = n::IS_ENTITY_DEAD(ped);
        if (dead) {
            npc.state.flags |= NF_Dead;
        } else {
            if (n::IS_PED_ON_MOUNT(ped)) npc.mount = n::GET_MOUNT(ped);
            if (n::IS_PED_RAGDOLL(ped)) npc.state.flags |= NF_Ragdoll;
            if (n::IS_PED_HUMAN(ped)) {
                npc.state.weapon = n::GET_PED_CURRENT_HELD_WEAPON(ped);
                npc.fightsPeer = puppetPed_ && n::IS_PED_IN_COMBAT(ped, puppetPed_);
                if (npc.fightsPeer || n::IS_PED_IN_COMBAT(ped, playerPed_)) npc.state.flags |= NF_InCombat;
                auto shot = npcShotMs_.find(ped);
                uint32_t shotMs = shot != npcShotMs_.end() ? shot->second : 0;
                if (shotMs && nowMs_ - shotMs < kShotHoldMs) npc.state.flags |= NF_Shooting;
                shots[ped] = shotMs;  // keep watching this one for gunfire every frame
            }
        }
        out.push_back(npc);
    }
    npcShotMs_ = std::move(shots);
}

void Rdr2Game::listAllPeds(std::vector<EntityHandle>& out) {
    static int handles[1024];
    int count = collectPeds(handles, 1024);
    for (int i = 0; i < count; ++i)
        if (handles[i] && handles[i] != playerPed_) out.push_back(handles[i]);
}

WorldInfo Rdr2Game::readWorldInfo() {
    WorldInfo info;
    info.hour = static_cast<uint8_t>(std::clamp(n::GET_CLOCK_HOURS(), 0, 23));
    info.minute = static_cast<uint8_t>(std::clamp(n::GET_CLOCK_MINUTES(), 0, 59));
    info.second = static_cast<uint8_t>(std::clamp(n::GET_CLOCK_SECONDS(), 0, 59));
    return info;
}

void Rdr2Game::applyWorldInfo(const WorldInfo& info) {
    // Only correct real drift, so the local clock keeps ticking smoothly in between.
    int local = n::GET_CLOCK_HOURS() * 60 + n::GET_CLOCK_MINUTES();
    int remote = info.hour * 60 + info.minute;
    int diff = std::abs(local - remote);
    diff = std::min(diff, 24 * 60 - diff);
    if (diff >= 3) n::SET_CLOCK_TIME(info.hour, info.minute, info.second);
}

EntityHandle Rdr2Game::spawnPed(uint32_t model, const Vec3& pos, float heading, PedKind kind) {
    if (!n::IS_MODEL_VALID(model)) {
        if (kind != PedKind::PlayerPuppet) return 0;
        model = kFallbackAvatar;  // unknown avatar name in the friend's ini
    }
    n::REQUEST_MODEL(model);
    if (!n::HAS_MODEL_LOADED(model)) return 0;
    int ped = n::CREATE_PED(model, pos, heading);
    if (!ped) return 0;
    n::SET_RANDOM_OUTFIT_VARIATION(ped);  // without a variation RDR2 peds are invisible
    n::SET_ENTITY_AS_MISSION_ENTITY(ped);
    n::SET_MODEL_AS_NO_LONGER_NEEDED(model);
    // We drive these peds ourselves: no AI reactions, and their bullets never hurt the local player.
    n::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, true);
    n::SET_PED_KEEP_TASK(ped, true);
    n::SET_PED_TO_PLAYER_WEAPON_DAMAGE_MODIFIER(ped, 0.0f);
    if (kind != PedKind::NpcMirror && playerPed_) {
        // Same side as the player: allies stay friendly, enemies treat the friend as a target too.
        n::SET_PED_RELATIONSHIP_GROUP_HASH(ped, n::GET_PED_RELATIONSHIP_GROUP_HASH(playerPed_));
    }
    if (kind == PedKind::PlayerPuppet) puppetPed_ = ped;
    return ped;
}

void Rdr2Game::deletePed(EntityHandle h) {
    auto blip = blips_.find(h);
    if (blip != blips_.end()) {
        n::REMOVE_BLIP(&blip->second);
        blips_.erase(blip);
    }
    if (h == puppetPed_) puppetPed_ = 0;
    npcShotMs_.erase(h);
    if (!n::DOES_ENTITY_EXIST(h)) return;
    int ped = h;
    n::SET_ENTITY_AS_MISSION_ENTITY(ped);
    n::DELETE_PED(&ped);
}

bool Rdr2Game::exists(EntityHandle h) { return h != 0 && n::DOES_ENTITY_EXIST(h); }
bool Rdr2Game::isDead(EntityHandle h) { return n::IS_ENTITY_DEAD(h); }
Vec3 Rdr2Game::position(EntityHandle h) { return n::GET_ENTITY_COORDS(h); }
float Rdr2Game::heading(EntityHandle h) { return n::GET_ENTITY_HEADING(h); }
int Rdr2Game::health(EntityHandle h) { return n::IS_ENTITY_DEAD(h) ? 0 : n::GET_ENTITY_HEALTH(h); }

void Rdr2Game::setHealth(EntityHandle h, int health, int maxHealth) {
    if (maxHealth > 0 && n::GET_ENTITY_MAX_HEALTH(h) != maxHealth) n::SET_ENTITY_MAX_HEALTH(h, maxHealth);
    n::SET_ENTITY_HEALTH(h, health, 0);
}

void Rdr2Game::kill(EntityHandle h) { n::SET_ENTITY_HEALTH(h, 0, 0); }

void Rdr2Game::teleport(EntityHandle h, const Vec3& pos, float heading) {
    n::SET_ENTITY_COORDS_NO_OFFSET(h, pos);
    n::SET_ENTITY_HEADING(h, heading);
}

void Rdr2Game::moveTo(EntityHandle h, const Vec3& pos, float speed, float heading) {
    float blend = speed < 2.5f ? 1.0f : speed < 6.0f ? 2.0f : 3.0f;  // walk / run / sprint
    n::TASK_GO_STRAIGHT_TO_COORD(h, pos, blend, heading);
}

void Rdr2Game::standStill(EntityHandle h, float heading) {
    n::CLEAR_PED_TASKS(h);
    n::SET_PED_DESIRED_HEADING(h, heading);
    n::TASK_STAND_STILL(h, -1);
}

void Rdr2Game::aimAt(EntityHandle h, const Vec3& target) { n::TASK_AIM_GUN_AT_COORD(h, target, 1500); }

void Rdr2Game::shootAt(EntityHandle h, const Vec3& target) {
    static const uint32_t kFullAuto = joaat("FIRING_PATTERN_FULL_AUTO");
    n::TASK_SHOOT_AT_COORD(h, target, 800, kFullAuto);
}

void Rdr2Game::setWeapon(EntityHandle h, uint32_t weapon) {
    if (weapon == 0 || weapon == kUnarmed) {
        n::SET_CURRENT_PED_WEAPON(h, kUnarmed);
        return;
    }
    static const uint32_t kAddReasonDefault = joaat("ADD_REASON_DEFAULT");
    n::GIVE_WEAPON_TO_PED(h, weapon, 200, kAddReasonDefault);
    n::SET_CURRENT_PED_WEAPON(h, weapon);
    n::SET_PED_INFINITE_AMMO(h, true, weapon);
}

void Rdr2Game::setMount(EntityHandle rider, EntityHandle mount) {
    if (mount) n::SET_PED_ONTO_MOUNT(rider, mount);
    else n::REMOVE_PED_FROM_MOUNT(rider);
}

void Rdr2Game::setLabel(EntityHandle h, const char* name) {
    static const uint32_t kFriendlyBlip = joaat("BLIP_STYLE_FRIENDLY");
    int blip = n::BLIP_ADD_FOR_ENTITY(kFriendlyBlip, h);
    if (blip) {
        n::SET_BLIP_NAME(blip, n::LITERAL_STRING(name));
        blips_[h] = blip;
    }
    n::SET_PED_PROMPT_NAME(h, n::LITERAL_STRING(name));
}

void Rdr2Game::damageNpc(EntityHandle npc, int amount, EntityHandle attacker) {
    if (n::IS_ENTITY_DEAD(npc)) return;
    int before = n::GET_ENTITY_HEALTH(npc);
    if (attacker && !n::DOES_ENTITY_EXIST(attacker)) attacker = 0;
    n::APPLY_DAMAGE_TO_PED(npc, amount, attacker);  // lets the game react: flinch, turn hostile, credit the kill
    // Whatever modifiers the game applied, the friend's hit must cost exactly what they saw.
    int wanted = std::max(before - amount, 0);
    if (!n::IS_ENTITY_DEAD(npc) && n::GET_ENTITY_HEALTH(npc) > wanted) n::SET_ENTITY_HEALTH(npc, wanted, attacker);
}

bool Rdr2Game::wasDamagedByLocalPlayer(EntityHandle h) {
    bool hit = playerPed_ && n::HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(h, playerPed_);
    n::CLEAR_ENTITY_LAST_DAMAGE_ENTITY(h);
    return hit;
}

void Rdr2Game::suppressAmbientPopulation() {
    n::SET_AMBIENT_PED_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
    n::SET_AMBIENT_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
    n::SET_AMBIENT_ANIMAL_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
    n::SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
    n::SET_SCENARIO_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
    n::SET_SCENARIO_ANIMAL_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
    n::SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
    n::SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
    n::SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
}

void Rdr2Game::setHidden(EntityHandle h, bool hidden) {
    if (!n::DOES_ENTITY_EXIST(h)) return;
    // Frozen first, so a ped without collision does not fall through the ground.
    if (hidden) n::FREEZE_ENTITY_POSITION(h, true);
    n::SET_ENTITY_VISIBLE(h, !hidden);
    n::SET_ENTITY_COLLISION(h, !hidden);
    n::SET_ENTITY_INVINCIBLE(h, hidden);
    n::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(h, hidden);
    if (!hidden) n::FREEZE_ENTITY_POSITION(h, false);
}
