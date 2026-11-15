// Everything the co-op logic needs from the game. The plugin implements it with script
// natives; the tests implement it with a small simulated world.
#pragma once
#include <cstdint>
#include <vector>
#include "protocol.h"

namespace coop {

using EntityHandle = int32_t;  // 0 = none

enum class PedKind {
    PlayerPuppet,  // the friend's character
    PuppetMount,   // the horse the friend rides
    NpcMirror,     // guest-side copy of an NPC that lives in the host's world
};

struct LocalNpc {
    EntityHandle handle = 0;
    EntityHandle mount = 0;      // handle of the ped this one rides
    bool fightsPeer = false;     // in combat with the friend's puppet
    NpcState state;              // id and mountId are filled in by the session
};

class IGame {
public:
    virtual ~IGame() = default;

    // False while loading, in menus before a save is loaded, etc. Nothing is touched then.
    virtual bool ready() = 0;

    // --- local player ---
    virtual PlayerState readLocalPlayer() = 0;                  // timeMs is filled in by the session
    virtual void damageLocalPlayer(int amount) = 0;
    virtual void teleportLocalPlayer(const Vec3& pos) = 0;
    // The player, their current mount and their own horse: never mirrored, never hidden.
    virtual bool belongsToLocalPlayer(EntityHandle h) = 0;

    // --- world queries ---
    // NPCs within `radius` of either point (excluding anything in `exclude` and the local player's own).
    virtual void listNpcs(const Vec3& a, const Vec3& b, float radius, const std::vector<EntityHandle>& exclude,
                          std::vector<LocalNpc>& out) = 0;
    virtual void listAllPeds(std::vector<EntityHandle>& out) = 0;
    virtual WorldInfo readWorldInfo() = 0;
    virtual void applyWorldInfo(const WorldInfo& info) = 0;

    // --- peds we create and drive ---
    // Returns 0 while the model is still streaming in; call again next frame.
    virtual EntityHandle spawnPed(uint32_t model, const Vec3& pos, float heading, PedKind kind) = 0;
    virtual void deletePed(EntityHandle h) = 0;
    virtual bool exists(EntityHandle h) = 0;
    virtual bool isDead(EntityHandle h) = 0;
    virtual Vec3 position(EntityHandle h) = 0;
    virtual float heading(EntityHandle h) = 0;
    virtual int health(EntityHandle h) = 0;
    virtual void setHealth(EntityHandle h, int health, int maxHealth) = 0;  // maxHealth 0 = leave as is
    virtual void kill(EntityHandle h) = 0;
    virtual void teleport(EntityHandle h, const Vec3& pos, float heading) = 0;
    virtual void moveTo(EntityHandle h, const Vec3& pos, float speed, float heading) = 0;
    virtual void standStill(EntityHandle h, float heading) = 0;
    virtual void aimAt(EntityHandle h, const Vec3& target) = 0;
    virtual void shootAt(EntityHandle h, const Vec3& target) = 0;
    virtual void setWeapon(EntityHandle h, uint32_t weapon) = 0;
    virtual void setMount(EntityHandle rider, EntityHandle mount) = 0;  // mount 0 = get off
    virtual void setLabel(EntityHandle h, const char* name) = 0;         // map blip + name for the friend

    // --- shared-world helpers ---
    // Applies damage to a real NPC on behalf of the friend (attacker = their puppet, may be 0).
    virtual void damageNpc(EntityHandle npc, int amount, EntityHandle attacker) = 0;
    virtual bool wasDamagedByLocalPlayer(EntityHandle h) = 0;  // also clears the damage record
    // Guest only: stop the local game from spawning its own ambient population this frame.
    virtual void suppressAmbientPopulation() = 0;
    // Guest only: hide (or restore) a locally spawned ped that does not exist in the host's world.
    virtual void setHidden(EntityHandle h, bool hidden) = 0;
};

}  // namespace coop
