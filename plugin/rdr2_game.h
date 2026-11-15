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
    uint32_t localOutfit() override;
    void setLocalProtected(bool on) override;
    void killLocalPlayer() override;
    void holdWorld(bool on) override;
    uint32_t localMissionScript() override;
    void listMissionEntities(std::vector<coop::MissionEntity>& out) override;
    void setHarmless(coop::EntityHandle h, bool on) override;
    void revive(coop::EntityHandle h, int health) override;
    bool readCamera(coop::CameraState& out) override;
    void spectate(bool on, const coop::CameraState& cam) override;
    bool readWaypoint(coop::Vec3& out) override;
    void showFriendWaypoint(bool on, const coop::Vec3& pos) override;
    bool belongsToLocalPlayer(coop::EntityHandle h) override;

    void listNearby(const coop::Vec3& a, const coop::Vec3& b, float radius, std::vector<coop::NearbyEntity>& out) override;
    void readEntity(coop::EntityHandle h, coop::EntityKind kind, coop::EntityRead& out) override;
    void listForeign(std::vector<coop::ForeignEntity>& out) override;
    coop::WorldInfo readWorldInfo() override;
    void applyWorldInfo(const coop::WorldInfo& info, bool clock, bool weather) override;

    coop::EntityHandle spawn(coop::EntityKind kind, uint32_t model, const coop::Vec3& pos, float heading,
                             coop::SpawnRole role) override;
    void prefetchModel(uint32_t model) override;
    void park(coop::EntityHandle h) override;
    void unpark(coop::EntityHandle h, const coop::Vec3& pos, float heading) override;
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
    void face(coop::EntityHandle h, float heading) override;
    void aimAt(coop::EntityHandle h, const coop::Vec3& target) override;
    void shootAt(coop::EntityHandle h, const coop::Vec3& target) override;
    void setWeapon(coop::EntityHandle h, uint32_t weapon) override;
    void setMount(coop::EntityHandle rider, coop::EntityHandle mount) override;
    void setVehicle(coop::EntityHandle ped, coop::EntityHandle vehicle, int seat) override;
    void setCrouch(coop::EntityHandle h, bool crouch) override;
    void ragdoll(coop::EntityHandle h) override;
    void setLabel(coop::EntityHandle h, const char* name) override;
    void setOutfit(coop::EntityHandle h, uint32_t outfit) override;
    void steerVehicle(coop::EntityHandle h, const coop::Vec3& velocity, float heading, float pitch, float roll,
                      float dt) override;

    void damageNpc(coop::EntityHandle npc, int amount, coop::EntityHandle attacker) override;
    bool wasDamagedByLocalPlayer(coop::EntityHandle h) override;
    void suppressAmbientPopulation() override;
    coop::ForeignAction removeForeign(coop::EntityHandle h) override;
    void restoreForeign(coop::EntityHandle h) override;

private:
    // Per-frame cache of the most frequent queries: the session asks the same questions about the
    // same entities several times a frame, and each native call has a real cost.
    struct Cached {
        uint8_t have = 0;  // which of the fields below are filled
        bool exists = false, dead = false;
        coop::Vec3 pos;
        float heading = 0;
        int health = 0;
    };
    Cached& cached(int h) { return cache_[h]; }
    void invalidate(int h) { cache_.erase(h); }

    // Classification that never changes for an entity (is it a player, a draft animal, a train),
    // remembered for a while so the world scan does not ask again every tick.
    struct Skip {
        uint32_t untilMs = 0;
        bool skip = false;
    };
    bool skipPed(int ped);
    bool skipVehicle(int veh);

    int seatOf(int ped, int vehicle);
    void hide(int h, bool hidden);
    void hideDraftAnimals(int vehicle, bool hidden);

    std::unordered_map<int, Cached> cache_;
    std::unordered_map<int, Skip> skip_;

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
    bool protected_ = false;
    bool held_ = false;
    int camera_ = 0;
    int waypointBlip_ = 0;
    std::vector<coop::MissionEntity> missionCache_;
    uint32_t missionCacheMs_ = 0;
    bool isSpawned(int h) const { return vehicles_.count(h) || quadruped_.count(h); }
};
