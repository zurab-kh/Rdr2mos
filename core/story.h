// Story synchronisation, player side. The server makes the decisions (see server.cpp); this
// part reports what happens in our game and carries out what the server asks for.
//
//  * reports which mission we play and our deaths in missions;
//  * obeys the server: hold (cutscene barrier: world frozen until the others are done), release,
//    fail the mission (after any running scene), regroup at the mission start, notices;
//  * host world: watches the world owner's scenes through the owner's camera;
//  * protected while another player is in a scene;
//  * shares map waypoints;
//  * campaign saves: the world owner's new saves go to the server automatically, and anyone can
//    download the campaign save into a free local slot.
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
    struct Peer {
        uint8_t id = 0;
        uint16_t flags = 0;
        Vec3 pos;
    };

    StorySync(IGame& game, Link& link, const SessionConfig& config, std::function<void(const std::string&)> notify);

    void setSaveStore(ISaveStore* store) { store_ = store; }
    void setMode(StoryMode mode) { mode_ = mode; }
    void setCampaignHasSave(bool has) { campaignHasSave_ = has; }

    // Call every frame. `peers` are the other players we know the state of.
    void tick(uint32_t nowMs, const PlayerState& local, const std::vector<Peer>& peers, uint8_t worldOwner, uint8_t me);
    // Story messages: StoryCommand, Camera, Waypoint, SaveChunk. `from` is the sending player for relayed ones.
    void handle(Msg type, uint8_t from, const uint8_t* data, size_t size, uint32_t nowMs);
    // Releases everything the story layer holds on to (frozen world, camera, protection, map marker).
    void reset();

    bool requestCampaignSave();
    bool uploadLatestSave();

    uint16_t extraFlags() const { return holding_ ? PF_Holding : 0; }
    bool holding() const { return holding_; }
    bool spectating() const { return spectating_; }
    // World owner: stream the scene at full rate (someone watches it through the camera).
    bool boostStreaming() const { return boost_; }
    Vec3 viewPoint(const Vec3& localPos) const { return spectating_ ? camPos_ : localPos; }
    int saveProgress() const { return saveProgress_; }
    bool missionMismatch() const { return mismatch_; }

private:
    void releaseHold(const std::string& why);
    void sendSaveChunks();
    void receiveSaveChunk(const SaveChunk& c);

    IGame& game_;
    Link& link_;
    const SessionConfig& config_;
    std::function<void(const std::string&)> notify_;
    ISaveStore* store_ = nullptr;
    StoryMode mode_ = StoryMode::Shared;

    bool first_ = true;
    uint16_t prevLocal_ = 0;
    PlayerState local_;
    uint32_t lastScriptCheckMs_ = 0, sentScript_ = 0;
    bool mismatch_ = false;
    bool pendingFail_ = false, causedByServer_ = false;
    bool protected_ = false;
    bool holding_ = false;
    uint32_t holdStartMs_ = 0;
    // spectator camera
    MotionBuffer camMotion_{60, 250};
    float camFov_ = 50.0f;
    uint32_t lastCamMs_ = 0, lastCamSentMs_ = 0;
    uint8_t camFrom_ = 0;
    bool spectating_ = false, boost_ = false;
    Vec3 camPos_;
    // waypoints
    uint32_t lastWaypointCheckMs_ = 0;
    bool localWaypoint_ = false, friendWaypoint_ = false;
    Vec3 localWaypointPos_, friendWaypointPos_;
    // saves
    uint32_t lastStampCheckMs_ = 0;
    uint64_t lastStamp_ = 0;
    bool stampKnown_ = false;
    bool campaignHasSave_ = true;
    std::vector<uint8_t> saveData_;
    size_t saveOffset_ = 0;
    uint32_t saveCrc_ = 0;
    bool sendingSave_ = false, receivingSave_ = false;
    int saveProgress_ = -1;
};

}  // namespace coop
