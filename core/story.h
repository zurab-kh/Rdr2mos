// Story synchronisation: keeps two story playthroughs in step.
//
// Shared story (both play the same save):
//  * the guest is brought to where the host started a mission and told to start it too;
//  * cutscene barrier: both watch their own (real) cutscene; whoever finishes first has their world
//    frozen until the other is done, so gameplay resumes together;
//  * if one player dies in a mission, the other's mission is failed too and both retry the checkpoint;
//  * the mission each side plays is compared, so a mismatch is reported instead of silently diverging;
//  * the host's save can be copied to the guest with one key.
// Host world (the guest visits the host's world):
//  * the guest watches the host's scenes through the host's camera, protected meanwhile;
//  * the guest is regrouped with the host when a mission starts or a scene ends far away.
// Both: map waypoints are shared.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "game_api.h"
#include "interp.h"
#include "link.h"
#include "session_config.h"

namespace coop {

class StorySync {
public:
    StorySync(IGame& game, Link& link, const SessionConfig& config, std::function<void(const std::string&)> notify);

    void setSaveStore(ISaveStore* store) { store_ = store; }

    // Call every frame after the friend's state was updated. `peer` is null until it is known.
    void tick(uint32_t nowMs, const PlayerState& local, const PlayerState* peer, float peerDistance);
    // Returns true if the message was a story message (and handled).
    bool handle(Msg type, const std::vector<uint8_t>& payload, uint32_t nowMs);
    // Releases everything the story layer holds on to (frozen world, camera, protection, map marker).
    void reset();

    // Guest: ask for the host's save. Returns false if not possible right now.
    bool requestHostSave();

    uint16_t extraFlags() const { return holding_ ? PF_Holding : 0; }
    bool holding() const { return holding_; }
    bool spectating() const { return spectating_; }
    // Host: stream the scene around the host at full rate (the guest is watching through the camera).
    bool boostStreaming() const { return boost_; }
    // Where the local player is looking from (camera while spectating), for level of detail.
    Vec3 viewPoint(const Vec3& localPos) const { return spectating_ ? camPos_ : localPos; }
    int saveProgress() const { return saveProgress_; }  // -1 = no transfer, 0..100
    bool missionMismatch() const { return mismatch_; }

private:
    bool isHost() const { return link_.local().role == Role::Host; }
    bool shared() const { return config_.storyMode == StoryMode::Shared; }
    void startHold(uint32_t nowMs, const char* why);
    void releaseHold(const char* why);
    void sendSaveChunks();
    void receiveSaveChunk(const SaveChunk& c);

    IGame& game_;
    Link& link_;
    const SessionConfig& config_;
    std::function<void(const std::string&)> notify_;
    ISaveStore* store_ = nullptr;

    bool first_ = true;
    uint16_t prevLocal_ = 0, prevPeer_ = 0;
    uint32_t localMissionStartMs_ = 0;

    // mission identity
    uint32_t lastScriptCheckMs_ = 0, sentScript_ = 0, peerScript_ = 0;
    bool mismatch_ = false;
    // deaths
    uint32_t lastOwnDeathMs_ = 0;
    bool haveDied_ = false;
    bool pendingFail_ = false;     // friend died while we watch a scene: fail once it is over
    bool causedByFriend_ = false;  // our next death is the friend's mission failure: do not report it back
    // protection / regroup
    bool protected_ = false;
    bool regroupPending_ = false, regroupToStart_ = false;
    Vec3 missionStartPos_;
    uint32_t joinHintMs_ = 0;
    // barrier
    bool holding_ = false;
    uint32_t holdStartMs_ = 0, peerReadySinceMs_ = 0;
    // spectator camera
    MotionBuffer camMotion_{60, 250};
    float camFov_ = 50.0f;
    uint32_t lastCamMs_ = 0, lastCamSentMs_ = 0;
    bool spectating_ = false, boost_ = false;
    Vec3 camPos_;
    // waypoints
    uint32_t lastWaypointCheckMs_ = 0;
    bool localWaypoint_ = false, friendWaypoint_ = false;
    Vec3 localWaypointPos_, friendWaypointPos_;
    // save transfer
    std::vector<uint8_t> saveData_;
    size_t saveOffset_ = 0;
    uint32_t saveCrc_ = 0;
    bool sendingSave_ = false, receivingSave_ = false;
    int saveProgress_ = -1;
};

}  // namespace coop
