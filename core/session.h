// The co-op session: turns the link + the game interface into one shared story world.
//
//  * Each player owns their own character; the other side shows it as a "puppet".
//  * The host owns the world. Peds, horses and vehicles near either player are streamed to the
//    guest, who shows them as "mirrors" and keeps its own population switched off.
//  * Streaming is prioritised: close entities update at the full rate, distant ones less often,
//    idle ones only send a keyframe once a second, and only changed fields are sent.
//  * Damage is forwarded to whoever owns the victim.
//  * Control can move across: if the guest drives a host wagon or rides a host horse, the host
//    moves the real one from the guest's input stream.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "game_api.h"
#include "interp.h"
#include "link.h"
#include "session_config.h"
#include "story.h"

namespace coop {



struct SessionStatus {
    bool peerVisible = false;
    Vec3 peerPos;
    float peerDistance = 0;
    bool peerDead = false;
    bool peerInMission = false;
    bool peerInCutscene = false;
    bool holding = false;        // waiting for the friend to finish a cutscene (world frozen)
    bool spectating = false;     // watching the host's cutscene through their camera
    bool missionMismatch = false;
    int saveProgress = -1;
    int syncedEntities = 0;      // host: entities streamed; guest: mirrors shown
    int bufferMs = 0;            // current smoothing delay for the friend's character
};

class CoopSession {
public:
    CoopSession(IGame& game, Link& link, const SessionConfig& config);

    // Call once per game frame.
    void tick(uint32_t nowMs);
    // Removes everything this session created in the game world.
    void cleanup();
    // Moves the local player next to the friend. Returns false if their position is unknown.
    bool teleportToPeer();

    const SessionStatus& status() const { return status_; }
    // Things worth telling the player ("Host started a mission"); returns false when none are left.
    bool popNotice(std::string& out);
    // Shared story: the guest asks the host for their save (stored in a free slot).
    bool requestHostSave() { return story_.requestHostSave(); }
    void setSaveStore(ISaveStore* store) { story_.setSaveStore(store); }

private:
    // Keeps track of how a driven entity was last told to move, so tasks are not re-issued
    // every frame (that would restart animations and cost native calls).
    struct Mover {
        enum Mode { None, Idle, Moving } mode = None;
        uint32_t lastTaskMs = 0;
        uint32_t lastFaceMs = 0;
        Vec3 lastGoal;
        float lastSpeed = 0;
    };

    struct Puppet {
        EntityHandle ped = 0;
        EntityHandle ownMount = 0, ownVehicle = 0;
        uint32_t ownMountModel = 0, ownVehicleModel = 0;
        uint32_t ownMountUnusedMs = 0, ownVehicleUnusedMs = 0;
        EntityHandle ridingOn = 0, seatedIn = 0;
        int seat = -1;
        uint32_t weapon = 0;
        int expectedHealth = 0;
        uint32_t deadSinceMs = 0, lastAimMs = 0;
        bool crouched = false, ragdolled = false, aimingTask = false;
        Mover mover, bodyMover;
        MotionBuffer motion{40, 300};
        PlayerState state;
        bool haveState = false;
        uint32_t outfit = 0, outfitApplied = 0;
    };

    // Host side: an entity of the host's world that is being streamed.
    struct Streamed {
        EntityHandle handle = 0;
        EntityKind kind = EntityKind::Ped;
        uint16_t id = 0;
        EntityState sent;            // last values sent
        uint32_t changedMs[7] = {};  // last change per field group (bit index of EntityFields)
        uint32_t nextDueMs = 0, lastKeyMs = 0, createdMs = 0;
        float guestDistance = 0;
        bool fresh = true;
        bool seen = false;
    };

    // Guest side: copy of a host entity.
    struct Mirror {
        uint16_t id = 0;
        EntityState st;              // accumulated latest values
        bool haveStatic = false, havePos = false, haveVel = false;
        EntityHandle handle = 0;
        uint32_t lastHeardMs = 0;
        MotionBuffer motion{60, 400};
        Mover mover;
        EntityHandle ridingOn = 0, seatedIn = 0;
        int seat = -1;
        uint32_t weapon = 0;
        int expectedHealth = 0;
        uint32_t deadSinceMs = 0, lastShotMs = 0, lastUpdateMs = 0;
        uint32_t outfitApplied = 0;
        uint32_t firstHeardMs = 0;
        bool ragdolled = false;
        bool bound = false;          // shared story: drives the guest's own copy of this mission entity
    };

    // Guest side: mirrors that left the stream, kept invisible for quick reuse.
    struct Parked {
        EntityHandle handle = 0;
        EntityKind kind = EntityKind::Ped;
        uint32_t model = 0;
        uint32_t sinceMs = 0;
    };

    void handleMessages(uint32_t nowMs);
    void onSnapshot(const EntitySnapshot& snap, uint32_t nowMs);
    void sendLocalState(uint32_t nowMs);
    void sendWorld(uint32_t nowMs);
    void streamEntity(Streamed& s, uint32_t nowMs);
    void updatePuppet(uint32_t nowMs, float dt);
    void updateMirrors(uint32_t nowMs, float dt);
    void updateMirrorPed(Mirror& m, uint32_t nowMs, float dt);
    void clearForeign(uint32_t nowMs);
    void restoreWorld();

    void movePed(EntityHandle h, Mover& m, const MotionSample& target, float dt, uint32_t nowMs);
    void steerVehicle(EntityHandle h, Mover& m, const MotionSample& target, float dt);
    void forwardDamage(EntityHandle h, int& expectedHealth, uint16_t target, bool onlyLocalPlayer = false);
    Vec3 refPosition(uint16_t ref, bool& found);
    EntityHandle resolveRef(uint16_t ref);
    uint16_t refOf(EntityHandle h);
    bool isSessionEntity(EntityHandle h) const;
    void sendAppearance(uint32_t nowMs);
    void bindMissionEntities(uint32_t nowMs);
    void followBound(EntityHandle h, const MotionSample& target, float dt);
    void notice(const std::string& text);
    void destroyPuppet();
    void destroyMirror(Mirror& m, bool recycle);
    EntityHandle obtain(EntityKind kind, uint32_t model, const Vec3& pos, float heading);
    bool inPool(EntityHandle h) const;
    void flushPool(uint32_t nowMs, bool all);
    uint16_t allocateId();
    bool isHost() const { return link_.local().role == Role::Host; }

    IGame& game_;
    Link& link_;
    SessionConfig config_;
    SessionStatus status_;
    uint32_t epoch_ = 0;
    bool active_ = false;
    uint32_t lastTickMs_ = 0;

    Puppet puppet_;
    LocalPlayer local_;
    bool haveLocal_ = false;
    uint32_t lastStateMs_ = 0;
    uint16_t lastSentFlags_ = 0;
    bool lastSentMoving_ = false;
    uint32_t lastOutfitCheckMs_ = 0, sentOutfit_ = 0;
    bool appearanceSent_ = false;
    std::vector<std::string> notices_;
    StorySync story_;
    std::vector<MissionEntity> missionLocal_;
    uint32_t lastMissionScanMs_ = 0;
    uint32_t frame_ = 0;
    uint32_t lastWorldMs_ = 0;
    uint32_t lastInfoMs_ = 0;
    uint32_t lastForeignMs_ = 0;

    // host
    std::unordered_map<EntityHandle, Streamed> streamed_;
    std::unordered_map<uint16_t, EntityHandle> handleById_;
    std::vector<std::pair<uint16_t, uint32_t>> removals_;  // id, resend until
    uint16_t nextId_ = 1;
    // guest
    std::unordered_map<uint16_t, Mirror> mirrors_;
    std::unordered_map<EntityHandle, uint16_t> mirrorByHandle_;
    std::unordered_set<EntityHandle> hidden_;
    std::vector<Parked> pool_;
    bool hostWorldSync_ = false;

    // scratch buffers reused every tick (no per-frame allocations)
    std::vector<NearbyEntity> nearby_;
    std::vector<std::pair<float, size_t>> order_;
    std::vector<EntityState> outgoing_;
    std::vector<uint16_t> removedIds_;
    std::vector<std::vector<uint8_t>> packets_;
    std::vector<ForeignEntity> foreign_;
    std::vector<Mirror*> spawnQueue_;
    std::vector<uint32_t> prefetched_;
};

}  // namespace coop
