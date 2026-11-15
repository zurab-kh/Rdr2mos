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
constexpr int kMaxHandles = 1024;
const uint32_t kUnarmed = joaat("WEAPON_UNARMED");
const uint32_t kFallbackAvatar = joaat("player_three");

int g_peds[kMaxHandles];
int g_vehicles[kMaxHandles];

int collectPeds() { return std::clamp(worldGetAllPeds(g_peds, kMaxHandles), 0, kMaxHandles); }
int collectVehicles() { return std::clamp(worldGetAllVehicles(g_vehicles, kMaxHandles), 0, kMaxHandles); }

// Move blend ratio (1 walk, 2 run, 3 sprint / gallop) for a ground speed, blended in between so
// the animation speed matches the friend's real speed instead of snapping between gaits.
float moveBlend(float speed, bool quadruped) {
    const float walk = quadruped ? 2.2f : 1.6f, run = quadruped ? 8.0f : 5.0f, sprint = quadruped ? 14.0f : 7.0f;
    if (speed <= walk) return std::max(speed / walk, 0.3f);
    if (speed <= run) return 1.0f + (speed - walk) / (run - walk);
    return std::min(2.0f + (speed - run) / (sprint - run), 3.0f);
}
}  // namespace

void Rdr2Game::frame(uint32_t nowMs) {
    nowMs_ = nowMs;
    playerPed_ = n::PLAYER_PED_ID();
    playerMount_ = playerVehicle_ = 0;
    own_.clear();
    if (!playerPed_ || !n::DOES_ENTITY_EXIST(playerPed_)) {
        playerPed_ = 0;
        return;
    }
    int player = n::PLAYER_ID();
    if (n::IS_PED_ON_MOUNT(playerPed_)) playerMount_ = n::GET_MOUNT(playerPed_);
    if (n::IS_PED_IN_ANY_VEHICLE(playerPed_)) playerVehicle_ = n::GET_VEHICLE_PED_IS_IN(playerPed_);
    own_.insert(playerPed_);
    for (int horse : {playerMount_, n::GET_LAST_MOUNT(playerPed_), n::GET_MOUNT_OWNED_BY_PLAYER(player),
                      n::GET_SADDLE_HORSE_FOR_PLAYER(player)})
        if (horse) own_.insert(horse);
    if (playerVehicle_) {
        own_.insert(playerVehicle_);
        for (int harness = 0; harness < 4; ++harness)
            if (int horse = n::GET_PED_IN_DRAFT_HARNESS(playerVehicle_, harness)) own_.insert(horse);
    }

    if (n::IS_PED_SHOOTING(playerPed_)) {
        lastPlayerShotMs_ = nowMs;
        playerShotSeen_ = true;
    }
    // Gunfire lasts a single frame, so streamed NPCs are watched every frame (cheap: one call each).
    for (auto it = npcShots_.begin(); it != npcShots_.end();) {
        // Stop watching NPCs that left the stream (not read for a while) or no longer exist.
        if (nowMs - it->second.readMs > 2000 || !n::DOES_ENTITY_EXIST(it->first)) {
            it = npcShots_.erase(it);
            continue;
        }
        if (n::IS_PED_SHOOTING(it->first)) it->second.shotMs = nowMs ? nowMs : 1;
        ++it;
    }
    if (puppetPed_ && !n::DOES_ENTITY_EXIST(puppetPed_)) puppetPed_ = 0;
}

bool Rdr2Game::ready() { return playerPed_ != 0 && !n::GET_IS_LOADING_SCREEN_ACTIVE(); }

int Rdr2Game::seatOf(int ped, int vehicle) {
    for (int seat = -1; seat < 8; ++seat)
        if (n::GET_PED_IN_VEHICLE_SEAT(vehicle, seat) == ped) return seat;
    return -1;
}

LocalPlayer Rdr2Game::readLocalPlayer() {
    LocalPlayer lp;
    PlayerState& s = lp.state;
    int player = n::PLAYER_ID();
    // While riding or driving, report the motion of what carries the player.
    int body = playerVehicle_ ? playerVehicle_ : playerMount_ ? playerMount_ : playerPed_;
    s.pos = n::GET_ENTITY_COORDS(body);
    s.vel = n::GET_ENTITY_VELOCITY(body);
    s.heading = n::GET_ENTITY_HEADING(body);
    s.health = static_cast<uint16_t>(std::clamp(n::GET_ENTITY_HEALTH(playerPed_), 0, 65535));
    s.maxHealth = static_cast<uint16_t>(std::clamp(n::GET_ENTITY_MAX_HEALTH(playerPed_), 0, 65535));
    s.weapon = n::GET_PED_CURRENT_HELD_WEAPON(playerPed_);
    if (playerVehicle_) {
        s.flags |= PF_InVehicle;
        s.vehicleModel = n::GET_ENTITY_MODEL(playerVehicle_);
        s.seat = static_cast<int8_t>(seatOf(playerPed_, playerVehicle_));
        lp.vehicle = playerVehicle_;
    } else if (playerMount_) {
        s.flags |= PF_OnMount;
        s.mountModel = n::GET_ENTITY_MODEL(playerMount_);
        lp.mount = playerMount_;
    }
    bool shooting = playerShotSeen_ && nowMs_ - lastPlayerShotMs_ < kShotHoldMs;
    if (shooting) s.flags |= PF_Shooting;
    if (n::IS_PLAYER_FREE_AIMING(player)) s.flags |= PF_Aiming;
    if (n::IS_ENTITY_DEAD(playerPed_)) s.flags |= PF_Dead;
    if (n::IS_PED_RAGDOLL(playerPed_)) s.flags |= PF_Ragdoll;
    if (n::GET_PED_CROUCH_MOVEMENT(playerPed_)) s.flags |= PF_Crouching;
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
    return lp;
}

void Rdr2Game::damageLocalPlayer(int amount) {
    if (!playerPed_ || n::IS_ENTITY_DEAD(playerPed_)) return;
    int health = n::GET_ENTITY_HEALTH(playerPed_);
    n::SET_ENTITY_HEALTH(playerPed_, std::max(health - amount, 0), 0);
}

void Rdr2Game::teleportLocalPlayer(const Vec3& pos) {
    if (!playerPed_) return;
    n::SET_ENTITY_COORDS(playerVehicle_ ? playerVehicle_ : playerMount_ ? playerMount_ : playerPed_, pos);
}

bool Rdr2Game::belongsToLocalPlayer(EntityHandle h) { return own_.count(h) != 0; }

void Rdr2Game::listNearby(const Vec3& a, const Vec3& b, float radius, std::vector<NearbyEntity>& out) {
    const float r2 = radius * radius;
    auto inRange = [&](const Vec3& p) {
        Vec3 da = p - a, db = p - b;
        return da.x * da.x + da.y * da.y + da.z * da.z <= r2 || db.x * db.x + db.y * db.y + db.z * db.z <= r2;
    };
    int peds = collectPeds();
    for (int i = 0; i < peds; ++i) {
        int ped = g_peds[i];
        if (!ped || own_.count(ped) || !n::DOES_ENTITY_EXIST(ped)) continue;
        Vec3 pos = n::GET_ENTITY_COORDS(ped);  // distance first: the cheapest way to drop most peds
        if (!inRange(pos) || n::IS_PED_A_PLAYER(ped) || n::GET_VEHICLE_DRAFT_HORSE_IS_ATTACHED_TO(ped)) continue;
        out.push_back({ped, EntityKind::Ped, pos});
    }
    int vehicles = collectVehicles();
    for (int i = 0; i < vehicles; ++i) {
        int veh = g_vehicles[i];
        if (!veh || vehicles_.count(veh) || !n::DOES_ENTITY_EXIST(veh)) continue;
        Vec3 pos = n::GET_ENTITY_COORDS(veh);
        if (!inRange(pos) || n::IS_THIS_MODEL_A_TRAIN(n::GET_ENTITY_MODEL(veh))) continue;
        out.push_back({veh, EntityKind::Vehicle, pos});
    }
}

void Rdr2Game::readEntity(EntityHandle h, EntityKind kind, EntityRead& out) {
    EntityState& st = out.state;
    st.kind = kind;
    st.model = n::GET_ENTITY_MODEL(h);
    st.pos = n::GET_ENTITY_COORDS(h);
    st.vel = n::GET_ENTITY_VELOCITY(h);
    st.heading = n::GET_ENTITY_HEADING(h);
    if (kind == EntityKind::Vehicle) {
        Vec3 rot = n::GET_ENTITY_ROTATION(h);
        st.pitch = rot.x;
        st.roll = rot.y;
        return;
    }
    st.health = static_cast<int16_t>(std::clamp(n::GET_ENTITY_HEALTH(h), 0, 32767));
    if (n::IS_ENTITY_DEAD(h)) {
        st.flags |= NF_Dead;
        return;
    }
    if (n::IS_PED_IN_ANY_VEHICLE(h)) {
        out.vehicle = n::GET_VEHICLE_PED_IS_IN(h);
        st.seat = static_cast<int8_t>(seatOf(h, out.vehicle));
    } else if (n::IS_PED_ON_MOUNT(h)) {
        out.mount = n::GET_MOUNT(h);
    }
    if (n::IS_PED_RAGDOLL(h)) st.flags |= NF_Ragdoll;
    if (!n::IS_PED_HUMAN(h)) return;
    st.weapon = n::GET_PED_CURRENT_HELD_WEAPON(h);
    bool combat = n::IS_PED_IN_COMBAT(h, 0);
    if (combat) {
        st.flags |= NF_InCombat;
        out.target = n::GET_CURRENT_TARGET_FOR_PED(h);
        if (out.target && st.weapon != kUnarmed) st.flags |= NF_Aiming;
    }
    ShotWatch& watch = npcShots_[h];  // starts watching this one for gunfire every frame
    watch.readMs = nowMs_;
    if (watch.shotMs && nowMs_ - watch.shotMs < kShotHoldMs) st.flags |= NF_Shooting;
}

void Rdr2Game::listForeign(std::vector<ForeignEntity>& out) {
    int peds = collectPeds();
    for (int i = 0; i < peds; ++i)
        if (g_peds[i] && g_peds[i] != playerPed_)
            out.push_back({g_peds[i], n::GET_VEHICLE_DRAFT_HORSE_IS_ATTACHED_TO(g_peds[i])});
    int vehicles = collectVehicles();
    for (int i = 0; i < vehicles; ++i)
        if (g_vehicles[i]) out.push_back({g_vehicles[i], 0});
}

WorldInfo Rdr2Game::readWorldInfo() {
    WorldInfo info;
    info.hour = static_cast<uint8_t>(std::clamp(n::GET_CLOCK_HOURS(), 0, 23));
    info.minute = static_cast<uint8_t>(std::clamp(n::GET_CLOCK_MINUTES(), 0, 59));
    info.second = static_cast<uint8_t>(std::clamp(n::GET_CLOCK_SECONDS(), 0, 59));
    info.weather = n::GET_PREV_WEATHER_TYPE_HASH_NAME();
    return info;
}

void Rdr2Game::applyWorldInfo(const WorldInfo& info, bool clock, bool weather) {
    if (clock) {
        // Only correct real drift, so the local clock keeps ticking smoothly in between.
        int local = n::GET_CLOCK_HOURS() * 60 + n::GET_CLOCK_MINUTES();
        int remote = info.hour * 60 + info.minute;
        int diff = std::abs(local - remote);
        diff = std::min(diff, 24 * 60 - diff);
        if (diff >= 3) n::SET_CLOCK_TIME(info.hour, info.minute, info.second);
    }
    if (weather && info.weather && info.weather != lastWeatherSet_) {
        // First sync: switch at once; later changes blend in like natural weather.
        float transition = lastWeatherSet_ ? 30.0f : 0.0f;
        if (n::GET_PREV_WEATHER_TYPE_HASH_NAME() != info.weather) n::SET_WEATHER_TYPE(info.weather, transition);
        lastWeatherSet_ = info.weather;
    }
}

EntityHandle Rdr2Game::spawn(EntityKind kind, uint32_t model, const Vec3& pos, float heading, SpawnRole role) {
    if (!n::IS_MODEL_VALID(model)) {
        if (role != SpawnRole::PlayerPuppet) return 0;
        model = kFallbackAvatar;  // unknown avatar name in the friend's ini
    }
    n::REQUEST_MODEL(model);
    if (!n::HAS_MODEL_LOADED(model)) return 0;

    if (kind == EntityKind::Vehicle) {
        int veh = n::CREATE_VEHICLE(model, pos, heading);  // draft vehicles get their horses automatically
        if (!veh) return 0;
        n::SET_ENTITY_AS_MISSION_ENTITY(veh);
        n::SET_MODEL_AS_NO_LONGER_NEEDED(model);
        vehicles_.insert(veh);
        return veh;
    }

    int ped = n::CREATE_PED(model, pos, heading);
    if (!ped) return 0;
    n::SET_RANDOM_OUTFIT_VARIATION(ped);  // without a variation RDR2 peds are invisible
    n::SET_ENTITY_AS_MISSION_ENTITY(ped);
    n::SET_MODEL_AS_NO_LONGER_NEEDED(model);
    // We drive these peds ourselves: no AI reactions, and their bullets never hurt the local player.
    n::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, true);
    n::SET_PED_KEEP_TASK(ped, true);
    n::SET_PED_TO_PLAYER_WEAPON_DAMAGE_MODIFIER(ped, 0.0f);
    if (role != SpawnRole::Mirror && playerPed_) {
        // Same side as the player: allies stay friendly, enemies treat the friend as a target too.
        n::SET_PED_RELATIONSHIP_GROUP_HASH(ped, n::GET_PED_RELATIONSHIP_GROUP_HASH(playerPed_));
    }
    quadruped_[ped] = !n::IS_PED_HUMAN(ped);
    if (role == SpawnRole::PlayerPuppet) puppetPed_ = ped;
    return ped;
}

void Rdr2Game::remove(EntityHandle h) {
    auto blip = blips_.find(h);
    if (blip != blips_.end()) {
        n::REMOVE_BLIP(&blip->second);
        blips_.erase(blip);
    }
    if (h == puppetPed_) puppetPed_ = 0;
    npcShots_.erase(h);
    quadruped_.erase(h);
    bool vehicle = vehicles_.erase(h) != 0;
    if (!n::DOES_ENTITY_EXIST(h)) return;
    int handle = h;
    n::SET_ENTITY_AS_MISSION_ENTITY(handle);
    if (vehicle) {
        for (int harness = 0; harness < 4; ++harness) {  // take the draft animals with it
            int horse = n::GET_PED_IN_DRAFT_HARNESS(handle, harness);
            if (horse) {
                n::SET_ENTITY_AS_MISSION_ENTITY(horse);
                n::DELETE_PED(&horse);
            }
        }
        n::DELETE_VEHICLE(&handle);
    } else {
        n::DELETE_PED(&handle);
    }
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
    n::SET_ENTITY_COORDS_AND_HEADING_NO_OFFSET(h, pos, heading);
}

void Rdr2Game::nudge(EntityHandle h, const Vec3& delta) {
    n::SET_ENTITY_COORDS_NO_OFFSET(h, n::GET_ENTITY_COORDS(h) + delta);
}

void Rdr2Game::moveTo(EntityHandle h, const Vec3& pos, float speed, float heading) {
    auto it = quadruped_.find(h);
    bool quadruped = it != quadruped_.end() ? it->second : !n::IS_PED_HUMAN(h);
    n::TASK_GO_STRAIGHT_TO_COORD(h, pos, moveBlend(speed, quadruped), heading);
}

void Rdr2Game::standStill(EntityHandle h, float heading) {
    // No CLEAR_PED_TASKS: replacing the task lets the ped blend out of its walk instead of snapping.
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

void Rdr2Game::setVehicle(EntityHandle ped, EntityHandle vehicle, int seat) {
    if (vehicle) n::SET_PED_INTO_VEHICLE(ped, vehicle, seat);
    else n::CLEAR_PED_TASKS_IMMEDIATELY(ped);  // pops the ped out of its seat
}

void Rdr2Game::setCrouch(EntityHandle h, bool crouch) { n::SET_PED_CROUCH_MOVEMENT(h, crouch); }

void Rdr2Game::ragdoll(EntityHandle h) { n::SET_PED_TO_RAGDOLL(h, 800, 2500); }

void Rdr2Game::setLabel(EntityHandle h, const char* name) {
    static const uint32_t kFriendlyBlip = joaat("BLIP_STYLE_FRIENDLY");
    int blip = n::BLIP_ADD_FOR_ENTITY(kFriendlyBlip, h);
    if (blip) {
        n::SET_BLIP_NAME(blip, n::LITERAL_STRING(name));
        blips_[h] = blip;
    }
    n::SET_PED_PROMPT_NAME(h, n::LITERAL_STRING(name));
}

void Rdr2Game::steerVehicle(EntityHandle h, const Vec3& velocity, float heading, float, float) {
    n::SET_ENTITY_VELOCITY(h, velocity);
    // Turn gradually; pitch and roll stay with the physics so wheels and hulls follow the ground.
    float current = n::GET_ENTITY_HEADING(h);
    float delta = angleDelta(current, heading);
    if (std::fabs(delta) > 1.5f) {
        Vec3 rot = n::GET_ENTITY_ROTATION(h);
        rot.z = wrapHeading(current + delta * 0.25f);
        n::SET_ENTITY_ROTATION(h, rot);
    }
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

void Rdr2Game::hide(int h, bool hidden) {
    // Frozen first, so an entity without collision does not fall through the ground.
    if (hidden) n::FREEZE_ENTITY_POSITION(h, true);
    n::SET_ENTITY_VISIBLE(h, !hidden);
    n::SET_ENTITY_COLLISION(h, !hidden);
    n::SET_ENTITY_INVINCIBLE(h, hidden);
    if (n::IS_ENTITY_A_PED(h)) n::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(h, hidden);
    if (!hidden) n::FREEZE_ENTITY_POSITION(h, false);
}

ForeignAction Rdr2Game::removeForeign(EntityHandle h) {
    if (!n::DOES_ENTITY_EXIST(h)) return ForeignAction::Deleted;
    // Ambient population is deleted: hidden entities would still cost AI and physics time.
    // Entities owned by a story script are only hidden, so the script does not break.
    if (n::IS_ENTITY_A_MISSION_ENTITY(h)) {
        hide(h, true);
        return ForeignAction::Hidden;
    }
    int handle = h;
    n::SET_ENTITY_AS_MISSION_ENTITY(handle);
    if (n::IS_ENTITY_A_PED(handle)) n::DELETE_PED(&handle);
    else n::DELETE_VEHICLE(&handle);
    return ForeignAction::Deleted;
}

void Rdr2Game::restoreForeign(EntityHandle h) {
    if (n::DOES_ENTITY_EXIST(h)) hide(h, false);
}
