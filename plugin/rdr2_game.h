// IGame implemented with RDR2 script natives. Must only be used from the script thread.
#pragma once
#include <map>
#include <set>
#include "game_api.h"

class Rdr2Game final : public coop::IGame {
public:
    // Call once per frame before the session ticks: refreshes cached handles and catches
    // one-frame events (gunshots) that slower polling would miss.
    void frame(uint32_t nowMs);

    bool ready() override;
    coop::PlayerState readLocalPlayer() override;
    void damageLocalPlayer(int amount) override;
    void teleportLocalPlayer(const coop::Vec3& pos) override;
    bool belongsToLocalPlayer(coop::EntityHandle h) override;

    void listNpcs(const coop::Vec3& a, const coop::Vec3& b, float radius,
                  const std::vector<coop::EntityHandle>& exclude, std::vector<coop::LocalNpc>& out) override;
    void listAllPeds(std::vector<coop::EntityHandle>& out) override;
    coop::WorldInfo readWorldInfo() override;
    void applyWorldInfo(const coop::WorldInfo& info) override;

    coop::EntityHandle spawnPed(uint32_t model, const coop::Vec3& pos, float heading, coop::PedKind kind) override;
    void deletePed(coop::EntityHandle h) override;
    bool exists(coop::EntityHandle h) override;
    bool isDead(coop::EntityHandle h) override;
    coop::Vec3 position(coop::EntityHandle h) override;
    float heading(coop::EntityHandle h) override;
    int health(coop::EntityHandle h) override;
    void setHealth(coop::EntityHandle h, int health, int maxHealth) override;
    void kill(coop::EntityHandle h) override;
    void teleport(coop::EntityHandle h, const coop::Vec3& pos, float heading) override;
    void moveTo(coop::EntityHandle h, const coop::Vec3& pos, float speed, float heading) override;
    void standStill(coop::EntityHandle h, float heading) override;
    void aimAt(coop::EntityHandle h, const coop::Vec3& target) override;
    void shootAt(coop::EntityHandle h, const coop::Vec3& target) override;
    void setWeapon(coop::EntityHandle h, uint32_t weapon) override;
    void setMount(coop::EntityHandle rider, coop::EntityHandle mount) override;
    void setLabel(coop::EntityHandle h, const char* name) override;

    void damageNpc(coop::EntityHandle npc, int amount, coop::EntityHandle attacker) override;
    bool wasDamagedByLocalPlayer(coop::EntityHandle h) override;
    void suppressAmbientPopulation() override;
    void setHidden(coop::EntityHandle h, bool hidden) override;

private:
    uint32_t nowMs_ = 0;
    int playerPed_ = 0;
    int playerMount_ = 0;
    std::set<int> ownPeds_;                     // player, mounts and horses that belong to them
    int puppetPed_ = 0;                         // the friend's character, if spawned
    uint32_t lastPlayerShotMs_ = 0;
    bool playerShotSeen_ = false;
    std::map<int, uint32_t> npcShotMs_;         // NPCs we report -> last time seen firing
    std::map<int, int> blips_;
};
