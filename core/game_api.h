// Everything the co-op logic needs from the game. The plugin implements it with script
// natives; the tests implement it with a small simulated world.
//
// Performance contract: listNearby() is called often and must be cheap (handle + position).
// The expensive readEntity() is only called for entities whose update is due.
#pragma once
#include <cstdint>
#include <vector>
#include "protocol.h"

namespace coop {

using EntityHandle = int32_t;  // 0 = none

enum class SpawnRole {
    PlayerPuppet,   // the friend's character
    PuppetMount,    // the friend's own horse
    PuppetVehicle,  // a vehicle the friend owns in their world
    Mirror,         // guest-side copy of an entity that lives in the host's world
};

struct LocalPlayer {
    PlayerState state;          // timeMs and the *Ref fields are filled in by the session
    EntityHandle mount = 0;     // ped the player rides
    EntityHandle vehicle = 0;   // vehicle the player sits in
};

struct NearbyEntity {
    EntityHandle handle = 0;
    EntityKind kind = EntityKind::Ped;
    Vec3 pos;
};

struct EntityRead {
    EntityState state;          // everything except id/fields/refs, which the session fills in
    EntityHandle mount = 0;
    EntityHandle vehicle = 0;
    EntityHandle target = 0;    // who this ped is fighting / aiming at
};

struct ForeignEntity {
    EntityHandle handle = 0;
    EntityHandle draftOf = 0;   // for draft animals: the vehicle they pull
};

enum class ForeignAction { Deleted, Hidden };

class IGame {
public:
    virtual ~IGame() = default;

    // False while loading, in menus before a save is loaded, etc. Nothing is touched then.
    virtual bool ready() = 0;

    // --- local player ---
    virtual LocalPlayer readLocalPlayer() = 0;
    virtual EntityHandle localPlayerPed() = 0;
    virtual void damageLocalPlayer(int amount) = 0;
    virtual void teleportLocalPlayer(const Vec3& pos) = 0;
    // The player, their current mount and their own horses: never mirrored, never removed.
    virtual bool belongsToLocalPlayer(EntityHandle h) = 0;

    // --- world queries ---
    // Peds and vehicles within `radius` of either point. Excludes the local player's own peds,
    // draft animals (they belong to their vehicle) and trains.
    virtual void listNearby(const Vec3& a, const Vec3& b, float radius, std::vector<NearbyEntity>& out) = 0;
    virtual void readEntity(EntityHandle h, EntityKind kind, EntityRead& out) = 0;
    // Every ped and vehicle in the local world (guest: used to clear out the local population).
    virtual void listForeign(std::vector<ForeignEntity>& out) = 0;
    virtual WorldInfo readWorldInfo() = 0;
    virtual void applyWorldInfo(const WorldInfo& info, bool clock, bool weather) = 0;

    // --- entities we create and drive ---
    // Returns 0 while the model is still streaming in; call again on a later frame.
    virtual EntityHandle spawn(EntityKind kind, uint32_t model, const Vec3& pos, float heading, SpawnRole role) = 0;
    virtual void remove(EntityHandle h) = 0;
    virtual bool exists(EntityHandle h) = 0;
    virtual bool isDead(EntityHandle h) = 0;
    virtual Vec3 position(EntityHandle h) = 0;
    virtual float heading(EntityHandle h) = 0;
    virtual int health(EntityHandle h) = 0;
    virtual void setHealth(EntityHandle h, int health, int maxHealth) = 0;  // maxHealth 0 = leave as is
    virtual void kill(EntityHandle h) = 0;
    virtual void teleport(EntityHandle h, const Vec3& pos, float heading) = 0;
    // Small positional correction while the entity keeps playing its movement animation.
    virtual void nudge(EntityHandle h, const Vec3& delta) = 0;

    // peds
    virtual void moveTo(EntityHandle h, const Vec3& pos, float speed, float heading) = 0;
    virtual void standStill(EntityHandle h, float heading) = 0;
    virtual void aimAt(EntityHandle h, const Vec3& target) = 0;
    virtual void shootAt(EntityHandle h, const Vec3& target) = 0;
    virtual void setWeapon(EntityHandle h, uint32_t weapon) = 0;
    virtual void setMount(EntityHandle rider, EntityHandle mount) = 0;                 // mount 0 = get off
    virtual void setVehicle(EntityHandle ped, EntityHandle vehicle, int seat) = 0;      // vehicle 0 = get out
    virtual void setCrouch(EntityHandle h, bool crouch) = 0;
    virtual void ragdoll(EntityHandle h) = 0;
    virtual void setLabel(EntityHandle h, const char* name) = 0;  // map blip + name for the friend

    // vehicles: called every frame with the velocity and orientation the vehicle should have
    virtual void steerVehicle(EntityHandle h, const Vec3& velocity, float heading, float pitch, float roll) = 0;

    // --- shared-world helpers ---
    // Applies damage to a real NPC on behalf of the friend (attacker = their puppet, may be 0).
    virtual void damageNpc(EntityHandle npc, int amount, EntityHandle attacker) = 0;
    virtual bool wasDamagedByLocalPlayer(EntityHandle h) = 0;  // also clears the damage record
    // Guest only: stop the local game from spawning its own population this frame.
    virtual void suppressAmbientPopulation() = 0;
    // Guest only: get rid of a local entity that does not exist in the host's world. Ambient
    // entities are deleted outright (cheapest); script-owned ones are only hidden.
    virtual ForeignAction removeForeign(EntityHandle h) = 0;
    virtual void restoreForeign(EntityHandle h) = 0;
};

}  // namespace coop
