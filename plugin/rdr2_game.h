// IGame implemented with RDR2 script natives. Must only be used from the script thread.
//
// Every native call goes through Script Hook (a hash lookup and a call into the game), and the
// session asks many questions per frame, so this class answers from caches wherever the answer
// cannot have changed: per frame (positions, health, the ped pool), per handle for a while
// (model, owning script, outfit, seat), and only re-asks the game when it could be different.
#pragma once
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "game_api.h"

class Rdr2Game final : public coop::IGame {
public:
    // Call once per frame before the session ticks: refreshes cached handles and catches
    // one-frame events (gunshots) that slower polling would miss. `active` is false while the
    // mod is offline: then only the safety checks run (a frozen world or a script camera left
    // over from before a script restart), not the work the session needs.
    void frame(uint32_t nowMs, bool active = true);

    bool ready() override;
    coop::LocalPlayer readLocalPlayer() override;
    coop::EntityHandle localPlayerPed() override { return playerPed_; }
    void damageLocalPlayer(int amount) override;
    void teleportLocalPlayer(const coop::Vec3& pos) override;
    uint32_t localOutfit() override;
    uint32_t localModel() override;
    bool setLocalModel(uint32_t model) override;
    bool modelExists(uint32_t model) override;
    bool readAppearance(coop::EntityHandle ped, coop::Appearance& out) override;
    void applyAppearance(coop::EntityHandle ped, const coop::Appearance& look) override;
    int outfitPresets(coop::EntityHandle ped) override;
    void setOutfitPreset(coop::EntityHandle ped, int preset) override;
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
    void listNearbyAny(const coop::Vec3* points, size_t count, float radius,
                       std::vector<coop::NearbyEntity>& out) override;
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
    bool adopt(coop::EntityHandle h) override;
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
    // same entities several times a frame.
    enum : uint8_t { kHaveExists = 1, kHavePos = 2, kHaveDead = 4, kHaveHeading = 8, kHaveHealth = 16 };
    struct Cached {
        uint8_t have = 0;  // which of the fields below are filled
        bool exists = false, dead = false;
        coop::Vec3 pos;
        float heading = 0;
        int health = 0;
    };
    Cached& cached(int h) { return cache_[h]; }
    void invalidate(int h) { cache_.erase(h); }

    // Facts about a handle that change rarely or never, re-read on a timer instead of every time.
    // Refresh times are spread over the interval, so a crowd seen in one frame does not come due
    // in one frame again (that would be a frame-time spike).
    struct Info {
        uint32_t model = 0;
        bool human = false, player = false, train = false;  // what it is
        bool mission = false;                               // owned by a script
        uint32_t script = 0;
        int draftOf = 0;                                    // draft animals: the vehicle they pull
        uint32_t outfit = 0;
        int seatVehicle = 0;                                // last seat found (one call to confirm)
        int seat = -1;
        coop::Vec3 seenPos;                                 // last position read by the world scan
        uint32_t seenMs = 0;
        bool haveSeen = false;
        uint32_t kindDueMs = 0, ownerDueMs = 0, outfitDueMs = 0, aliveUntilMs = 0;
        uint32_t usedMs = 0;
        // Each deadline above counts only with its flag: 0 is not "due" once the clock (time since
        // boot) is past 2^31 ms, where the wrap-safe comparison takes it for a time in the future.
        bool haveKind = false, haveOwner = false, haveOutfit = false, haveAlive = false;
    };
    Info& kindInfo(int h, bool vehicle);   // model, human, player, train
    Info& ownerInfo(int h, bool vehicle);  // + mission, script, draftOf
    uint32_t outfitOf(int ped);
    uint32_t spreadDue(int h, uint32_t intervalMs) const;
    void forget(int h);

    // The ped and vehicle pools, read once per frame (and again after we create or delete).
    const std::vector<int>& peds();
    const std::vector<int>& vehicles();
    void poolsChanged() { pedsFresh_ = vehiclesFresh_ = false; }

    bool missionFlag();
    int seatOf(int ped, int vehicle);
    void hide(int h, bool hidden);
    void hideDraftAnimals(int vehicle, bool hidden);
    void refreshOwnHorses(bool force);

    std::unordered_map<int, Cached> cache_;
    std::unordered_map<int, Info> info_;
    std::vector<int> peds_, vehiclesList_;
    bool pedsFresh_ = false, vehiclesFresh_ = false;
    uint32_t lastPruneMs_ = 0;

    uint32_t nowMs_ = 0;
    int player_ = 0;
    int playerPed_ = 0;
    int playerMount_ = 0;
    int playerVehicle_ = 0;
    int loading_ = -1;                              // this frame's loading screen state, -1 = not read yet
    int missionFlag_ = -1;                          // this frame's GET_MISSION_FLAG, -1 = not read yet
    std::unordered_set<int> own_;                   // player, their horses, their vehicle and its draft animals
    std::vector<int> ownHorses_;                    // last, owned and saddle horse, draft team of the vehicle
    int ownHorsesPed_ = 0, ownHorsesVehicle_ = 0, ownHorsesMount_ = 0;
    uint32_t ownHorsesDueMs_ = 0;
    bool ownHorsesFresh_ = false;                   // refreshed this frame already
    int playerMaxHealth_ = 0;
    uint32_t playerMaxHealthDueMs_ = 0;
    bool haveMaxHealth_ = false;                    // playerMaxHealthDueMs_ is only valid with this
    // Mirrors adopted into the local world (see adopt()): they stay the player's while they exist,
    // like the last horse, so a guest who steps off an adopted wagon does not see it deleted as
    // foreign population. Few: one per mirror the player was riding when its original went away.
    std::vector<int> adopted_;
    std::unordered_set<int> vehicles_;              // vehicles we created
    std::unordered_map<int, bool> quadruped_;       // peds we created: horse/animal (different gaits)
    uint32_t lastPlayerShotMs_ = 0;
    bool playerShotSeen_ = false;
    struct ShotWatch {
        uint32_t shotMs = 0;   // last time seen firing
        uint32_t readMs = 0;   // last time the session asked about this NPC
    };
    std::unordered_map<int, ShotWatch> npcShots_;  // armed streamed NPCs, watched for gunfire every frame
    std::unordered_map<int, int> blips_;
    uint32_t lastWeatherSet_ = 0;
    bool protected_ = false;
    bool held_ = false;
    int camera_ = 0;
    int waypointBlip_ = 0;
    std::vector<coop::MissionEntity> missionCache_;
    uint32_t missionCacheMs_ = 0;
    bool missionCacheValid_ = false;
    bool isSpawned(int h) const { return vehicles_.count(h) || quadruped_.count(h); }
};
