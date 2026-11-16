#include "rdr2_game.h"

#include <algorithm>
#include <cmath>

#include "hud.h"
#include "mathx.h"
#include "natives.h"

using namespace coop;
namespace n = native;

namespace {
constexpr uint32_t kShotHoldMs = 350;    // how long a detected gunshot keeps the "shooting" flag up
constexpr uint32_t kShotWatchMs = 2000;  // an NPC the session stopped reading is no longer watched
constexpr float kPi = 3.14159265f;
constexpr int kMaxHandles = 1024;
constexpr int kDraftHarnesses = 6;       // eDraftHarness: LR, RR, LM, RM, LF, RF
// How long cached facts about a handle are trusted. A handle carries a generation counter, so the
// game does not hand the same number to a new entity within these times.
constexpr uint32_t kKindMs = 5000;       // model, human, player, train: fixed for an entity
constexpr uint32_t kOwnerMs = 1000;      // mission entity / owning script / draft attachment
constexpr uint32_t kOutfitMs = 1000;
constexpr uint32_t kAliveMs = 250;       // our own (persistent) entities are not re-checked every frame
constexpr uint32_t kOwnHorsesMs = 500;
constexpr uint32_t kMaxHealthMs = 1000;
constexpr uint32_t kInfoKeepMs = 10000;  // forget handles not asked about for this long
constexpr size_t kMaxAdopted = 8;         // adopted mirrors kept as the player's (oldest dropped)
// World scan: an entity far out of range cannot be in range before it could have covered the gap
// (nothing but trains moves faster than this), so its position is not re-read every scan.
constexpr float kMaxScanSpeed = 30.0f;   // m/s
constexpr uint32_t kFarRecheckMs = 1000;
const uint32_t kUnarmed = joaat("WEAPON_UNARMED");
const uint32_t kFallbackAvatar = joaat("player_three");

// Survive the script restart after a save load, so nothing we switched on is left behind.
bool g_protectionLeftOn = false;
bool g_holdLeftOn = false;
int g_scriptCamera = 0;

bool due(uint32_t nowMs, uint32_t dueMs) { return static_cast<int32_t>(nowMs - dueMs) >= 0; }
bool armed(uint32_t weapon) { return weapon != 0 && weapon != kUnarmed; }

// Move blend ratio (1 walk, 2 run, 3 sprint / gallop) for a ground speed, blended in between so
// the animation speed matches the friend's real speed instead of snapping between gaits.
float moveBlend(float speed, bool quadruped) {
    const float walk = quadruped ? 2.2f : 1.6f, run = quadruped ? 8.0f : 5.0f, sprint = quadruped ? 14.0f : 7.0f;
    if (speed <= walk) return std::max(speed / walk, 0.3f);
    if (speed <= run) return 1.0f + (speed - walk) / (run - walk);
    return std::min(2.0f + (speed - run) / (sprint - run), 3.0f);
}
}  // namespace

// ---------------------------------------------------------------- caches

uint32_t Rdr2Game::spreadDue(int h, uint32_t intervalMs) const {
    // Between half and the full interval, fixed per handle: refreshes of a crowd drift apart.
    uint32_t mix = static_cast<uint32_t>(h) * 2654435761u;
    return nowMs_ + intervalMs - (mix >> 8) % (intervalMs / 2);
}

Rdr2Game::Info& Rdr2Game::kindInfo(int h, bool vehicle) {
    Info& in = info_[h];
    in.usedMs = nowMs_;
    if (!in.haveKind || due(nowMs_, in.kindDueMs)) {
        bool newcomer = !in.haveKind;
        in.model = n::GET_ENTITY_MODEL(h);
        if (vehicle) {
            in.train = n::IS_THIS_MODEL_A_TRAIN(in.model);
        } else {
            in.player = n::IS_PED_A_PLAYER(h);
            in.human = n::IS_PED_HUMAN(h);
        }
        in.haveKind = true;
        in.kindDueMs = spreadDue(h, kKindMs);
        // A horse that just appeared may be the player's (called with a whistle): find out now,
        // before a scan takes it for a world entity (or a guest deletes it as foreign).
        if (newcomer && !vehicle && !in.human && playerPed_ && !ownHorsesFresh_) refreshOwnHorses(true);
    }
    return in;
}

Rdr2Game::Info& Rdr2Game::ownerInfo(int h, bool vehicle) {
    Info& in = kindInfo(h, vehicle);
    if (!in.haveOwner || due(nowMs_, in.ownerDueMs)) {
        in.mission = n::IS_ENTITY_A_MISSION_ENTITY(h);
        in.script = in.mission ? n::GET_ENTITY_SCRIPT(h) : 0;
        in.draftOf = vehicle || in.human ? 0 : n::GET_VEHICLE_DRAFT_HORSE_IS_ATTACHED_TO(h);  // only animals pull
        in.haveOwner = true;
        in.ownerDueMs = spreadDue(h, kOwnerMs);
    }
    return in;
}

uint32_t Rdr2Game::outfitOf(int ped) {
    Info& in = info_[ped];
    in.usedMs = nowMs_;
    if (!in.haveOutfit || due(nowMs_, in.outfitDueMs)) {
        in.outfit = n::GET_PED_META_OUTFIT_HASH(ped);
        in.haveOutfit = true;
        in.outfitDueMs = spreadDue(ped, kOutfitMs);
    }
    return in.outfit;
}

void Rdr2Game::forget(int h) {
    cache_.erase(h);
    info_.erase(h);
    npcShots_.erase(h);
}

const std::vector<int>& Rdr2Game::peds() {
    if (!pedsFresh_) {
        peds_.resize(kMaxHandles);
        peds_.resize(static_cast<size_t>(std::clamp(worldGetAllPeds(peds_.data(), kMaxHandles), 0, kMaxHandles)));
        pedsFresh_ = true;
    }
    return peds_;
}

const std::vector<int>& Rdr2Game::vehicles() {
    if (!vehiclesFresh_) {
        vehiclesList_.resize(kMaxHandles);
        vehiclesList_.resize(
            static_cast<size_t>(std::clamp(worldGetAllVehicles(vehiclesList_.data(), kMaxHandles), 0, kMaxHandles)));
        vehiclesFresh_ = true;
    }
    return vehiclesList_;
}

bool Rdr2Game::missionFlag() {
    if (missionFlag_ < 0) missionFlag_ = n::GET_MISSION_FLAG() ? 1 : 0;
    return missionFlag_ != 0;
}

// ---------------------------------------------------------------- frame

void Rdr2Game::frame(uint32_t nowMs, bool active) {
    nowMs_ = nowMs;
    cache_.clear();
    poolsChanged();
    missionFlag_ = -1;
    loading_ = -1;
    ownHorsesFresh_ = false;
    if (nowMs - lastPruneMs_ >= 2000) {
        lastPruneMs_ = nowMs;
        for (auto it = info_.begin(); it != info_.end();)
            it = nowMs - it->second.usedMs > kInfoKeepMs ? info_.erase(it) : std::next(it);
    }
    playerPed_ = n::PLAYER_PED_ID();
    playerMount_ = playerVehicle_ = 0;
    own_.clear();
    if (!playerPed_ || !n::DOES_ENTITY_EXIST(playerPed_)) {
        playerPed_ = 0;
        return;
    }
    Cached& me = cached(playerPed_);
    me.exists = true;
    me.have |= kHaveExists;

    if (g_protectionLeftOn && !protected_) {
        n::SET_ENTITY_INVINCIBLE(playerPed_, false);
        g_protectionLeftOn = false;
    }
    if (g_holdLeftOn && !held_) {  // the script restarted while the world was frozen
        n::SET_TIME_SCALE(1.0f);
        n::DO_SCREEN_FADE_IN(500);
        g_holdLeftOn = false;
    }
    if (g_scriptCamera && !camera_) {  // ... or while the spectator camera was on
        n::RENDER_SCRIPT_CAMS(false, 0);
        n::DESTROY_CAM(g_scriptCamera);
        n::CLEAR_FOCUS();
        g_scriptCamera = 0;
    }
    if (held_) n::SET_TIME_SCALE(0.0f);
    if (held_ || camera_) n::DISABLE_ALL_CONTROL_ACTIONS(0);
    if (!active) return;

    player_ = n::PLAYER_ID();
    if (n::IS_PED_ON_MOUNT(playerPed_)) playerMount_ = n::GET_MOUNT(playerPed_);
    if (n::IS_PED_IN_ANY_VEHICLE(playerPed_)) playerVehicle_ = n::GET_VEHICLE_PED_IS_IN(playerPed_);
    own_.insert(playerPed_);
    if (playerMount_) own_.insert(playerMount_);
    if (playerVehicle_) own_.insert(playerVehicle_);
    own_.insert(ownHorses_.begin(), ownHorses_.end());
    refreshOwnHorses(false);

    if (n::IS_PED_SHOOTING(playerPed_)) {
        lastPlayerShotMs_ = nowMs;
        playerShotSeen_ = true;
    }
    // Gunfire lasts a single frame, so armed streamed NPCs are watched every frame (one call each).
    // A handle that vanished just answers "no"; it drops out once the session stops reading it.
    for (auto it = npcShots_.begin(); it != npcShots_.end();) {
        if (nowMs - it->second.readMs > kShotWatchMs) {
            it = npcShots_.erase(it);
            continue;
        }
        if (n::IS_PED_SHOOTING(it->first)) it->second.shotMs = nowMs ? nowMs : 1;
        ++it;
    }
}

// The player's horses and the team of their vehicle change rarely: refreshed twice a second, or
// at once when the player mounts, dismounts or changes vehicle.
void Rdr2Game::refreshOwnHorses(bool force) {
    bool changed = playerPed_ != ownHorsesPed_ || playerMount_ != ownHorsesMount_ || playerVehicle_ != ownHorsesVehicle_;
    if (!force && !changed && !due(nowMs_, ownHorsesDueMs_)) return;
    ownHorsesFresh_ = true;
    for (int horse : ownHorses_) own_.erase(horse);
    ownHorses_.clear();
    for (int horse : {n::GET_LAST_MOUNT(playerPed_), n::GET_MOUNT_OWNED_BY_PLAYER(player_),
                      n::GET_SADDLE_HORSE_FOR_PLAYER(player_)})
        if (horse) ownHorses_.push_back(horse);
    if (playerVehicle_)
        for (int harness = 0; harness < kDraftHarnesses; ++harness)
            if (int horse = n::GET_PED_IN_DRAFT_HARNESS(playerVehicle_, harness)) ownHorses_.push_back(horse);
    // Adopted mirrors stay the player's until the game cleans them up (they are no longer needed,
    // so it will once nobody is near). Their draft teams follow through draftOf.
    adopted_.erase(std::remove_if(adopted_.begin(), adopted_.end(), [](int h) { return !n::DOES_ENTITY_EXIST(h); }),
                   adopted_.end());
    ownHorses_.insert(ownHorses_.end(), adopted_.begin(), adopted_.end());
    ownHorsesPed_ = playerPed_;
    ownHorsesMount_ = playerMount_;
    ownHorsesVehicle_ = playerVehicle_;
    ownHorsesDueMs_ = nowMs_ + kOwnHorsesMs;
    own_.insert(ownHorses_.begin(), ownHorses_.end());
    own_.insert(playerPed_);
    if (playerMount_) own_.insert(playerMount_);
    if (playerVehicle_) own_.insert(playerVehicle_);
}

bool Rdr2Game::ready() {
    if (!playerPed_) return false;
    if (loading_ < 0) loading_ = n::GET_IS_LOADING_SCREEN_ACTIVE() ? 1 : 0;
    return loading_ == 0;
}

// The seat a ped sits in: the last one found is confirmed with one call, the full search only
// runs when the ped moved.
int Rdr2Game::seatOf(int ped, int vehicle) {
    Info& in = info_[ped];
    in.usedMs = nowMs_;
    if (in.seatVehicle == vehicle && n::GET_PED_IN_VEHICLE_SEAT(vehicle, in.seat) == ped) return in.seat;
    in.seatVehicle = vehicle;
    in.seat = -1;
    for (int seat = -1; seat < 8; ++seat)
        if (n::GET_PED_IN_VEHICLE_SEAT(vehicle, seat) == ped) {
            in.seat = seat;
            break;
        }
    return in.seat;
}

LocalPlayer Rdr2Game::readLocalPlayer() {
    LocalPlayer lp;
    PlayerState& s = lp.state;
    // While riding or driving, report the motion of what carries the player.
    int body = playerVehicle_ ? playerVehicle_ : playerMount_ ? playerMount_ : playerPed_;
    s.pos = position(body);
    s.vel = n::GET_ENTITY_VELOCITY(body);
    s.heading = heading(body);
    s.health = static_cast<uint16_t>(std::clamp(n::GET_ENTITY_HEALTH(playerPed_), 0, 65535));
    if (!haveMaxHealth_ || due(nowMs_, playerMaxHealthDueMs_)) {
        playerMaxHealth_ = n::GET_ENTITY_MAX_HEALTH(playerPed_);
        playerMaxHealthDueMs_ = nowMs_ + kMaxHealthMs;
        haveMaxHealth_ = true;
    }
    s.maxHealth = static_cast<uint16_t>(std::clamp(playerMaxHealth_, 0, 65535));
    s.weapon = n::GET_PED_CURRENT_HELD_WEAPON(playerPed_);
    if (playerVehicle_) {
        s.flags |= PF_InVehicle;
        s.vehicleModel = kindInfo(playerVehicle_, true).model;
        s.seat = static_cast<int8_t>(seatOf(playerPed_, playerVehicle_));
        lp.vehicle = playerVehicle_;
    } else if (playerMount_) {
        s.flags |= PF_OnMount;
        s.mountModel = kindInfo(playerMount_, false).model;
        lp.mount = playerMount_;
    }
    bool shooting = playerShotSeen_ && nowMs_ - lastPlayerShotMs_ < kShotHoldMs;
    bool aiming = n::IS_PLAYER_FREE_AIMING(player_) || n::IS_PLAYER_TARGETTING_ANYTHING(player_);
    if (shooting) s.flags |= PF_Shooting;
    if (aiming) s.flags |= PF_Aiming;
    if (isDead(playerPed_)) s.flags |= PF_Dead;
    if (n::IS_PED_RAGDOLL(playerPed_)) s.flags |= PF_Ragdoll;
    if (n::GET_PED_CROUCH_MOVEMENT(playerPed_)) s.flags |= PF_Crouching;
    if (missionFlag()) {
        s.flags |= PF_InMission;
        // RDR2 plays its story scenes as scripted scenes during which the player has no control.
        if (!n::IS_PLAYER_CONTROL_ON(player_)) s.flags |= PF_Cutscene;
    }

    // Aim point (only used while aiming or shooting): where the last bullet landed, otherwise a
    // point far along the camera direction.
    n::Vector3 impact{};
    if (shooting && n::GET_PED_LAST_WEAPON_IMPACT_COORD(playerPed_, &impact)) {
        s.aim = impact.vec();
    } else if (shooting || aiming) {
        Vec3 cam = n::GET_GAMEPLAY_CAM_COORD();
        Vec3 rot = n::GET_GAMEPLAY_CAM_ROT();
        float pitch = rot.x * kPi / 180.0f, yaw = rot.z * kPi / 180.0f;
        Vec3 dir{-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)};
        s.aim = cam + dir * 60.0f;
    } else {
        s.aim = s.pos;
    }
    return lp;
}

void Rdr2Game::damageLocalPlayer(int amount) {
    if (!playerPed_ || n::IS_ENTITY_DEAD(playerPed_)) return;
    int health = n::GET_ENTITY_HEALTH(playerPed_);
    n::SET_ENTITY_HEALTH(playerPed_, std::max(health - amount, 0), 0);
    invalidate(playerPed_);
}

void Rdr2Game::teleportLocalPlayer(const Vec3& pos) {
    if (!playerPed_) return;
    int body = playerVehicle_ ? playerVehicle_ : playerMount_ ? playerMount_ : playerPed_;
    n::SET_ENTITY_COORDS(body, pos);
    invalidate(body);
}

bool Rdr2Game::belongsToLocalPlayer(EntityHandle h) { return own_.count(h) != 0; }

uint32_t Rdr2Game::localOutfit() { return playerPed_ ? n::GET_PED_META_OUTFIT_HASH(playerPed_) : 0; }

// ---------------------------------------------------------------- characters

uint32_t Rdr2Game::localModel() { return playerPed_ ? n::GET_ENTITY_MODEL(playerPed_) : 0; }

bool Rdr2Game::modelExists(uint32_t model) { return model && n::IS_MODEL_VALID(model); }

bool Rdr2Game::setLocalModel(uint32_t model) {
    if (!playerPed_ || !modelExists(model)) return false;
    n::REQUEST_MODEL(model);
    if (!n::HAS_MODEL_LOADED(model)) return false;  // streaming in: the session asks again
    int old = playerPed_;
    // The new ped starts out fresh: it keeps the wounds of the old one (no free healing), and the
    // protection of a running scene.
    int health = n::GET_ENTITY_HEALTH(old);
    n::SET_PLAYER_MODEL(player_, model);
    n::SET_MODEL_AS_NO_LONGER_NEEDED(model);
    // A new ped for the player: nothing known about the old one applies to it.
    forget(old);
    invalidate(old);
    poolsChanged();
    playerPed_ = n::PLAYER_PED_ID();
    if (playerPed_) {
        forget(playerPed_);
        Cached& me = cached(playerPed_);
        me.exists = true;
        me.have |= kHaveExists;
        own_.insert(playerPed_);
        if (health > 0) n::SET_ENTITY_HEALTH(playerPed_, std::min(health, n::GET_ENTITY_MAX_HEALTH(playerPed_)), 0);
        if (protected_) n::SET_ENTITY_INVINCIBLE(playerPed_, true);
    }
    ownHorsesFresh_ = false;
    playerMaxHealth_ = 0;
    return playerPed_ != 0;
}

bool Rdr2Game::readAppearance(EntityHandle ped, coop::Appearance& out) {
    out = coop::Appearance{};
    if (!ped || !exists(ped)) return false;
    out.model = n::GET_ENTITY_MODEL(ped);
    out.outfit = n::GET_PED_META_OUTFIT_HASH(ped);
    int count = std::clamp(n::GET_NUM_COMPONENTS_IN_PED(ped), 0, static_cast<int>(coop::kMaxShopItems));
    for (int i = 0; i < count; ++i) {
        uint32_t wearable = 0;
        uint32_t item = n::GET_SHOP_ITEM_COMPONENT_AT_INDEX(ped, i, &wearable);
        if (item) out.items.push_back({item, wearable});
    }
    return true;
}

void Rdr2Game::applyAppearance(EntityHandle ped, const coop::Appearance& look) {
    if (!ped || !exists(ped)) return;
    // The base first (an outfit preset, or the outfit it was dressed in), then every shop item on
    // top, worn the same way: hats, coats, rolled-up sleeves.
    if (look.preset != coop::Appearance::kNoPreset) n::EQUIP_META_PED_OUTFIT_PRESET(ped, look.preset);
    else if (look.items.empty() && look.outfit) n::EQUIP_META_PED_OUTFIT(ped, look.outfit);
    // What it wears now that the look does not (a hat taken off), or wears differently (sleeves
    // rolled down again: the default state comes back with the item put on afresh), comes off.
    coop::Appearance worn;
    readAppearance(ped, worn);
    for (const coop::ShopItem& have : worn.items) {
        auto want = std::find_if(look.items.begin(), look.items.end(),
                                 [&](const coop::ShopItem& i) { return i.component == have.component; });
        if (want == look.items.end() || (want->wearable == 0 && have.wearable != 0))
            n::REMOVE_SHOP_ITEM_FROM_PED(ped, have.component);
    }
    for (const coop::ShopItem& item : look.items) {
        n::APPLY_SHOP_ITEM_TO_PED(ped, item.component);
        if (item.wearable) n::UPDATE_SHOP_ITEM_WEARABLE_STATE(ped, item.component, item.wearable);
    }
    n::UPDATE_PED_VARIATION(ped);
}

void Rdr2Game::blockInput() { n::DISABLE_ALL_CONTROL_ACTIONS(0); }

int Rdr2Game::outfitPresets(EntityHandle ped) { return ped && exists(ped) ? n::GET_NUM_META_PED_OUTFITS(ped) : 0; }

void Rdr2Game::setOutfitPreset(EntityHandle ped, int preset) {
    if (!ped || !exists(ped) || preset < 0) return;
    n::EQUIP_META_PED_OUTFIT_PRESET(ped, preset);
    n::UPDATE_PED_VARIATION(ped);
}

void Rdr2Game::setLocalProtected(bool on) {
    protected_ = on;
    g_protectionLeftOn = on;
    if (playerPed_) n::SET_ENTITY_INVINCIBLE(playerPed_, on);
}

void Rdr2Game::killLocalPlayer() {
    if (playerPed_ && !n::IS_ENTITY_DEAD(playerPed_)) {
        n::SET_ENTITY_INVINCIBLE(playerPed_, false);
        n::SET_ENTITY_HEALTH(playerPed_, 0, 0);
        invalidate(playerPed_);
    }
}

void Rdr2Game::holdWorld(bool on) {
    if (on == held_) return;
    held_ = on;
    g_holdLeftOn = on;
    n::SET_TIME_SCALE(on ? 0.0f : 1.0f);
    if (on) n::DO_SCREEN_FADE_OUT(800);
    else n::DO_SCREEN_FADE_IN(800);
}

// ---------------------------------------------------------------- story

void Rdr2Game::listMissionEntities(std::vector<MissionEntity>& out) {
    // The whole pool is scanned four times a second; who owns an entity is cached per handle, so
    // a scan mostly costs one position per story entity.
    for (int ped : peds()) {
        if (!ped || own_.count(ped) || isSpawned(ped)) continue;
        const Info& in = ownerInfo(ped, false);
        if (in.player || !in.mission || !in.script || own_.count(ped)) continue;
        out.push_back({ped, EntityKind::Ped, in.model, in.script, position(ped)});
    }
    for (int veh : vehicles()) {
        if (!veh || own_.count(veh) || isSpawned(veh)) continue;
        const Info& in = ownerInfo(veh, true);
        if (!in.mission || !in.script) continue;
        out.push_back({veh, EntityKind::Vehicle, in.model, in.script, position(veh)});
    }
    missionCache_ = out;
    missionCacheMs_ = nowMs_;
    missionCacheValid_ = true;
}

uint32_t Rdr2Game::localMissionScript() {
    if (!playerPed_ || !missionFlag()) return 0;
    if (!missionCacheValid_ || nowMs_ - missionCacheMs_ > 1000) {
        std::vector<MissionEntity> fresh;
        listMissionEntities(fresh);
    }
    // The mission script is the one that owns most of the story entities around the player.
    Vec3 me = position(playerPed_);
    std::unordered_map<uint32_t, int> votes;
    for (const MissionEntity& e : missionCache_)
        if (distance(e.pos, me) < 150.0f) ++votes[e.script];
    uint32_t best = 0;
    int bestVotes = 0;
    for (const auto& [script, count] : votes)
        if (count > bestVotes || (count == bestVotes && script < best)) {  // ties: same answer every time
            best = script;
            bestVotes = count;
        }
    return best;
}

void Rdr2Game::setHarmless(EntityHandle h, bool on) {
    if (n::DOES_ENTITY_EXIST(h) && n::IS_ENTITY_A_PED(h)) n::SET_PED_TO_PLAYER_WEAPON_DAMAGE_MODIFIER(h, on ? 0.0f : 1.0f);
}

void Rdr2Game::revive(EntityHandle h, int health) {
    invalidate(h);
    n::RESURRECT_PED(h);
    n::REVIVE_INJURED_PED(h);
    n::SET_ENTITY_HEALTH(h, health, 0);
}

bool Rdr2Game::readCamera(CameraState& out) {
    out.pos = n::GET_FINAL_RENDERED_CAM_COORD();
    out.rot = n::GET_FINAL_RENDERED_CAM_ROT();
    out.fov = n::GET_FINAL_RENDERED_CAM_FOV();
    return out.fov > 1.0f;
}

void Rdr2Game::spectate(bool on, const CameraState& cam) {
    if (on) {
        if (!camera_) {
            camera_ = n::CREATE_CAM("DEFAULT_SCRIPTED_CAMERA");
            if (!camera_) return;
            g_scriptCamera = camera_;
            n::SET_CAM_ACTIVE(camera_, true);
            n::RENDER_SCRIPT_CAMS(true, 500);
        }
        n::SET_CAM_COORD(camera_, cam.pos);
        n::SET_CAM_ROT(camera_, cam.rot);
        n::SET_CAM_FOV(camera_, std::clamp(cam.fov, 1.0f, 130.0f));
        n::SET_FOCUS_POS_AND_VEL(cam.pos);  // stream the world in where the camera is, not where we stand
    } else if (camera_) {
        n::RENDER_SCRIPT_CAMS(false, 500);
        n::SET_CAM_ACTIVE(camera_, false);
        n::DESTROY_CAM(camera_);
        n::CLEAR_FOCUS();
        camera_ = 0;
        g_scriptCamera = 0;
    }
}

bool Rdr2Game::readWaypoint(Vec3& out) {
    if (!n::IS_WAYPOINT_ACTIVE()) return false;
    out = n::GET_WAYPOINT_COORDS();
    return true;
}

void Rdr2Game::showFriendWaypoint(bool on, const Vec3& pos) {
    if (!on) {
        if (waypointBlip_) n::REMOVE_BLIP(&waypointBlip_);
        waypointBlip_ = 0;
        return;
    }
    if (waypointBlip_) {
        n::SET_BLIP_COORDS(waypointBlip_, pos);
        return;
    }
    static const uint32_t kStyles[] = {joaat("BLIP_STYLE_WAYPOINT"), joaat("BLIP_STYLE_FRIENDLY")};
    for (uint32_t style : kStyles)
        if ((waypointBlip_ = n::BLIP_ADD_FOR_COORDS(style, pos)) != 0) break;
    if (waypointBlip_) n::SET_BLIP_NAME(waypointBlip_, n::LITERAL_STRING("Friend's waypoint"));
}

// ---------------------------------------------------------------- world queries

void Rdr2Game::listNearby(const Vec3& a, const Vec3& b, float radius, std::vector<NearbyEntity>& out) {
    const Vec3 points[2] = {a, b};
    listNearbyAny(points, 2, radius, out);
}

void Rdr2Game::listNearbyAny(const Vec3* points, size_t count, float radius, std::vector<NearbyEntity>& out) {
    // One pass over the pools for every viewer at once: each entity is listed once, and the pool
    // and every position are read only once per frame however often this is called.
    if (!count) return;
    const float r2 = radius * radius;
    auto within = [&](const Vec3& p, float limit2) {
        for (size_t i = 0; i < count; ++i) {
            Vec3 d = p - points[i];
            if (d.x * d.x + d.y * d.y + d.z * d.z <= limit2) return true;
        }
        return false;
    };
    auto inRange = [&](const Vec3& p) { return within(p, r2); };
    auto farOut = [&](int h) {
        auto it = info_.find(h);
        if (it == info_.end() || !it->second.haveSeen) return false;
        const Info& in = it->second;
        uint32_t age = nowMs_ - in.seenMs;
        if (age >= kFarRecheckMs) return false;
        float reach = radius + kMaxScanSpeed * static_cast<float>(age) * 0.001f;
        return !within(in.seenPos, reach * reach);
    };
    auto seen = [&](int h) -> Vec3 {
        Cached& c = cached(h);  // the pool only lists existing entities
        c.exists = true;
        c.have |= kHaveExists;
        bool fresh = !(c.have & kHavePos);
        Vec3 pos = position(h);
        if (fresh) {
            Info& in = info_[h];
            in.usedMs = nowMs_;
            in.seenPos = pos;
            in.seenMs = nowMs_;
            in.haveSeen = true;
        }
        return pos;
    };
    for (int ped : peds()) {
        if (!ped || own_.count(ped) || farOut(ped)) continue;
        Vec3 pos = seen(ped);  // distance first: the cheapest way to drop most peds
        if (!inRange(pos)) continue;
        const Info& in = ownerInfo(ped, false);
        if (in.player || in.draftOf || own_.count(ped)) continue;  // draft animals belong to their vehicle
        out.push_back({ped, EntityKind::Ped, pos});
    }
    for (int veh : vehicles()) {
        if (!veh || vehicles_.count(veh) || farOut(veh)) continue;
        Vec3 pos = seen(veh);
        if (!inRange(pos) || kindInfo(veh, true).train) continue;
        out.push_back({veh, EntityKind::Vehicle, pos});
    }
}

void Rdr2Game::readEntity(EntityHandle h, EntityKind kind, EntityRead& out) {
    EntityState& st = out.state;
    const bool vehicle = kind == EntityKind::Vehicle;
    const Info& in = ownerInfo(h, vehicle);
    st.kind = kind;
    st.model = in.model;
    st.script = in.mission ? in.script : 0;
    st.pos = position(h);
    st.vel = n::GET_ENTITY_VELOCITY(h);
    st.heading = heading(h);
    if (vehicle) {
        Vec3 rot = n::GET_ENTITY_ROTATION(h);
        st.pitch = rot.x;
        st.roll = rot.y;
        return;
    }
    const bool human = in.human;
    st.health = static_cast<int16_t>(std::clamp(health(h), 0, 32767));
    if (isDead(h)) {
        st.flags |= NF_Dead;
        npcShots_.erase(h);
        return;
    }
    if (n::IS_PED_IN_ANY_VEHICLE(h)) {
        out.vehicle = n::GET_VEHICLE_PED_IS_IN(h);
        st.seat = static_cast<int8_t>(seatOf(h, out.vehicle));
    } else if (n::IS_PED_ON_MOUNT(h)) {
        out.mount = n::GET_MOUNT(h);
    }
    if (n::IS_PED_RAGDOLL(h)) st.flags |= NF_Ragdoll;
    st.outfit = outfitOf(h);
    if (!human) return;
    st.weapon = n::GET_PED_CURRENT_HELD_WEAPON(h);
    if (n::IS_PED_IN_COMBAT(h, 0)) {
        st.flags |= NF_InCombat;
        out.target = n::GET_CURRENT_TARGET_FOR_PED(h);
        if (out.target && armed(st.weapon)) st.flags |= NF_Aiming;
    }
    // Only a ped holding a weapon can fire: those are watched for gunfire every frame. Drawing a
    // weapon takes longer than the time between two reads, so the first shot is not missed.
    if (armed(st.weapon)) {
        ShotWatch& watch = npcShots_[h];
        watch.readMs = nowMs_;
        if (watch.shotMs && nowMs_ - watch.shotMs < kShotHoldMs) st.flags |= NF_Shooting;
    } else {
        npcShots_.erase(h);
    }
}

void Rdr2Game::listForeign(std::vector<ForeignEntity>& out) {
    for (int ped : peds()) {
        if (!ped || ped == playerPed_) continue;
        if (isSpawned(ped)) {  // ours: no need to ask the game
            out.push_back({ped, 0, false});
            continue;
        }
        const Info& in = ownerInfo(ped, false);
        out.push_back({ped, in.draftOf, in.mission});
    }
    for (int veh : vehicles()) {
        if (!veh) continue;
        out.push_back({veh, 0, !isSpawned(veh) && ownerInfo(veh, true).mission});
    }
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
        int remote = (info.hour % 24) * 60 + info.minute % 60;
        int diff = std::abs(local - remote);
        diff = std::min(diff, 24 * 60 - diff);
        if (diff >= 3) n::SET_CLOCK_TIME(info.hour % 24, info.minute % 60, info.second % 60);
    }
    if (weather && info.weather) {
        // The local game keeps cycling its own weather, so this is checked every time, not only
        // when the owner's weather changes. A transition already heading there is left alone.
        bool there = n::GET_PREV_WEATHER_TYPE_HASH_NAME() == info.weather || n::GET_NEXT_WEATHER_TYPE_HASH_NAME() == info.weather;
        if (!there) {
            // First sync: switch at once; later changes blend in like natural weather.
            n::SET_WEATHER_TYPE(info.weather, lastWeatherSet_ ? 30.0f : 0.0f);
        }
        lastWeatherSet_ = info.weather;
    }
}

// ---------------------------------------------------------------- entities we create and drive

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
        poolsChanged();
        forget(veh);
        n::SET_ENTITY_AS_MISSION_ENTITY(veh);
        n::SET_MODEL_AS_NO_LONGER_NEEDED(model);
        vehicles_.insert(veh);
        return veh;
    }

    int ped = n::CREATE_PED(model, pos, heading);
    if (!ped) return 0;
    poolsChanged();
    forget(ped);
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
    return ped;
}

void Rdr2Game::prefetchModel(uint32_t model) {
    if (n::IS_MODEL_VALID(model)) n::REQUEST_MODEL(model);
}

void Rdr2Game::hideDraftAnimals(int vehicle, bool hidden) {
    for (int harness = 0; harness < kDraftHarnesses; ++harness)
        if (int horse = n::GET_PED_IN_DRAFT_HARNESS(vehicle, harness)) hide(horse, hidden);
}

void Rdr2Game::park(EntityHandle h) {
    invalidate(h);
    if (!n::DOES_ENTITY_EXIST(h)) return;
    if (vehicles_.count(h)) {
        n::SET_ENTITY_VELOCITY(h, Vec3{});
        hideDraftAnimals(h, true);
    } else {
        if (n::IS_PED_ON_MOUNT(h)) n::REMOVE_PED_FROM_MOUNT(h);
        if (n::IS_PED_IN_ANY_VEHICLE(h)) n::CLEAR_PED_TASKS_IMMEDIATELY(h);
        else n::CLEAR_PED_TASKS(h);
        npcShots_.erase(h);
    }
    hide(h, true);
}

void Rdr2Game::unpark(EntityHandle h, const Vec3& pos, float heading) {
    invalidate(h);
    n::SET_ENTITY_COORDS_AND_HEADING_NO_OFFSET(h, pos, heading);
    hide(h, false);
    if (vehicles_.count(h)) {
        hideDraftAnimals(h, false);
    } else {
        n::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(h, true);  // still driven by us, not by its own AI
        n::SET_ENTITY_HEALTH(h, n::GET_ENTITY_MAX_HEALTH(h), 0);
    }
}

bool Rdr2Game::adopt(EntityHandle h) {
    // A mirror the player is riding or driving when its original goes away: it becomes an ordinary
    // entity of this world, where it stands, instead of vanishing under the player.
    if (!isSpawned(h) || !n::DOES_ENTITY_EXIST(h)) return false;
    auto blip = blips_.find(h);
    if (blip != blips_.end()) {
        n::REMOVE_BLIP(&blip->second);
        blips_.erase(blip);
    }
    npcShots_.erase(h);
    quadruped_.erase(h);
    bool vehicle = vehicles_.erase(h) != 0;
    forget(h);
    hide(h, false);
    if (vehicle) {
        hideDraftAnimals(h, false);
        for (int harness = 0; harness < kDraftHarnesses; ++harness)
            if (int horse = n::GET_PED_IN_DRAFT_HARNESS(h, harness)) forget(horse);
    } else {
        // Its own AI again, and the local player's weapons hurt it like any other.
        n::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(h, false);
        n::SET_PED_KEEP_TASK(h, false);
        n::SET_PED_TO_PLAYER_WEAPON_DAMAGE_MODIFIER(h, 1.0f);
    }
    int handle = h;
    n::SET_ENTITY_AS_NO_LONGER_NEEDED(&handle);  // the game may clean it up once nobody uses it
    poolsChanged();
    // For a guest an ordinary local entity is foreign population, and nothing else keeps a wagon
    // the player stepped off (there is no last-vehicle equivalent of GET_LAST_MOUNT).
    if (std::find(adopted_.begin(), adopted_.end(), h) == adopted_.end()) {
        if (adopted_.size() >= kMaxAdopted) adopted_.erase(adopted_.begin());
        adopted_.push_back(h);
    }
    own_.insert(h);
    ownHorsesDueMs_ = nowMs_;  // the next frame's refresh takes it into ownHorses_
    return true;
}

void Rdr2Game::remove(EntityHandle h) {
    adopted_.erase(std::remove(adopted_.begin(), adopted_.end(), h), adopted_.end());
    auto blip = blips_.find(h);
    if (blip != blips_.end()) {
        n::REMOVE_BLIP(&blip->second);
        blips_.erase(blip);
    }
    forget(h);
    quadruped_.erase(h);
    bool vehicle = vehicles_.erase(h) != 0;
    if (!n::DOES_ENTITY_EXIST(h)) return;
    poolsChanged();
    int handle = h;
    n::SET_ENTITY_AS_MISSION_ENTITY(handle);
    if (vehicle) {
        for (int harness = 0; harness < kDraftHarnesses; ++harness) {  // take the draft animals with it
            int horse = n::GET_PED_IN_DRAFT_HARNESS(handle, harness);
            if (horse) {
                forget(horse);
                n::SET_ENTITY_AS_MISSION_ENTITY(horse);
                n::DELETE_PED(&horse);
            }
        }
        n::DELETE_VEHICLE(&handle);
    } else {
        n::DELETE_PED(&handle);
    }
}

bool Rdr2Game::exists(EntityHandle h) {
    if (h == 0) return false;
    Cached& c = cached(h);
    if (!(c.have & kHaveExists)) {
        // Our own entities are persistent (mission entities): asking every frame is not needed.
        auto known = isSpawned(h) ? info_.find(h) : info_.end();
        // The facts may have been created by another query (kindInfo, the world scan) that never
        // asked whether it exists: only a deadline this check set itself counts.
        if (known != info_.end() && known->second.haveAlive && !due(nowMs_, known->second.aliveUntilMs)) {
            c.exists = true;
        } else {
            c.exists = n::DOES_ENTITY_EXIST(h);
            if (c.exists && isSpawned(h)) {
                Info& in = info_[h];
                in.usedMs = nowMs_;
                in.aliveUntilMs = nowMs_ + kAliveMs;
                in.haveAlive = true;
            }
        }
        c.have |= kHaveExists;
    }
    return c.exists;
}
bool Rdr2Game::isDead(EntityHandle h) {
    Cached& c = cached(h);
    if (!(c.have & kHaveDead)) {
        c.dead = n::IS_ENTITY_DEAD(h);
        c.have |= kHaveDead;
    }
    return c.dead;
}
Vec3 Rdr2Game::position(EntityHandle h) {
    Cached& c = cached(h);
    if (!(c.have & kHavePos)) {
        c.pos = n::GET_ENTITY_COORDS(h);
        c.have |= kHavePos;
    }
    return c.pos;
}
float Rdr2Game::heading(EntityHandle h) {
    Cached& c = cached(h);
    if (!(c.have & kHaveHeading)) {
        c.heading = n::GET_ENTITY_HEADING(h);
        c.have |= kHaveHeading;
    }
    return c.heading;
}
int Rdr2Game::health(EntityHandle h) {
    Cached& c = cached(h);
    if (!(c.have & kHaveHealth)) {
        c.health = isDead(h) ? 0 : n::GET_ENTITY_HEALTH(h);
        c.have |= kHaveHealth;
    }
    return c.health;
}

void Rdr2Game::setHealth(EntityHandle h, int health, int maxHealth) {
    invalidate(h);
    if (maxHealth > 0 && n::GET_ENTITY_MAX_HEALTH(h) != maxHealth) n::SET_ENTITY_MAX_HEALTH(h, maxHealth);
    n::SET_ENTITY_HEALTH(h, health, 0);
}

void Rdr2Game::kill(EntityHandle h) {
    invalidate(h);
    n::SET_ENTITY_HEALTH(h, 0, 0);
}

void Rdr2Game::teleport(EntityHandle h, const Vec3& pos, float heading) {
    invalidate(h);
    n::SET_ENTITY_COORDS_AND_HEADING_NO_OFFSET(h, pos, heading);
}

void Rdr2Game::nudge(EntityHandle h, const Vec3& delta) {
    Vec3 to = position(h) + delta;
    n::SET_ENTITY_COORDS_NO_OFFSET(h, to);
    cached(h).pos = to;
}

void Rdr2Game::moveTo(EntityHandle h, const Vec3& pos, float speed, float heading) {
    auto it = quadruped_.find(h);
    bool quadruped = it != quadruped_.end() ? it->second : !kindInfo(h, false).human;
    n::TASK_GO_STRAIGHT_TO_COORD(h, pos, moveBlend(speed, quadruped), heading);
}

void Rdr2Game::standStill(EntityHandle h, float heading) {
    // No CLEAR_PED_TASKS: replacing the task lets the ped blend out of its walk instead of snapping.
    n::SET_PED_DESIRED_HEADING(h, heading);
    n::TASK_STAND_STILL(h, -1);
}

void Rdr2Game::face(EntityHandle h, float heading) { n::SET_PED_DESIRED_HEADING(h, heading); }

void Rdr2Game::aimAt(EntityHandle h, const Vec3& target) { n::TASK_AIM_GUN_AT_COORD(h, target, 1500); }

void Rdr2Game::shootAt(EntityHandle h, const Vec3& target) {
    static const uint32_t kFullAuto = joaat("FIRING_PATTERN_FULL_AUTO");
    n::TASK_SHOOT_AT_COORD(h, target, 800, kFullAuto);
}

void Rdr2Game::setWeapon(EntityHandle h, uint32_t weapon) {
    if (!armed(weapon)) {
        n::SET_CURRENT_PED_WEAPON(h, kUnarmed);
        return;
    }
    static const uint32_t kAddReasonDefault = joaat("ADD_REASON_DEFAULT");
    n::GIVE_WEAPON_TO_PED(h, weapon, 200, kAddReasonDefault);
    n::SET_CURRENT_PED_WEAPON(h, weapon);
    n::SET_PED_INFINITE_AMMO(h, true, weapon);
}

void Rdr2Game::setMount(EntityHandle rider, EntityHandle mount) {
    invalidate(rider);
    if (mount) n::SET_PED_ONTO_MOUNT(rider, mount);
    else n::REMOVE_PED_FROM_MOUNT(rider);
}

void Rdr2Game::setVehicle(EntityHandle ped, EntityHandle vehicle, int seat) {
    invalidate(ped);
    if (vehicle) n::SET_PED_INTO_VEHICLE(ped, vehicle, seat);
    else n::CLEAR_PED_TASKS_IMMEDIATELY(ped);  // pops the ped out of its seat
}

void Rdr2Game::setCrouch(EntityHandle h, bool crouch) { n::SET_PED_CROUCH_MOVEMENT(h, crouch); }

void Rdr2Game::ragdoll(EntityHandle h) { n::SET_PED_TO_RAGDOLL(h, 800, 2500); }

void Rdr2Game::setLabel(EntityHandle h, const char* name) {
    static const uint32_t kFriendlyBlip = joaat("BLIP_STYLE_FRIENDLY");
    const std::string label = hud::printable(name ? name : "", 48);  // a name from the network
    auto old = blips_.find(h);  // a second label replaces the first instead of leaking its blip
    if (old != blips_.end()) {
        n::REMOVE_BLIP(&old->second);
        blips_.erase(old);
    }
    int blip = n::BLIP_ADD_FOR_ENTITY(kFriendlyBlip, h);
    if (blip) {
        n::SET_BLIP_NAME(blip, n::LITERAL_STRING(label.c_str()));
        blips_[h] = blip;
    }
    n::SET_PED_PROMPT_NAME(h, n::LITERAL_STRING(label.c_str()));
}

void Rdr2Game::setOutfit(EntityHandle h, uint32_t outfit) {
    if (!outfit) return;
    n::EQUIP_META_PED_OUTFIT(h, outfit);
    n::UPDATE_PED_VARIATION(h);
}

void Rdr2Game::steerVehicle(EntityHandle h, const Vec3& velocity, float targetHeading, float, float, float dt) {
    n::SET_ENTITY_VELOCITY(h, velocity);
    // Turn gradually (frame-rate independent); pitch and roll stay with the physics so wheels and
    // hulls follow the ground.
    float current = heading(h);
    float delta = angleDelta(current, targetHeading);
    if (std::fabs(delta) > 1.0f) {
        Vec3 rot = n::GET_ENTITY_ROTATION(h);
        rot.z = wrapHeading(current + delta * (1.0f - std::exp(-dt * 10.0f)));
        n::SET_ENTITY_ROTATION(h, rot);
        invalidate(h);
    }
}

// ---------------------------------------------------------------- shared-world helpers

void Rdr2Game::damageNpc(EntityHandle npc, int amount, EntityHandle attacker) {
    invalidate(npc);
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
    invalidate(h);
    if (!n::DOES_ENTITY_EXIST(h)) {
        forget(h);
        return ForeignAction::Deleted;
    }
    // Ambient population is deleted: hidden entities would still cost AI and physics time.
    // Entities owned by a story script are only hidden, so the script does not break. Asked fresh
    // here (not from the cache): deleting a script's entity is not something to get wrong.
    if (n::IS_ENTITY_A_MISSION_ENTITY(h)) {
        hide(h, true);
        return ForeignAction::Hidden;
    }
    forget(h);
    poolsChanged();
    int handle = h;
    n::SET_ENTITY_AS_MISSION_ENTITY(handle);
    if (n::IS_ENTITY_A_PED(handle)) n::DELETE_PED(&handle);
    else n::DELETE_VEHICLE(&handle);
    return ForeignAction::Deleted;
}

void Rdr2Game::restoreForeign(EntityHandle h) {
    if (n::DOES_ENTITY_EXIST(h)) hide(h, false);
}
