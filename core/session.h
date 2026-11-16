// A player's side of a co-op session: turns the link to the server + the game interface into
// one shared story world with any number of players.
//
//  * Every player owns their own character; the others show it as a "puppet".
//  * One player's game owns the world (chosen by the server). It streams peds, horses and vehicles
//    near any player; everybody else shows them as "mirrors" and keeps their own population off.
//    Streaming is prioritised by distance to the nearest other player, only changed fields are
//    sent, and idle entities only send a keyframe once a second.
//  * Damage is routed by the server to whoever owns the victim. It is added up per victim and sent
//    at most every 100 ms (fire and poison hurt a little every frame), tagged with the world epoch:
//    entity ids mean nothing in another owner's world.
//  * Control can move across: if someone drives the owner's wagon or rides the owner's horse, the
//    owner moves the real one from that player's stream, and so does everybody else with their
//    copy. A seat in another player's own wagon is referred to by that player, so it stays one
//    wagon in every game, whoever holds the reins; a ride whose original goes away (owner left)
//    stays with everybody sitting on it.
//  * Shared story: each player's own copies of mission NPCs are linked to the owner's.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
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
    bool welcomed = false;
    std::string denied;          // why the server refused us
    uint8_t playerId = 0;
    bool worldOwner = false;
    std::string ownerName;
    int players = 0;             // including us
    StoryMode storyMode = StoryMode::Shared;
    bool campaignHasSave = false;
    // nearest other player
    bool peerVisible = false;
    std::string peerName;
    Vec3 peerPos;
    float peerDistance = 0;
    bool peerDead = false;
    bool peerInMission = false;
    bool peerInCutscene = false; // anyone else in a scene
    int bufferMs = 0;            // smoothing delay for the nearest player's character
    // world and story
    int syncedEntities = 0;      // owner: entities streamed; others: mirrors shown
    bool holding = false;
    bool spectating = false;
    bool missionMismatch = false;
    int saveProgress = -1;
};

class CoopSession {
public:
    CoopSession(IGame& game, Link& link, const SessionConfig& config);

    // Call once per game frame.
    void tick(uint32_t nowMs);
    // Removes everything this session created in the game world.
    void cleanup();
    // Moves the local player next to the world owner (or the nearest player if we own the world).
    bool teleportToPeer();

    const SessionStatus& status() const { return status_; }
    bool popNotice(std::string& out);
    // Downloads the campaign save from the server into a free local slot.
    bool requestCampaignSave() { return story_.requestCampaignSave(); }
    // Puts the latest local save on the server as the campaign save.
    bool uploadSave() { return story_.uploadLatestSave(); }
    void setSaveStore(ISaveStore* store) { story_.setSaveStore(store); }

private:
    struct Mover {
        enum Mode { None, Idle, Settle, Moving } mode = None;
        uint32_t lastTaskMs = 0, settleSinceMs = 0, lastCheckMs = 0;
        Vec3 restPos;           // standing still: the spot and heading of the last check
        float restHeading = 0;
        bool still = false;     // ... and it was on that spot, facing that way
        uint32_t lastFaceMs = 0;
        Vec3 lastGoal;
        float lastSpeed = 0;
    };

    struct Puppet {
        uint8_t id = 0;
        std::string name, avatar;
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

    // World owner: an entity of the local world that is being streamed.
    struct Streamed {
        EntityHandle handle = 0;
        EntityKind kind = EntityKind::Ped;
        uint16_t id = 0;
        EntityState sent;
        uint32_t changedMs[7] = {};
        uint32_t nextDueMs = 0, lastKeyMs = 0, createdMs = 0;
        float viewerDistance = 0;
        bool fresh = true;
        bool seen = false;
    };

    // Everyone else: copy of an entity of the owner's world.
    struct Mirror {
        uint16_t id = 0;
        EntityState st;
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
        bool bound = false;          // shared story: drives our own copy of this mission entity
    };

    struct Parked {
        EntityHandle handle = 0;
        EntityKind kind = EntityKind::Ped;
        uint32_t model = 0;
        uint32_t sinceMs = 0;
    };

    void handleMessages(uint32_t nowMs);
    void onRoster(const Roster& roster);
    void onSnapshot(const EntitySnapshot& snap, uint32_t nowMs);
    void sendLocalState(uint32_t nowMs);
    void sendAppearance(uint32_t nowMs);
    void sendWorld(uint32_t nowMs);
    void streamEntity(Streamed& s, uint32_t nowMs);
    void updatePuppets(uint32_t nowMs, float dt);
    void updatePuppet(Puppet& p, uint32_t nowMs, float dt);
    void updateMirrors(uint32_t nowMs, float dt);
    void updateMirrorPed(Mirror& m, uint32_t nowMs, float dt);
    void bindMissionEntities(uint32_t nowMs);
    void followBound(EntityHandle h, const MotionSample& target, float dt);
    void clearForeign(uint32_t nowMs);
    void resetWorld();
    void restoreWorld();
    void notice(const std::string& text);

    void movePed(EntityHandle h, Mover& m, const MotionBuffer& motion, const MotionSample& target, float dt, uint32_t nowMs);
    void steerVehicle(EntityHandle h, Mover& m, const MotionSample& target, float dt);
    void forwardDamage(EntityHandle h, int& expectedHealth, uint16_t target);
    void flushDamage(uint32_t nowMs);
    Vec3 refPosition(uint16_t ref, bool& found);
    EntityHandle resolveRef(uint16_t ref);
    EntityHandle resolveVehicle(uint16_t ref);
    uint16_t refOf(EntityHandle h);
    bool isSessionEntity(EntityHandle h) const;
    bool inPool(EntityHandle h) const;
    // The local player rides it or drives it.
    bool localRides(EntityHandle h) const {
        return h && (h == local_.mount || (h == local_.vehicle && local_.state.seat == -1));
    }
    // The local player is on it in any seat, passengers included.
    bool localSitsIn(EntityHandle h) const { return h && (h == local_.mount || h == local_.vehicle); }
    // Another player's puppet rides it or sits in it.
    bool puppetUses(EntityHandle h) const {
        for (const auto& entry : puppets_)
            if (h == entry.second.ridingOn || h == entry.second.seatedIn) return true;
        return false;
    }
    bool puppetDrives(EntityHandle h) const { return std::find(driven_.begin(), driven_.end(), h) != driven_.end(); }
    Puppet* puppetByPed(EntityHandle h);
    // forGood: the player left (or we did). Otherwise only the character is recreated in a moment.
    void destroyPuppet(Puppet& p, bool forGood = true);
    void destroyMirror(Mirror& m, bool recycle);
    bool handOverToDriver(Mirror& m);
    void retargetVehicleRefs(uint16_t from, uint16_t to);
    EntityHandle obtain(EntityKind kind, uint32_t model, const Vec3& pos, float heading);
    void flushPool(uint32_t nowMs, bool all);
    EntityHandle takeOverOwnVehicle(uint32_t model, const Vec3& pos, uint16_t id);
    uint16_t allocateId();
    bool isOwner() const { return myId_ != 0 && myId_ == owner_; }

    IGame& game_;
    Link& link_;
    SessionConfig config_;
    SessionStatus status_;
    uint32_t linkEpoch_ = 0;
    bool active_ = false;
    uint32_t lastTickMs_ = 0;
    uint32_t frame_ = 0;

    // who is who (from the server)
    uint8_t myId_ = 0, owner_ = 0;
    uint16_t worldEpoch_ = 0;
    bool haveRoster_ = false;
    StoryMode mode_ = StoryMode::Shared;
    std::map<uint8_t, Puppet> puppets_;

    LocalPlayer local_;
    EntityHandle localPed_ = 0;  // the local player's ped this frame
    EntityHandle ownVehicle_ = 0; // the last vehicle of our own world we sat in
    bool haveLocal_ = false;
    uint32_t lastStateMs_ = 0;
    uint16_t lastSentFlags_ = 0;
    bool lastSentMoving_ = false;
    uint32_t lastMovingMs_ = 0;
    uint32_t lastOutfitCheckMs_ = 0, sentOutfit_ = 0;
    bool appearanceSent_ = false;
    uint32_t lastWorldMs_ = 0, lastInfoMs_ = 0, lastForeignMs_ = 0;
    std::vector<std::string> notices_;
    StorySync story_;
    // Damage not yet sent, per victim (an entity id of the current world, or a player ref).
    struct PendingDamage {
        uint32_t amount = 0;
        uint32_t lastSentMs = 0;
    };
    std::unordered_map<uint16_t, PendingDamage> damage_;

    // world owner
    std::unordered_map<EntityHandle, Streamed> streamed_;
    std::unordered_map<uint16_t, EntityHandle> handleById_;
    std::vector<std::pair<uint16_t, uint32_t>> removals_;
    uint16_t nextId_ = 1;
    // everyone else
    std::unordered_map<uint16_t, Mirror> mirrors_;
    std::unordered_map<EntityHandle, uint16_t> mirrorByHandle_;
    std::unordered_set<EntityHandle> hidden_;
    std::vector<Parked> pool_;
    bool ownerWorldSync_ = false;
    std::vector<MissionEntity> missionLocal_;
    struct BindCandidate {
        float d2;
        Mirror* mirror;
        size_t local;  // into missionLocal_
    };
    std::vector<BindCandidate> bindPairs_;
    uint32_t lastMissionScanMs_ = 0;

    // scratch buffers reused every tick
    std::vector<NearbyEntity> nearby_;
    struct Candidate {
        float rank;            // distance that decides who gets streamed first
        float viewerDistance;  // to the nearest other player
        size_t index;          // into nearby_
    };
    std::vector<Candidate> order_;
    std::vector<Vec3> points_;  // the owner, then every other player
    std::vector<EntityState> outgoing_;
    std::vector<uint16_t> removedIds_;
    std::vector<std::vector<uint8_t>> packets_;
    std::vector<ForeignEntity> foreign_;
    std::vector<Mirror*> spawnQueue_;
    std::vector<uint32_t> prefetched_;
    std::vector<EntityHandle> driven_;  // shared entities other players steer this frame (their puppets move them)
    std::vector<StorySync::Peer> peers_;
    EntitySnapshot snapshot_;           // decoded snapshots reuse its entity list
};

}  // namespace coop
