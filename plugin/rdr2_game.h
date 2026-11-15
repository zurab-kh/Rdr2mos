// IGame implemented with RDR2 script natives. Must only be used from the script thread.
#pragma once
#include <unordered_map>
#include <unordered_set>
#include "game_api.h"

class Rdr2Game final : public coop::IGame {
public:
    // Call once per frame before the session ticks: refreshes cached handles and catches
    // one-frame events (gunshots) that slower polling would miss.
    void frame(uint32_t nowMs);

    bool ready() override;
    coop::LocalPlayer readLocalPlayer() override;
    coop::EntityHandle localPlayerPed() override { return playerPed_; }
    void damageLocalPlayer(int amount) override;
    void teleportLocalPlayer(const coop::Vec3& pos) override;
    bool belongsToLocalPlayer(coop::EntityHandle h) override;

    void listNearby(const coop::Vec3& a, const coop::Vec3& b, float radius, std::vector<coop::NearbyEntity>& out) override;
    void readEntity(coop::EntityHandle h, coop::EntityKind kind, coop::EntityRead& out) override;
    void listForeign(std::vector<coop::ForeignEntity>& out) override;
    coop::WorldInfo readWorldInfo() override;
    void applyWorldInfo(const coop::WorldInfo& info, bool clock, bool weather) override;

    coop::EntityHandle spawn(coop::EntityKind kind, uint32_t model, const coop::Vec3& pos, float heading,
                             coop::SpawnRole role) override;
    void remove(coop::EntityHandle h) override;
    bool exists(coop::EntityHandle h) override;
    bool isDead(coop::EntityHandle h) override;
    coop::Vec3 position(coop::EntityHandle h) override;
    float heading(coop::EntityHandle h) override;
    int health(coop::EntityHandle h) override;
    void setHealth(coop::EntityHandle h, int health, int maxHealth) override;
    void kill(coop::EntityHandle h) override;
    void teleport(coop::EntityHandle h, const coop::Vec3& pos, float heading) override;
    void nudge(coop::EntityHandle h, const coop::Vec3& delta) override;

    void moveTo(coop::EntityHandle h, const coop::Vec3& pos, float speed, float heading) override;
    void standStill(coop::EntityHandle h, float heading) override;
    void aimAt(coop::EntityHandle h, const coop::Vec3& target) override;
    void shootAt(coop::EntityHandle h, const coop::Vec3& target) override;
    void setWeapon(coop::EntityHandle h, uint32_t weapon) override;
    void setMount(coop::EntityHandle rider, coop::EntityHandle mount) override;
    void setVehicle(coop::EntityHandle ped, coop::EntityHandle vehicle, int seat) override;
    void setCrouch(coop::EntityHandle h, bool crouch) override;
    void ragdoll(coop::EntityHandle h) override;
    void setLabel(coop::EntityHandle h, const char* name) override;
    void steerVehicle(coop::EntityHandle h, const coop::Vec3& velocity, float heading, float pitch, float roll) override;

    void damageNpc(coop::EntityHandle npc, int amount, coop::EntityHandle attacker) override;
    bool wasDamagedByLocalPlayer(coop::EntityHandle h) override;
    void suppressAmbientPopulation() override;
    coop::ForeignAction removeForeign(coop::EntityHandle h) override;
    void restoreForeign(coop::EntityHandle h) override;

private:
    int seatOf(int ped, int vehicle);
    void hide(int h, bool hidden);

    uint32_t nowMs_ = 0;
    int playerPed_ = 0;
    int playerMount_ = 0;
    int playerVehicle_ = 0;
    std::unordered_set<int> own_;                   // player, their horses, their vehicle and its draft animals
    std::unordered_set<int> vehicles_;              // vehicles we created
    std::unordered_map<int, bool> quadruped_;       // peds we created: horse/animal (different gaits)
    int puppetPed_ = 0;                             // the friend's character, if spawned
    uint32_t lastPlayerShotMs_ = 0;
    bool playerShotSeen_ = false;
    struct ShotWatch {
        uint32_t shotMs = 0;   // last time seen firing
        uint32_t readMs = 0;   // last time the session asked about this NPC
    };
    std::unordered_map<int, ShotWatch> npcShots_;  // streamed NPCs, watched for gunfire every frame
    std::unordered_map<int, int> blips_;
    uint32_t lastWeatherSet_ = 0;
};
