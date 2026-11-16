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
//    download the campaign save into a free local slot. A transfer cut off by a reconnect is
//    picked up again, and a save the server already has is never sent back. An upload counts as
//    stored only when the server says so (SaveResult); a save the owner made just before quitting
//    goes up when the owner is back.
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
    // What the server holds for the campaign (from Welcome and every Roster): whether it has a save,
    // that save's CRC-32 (0: none) and when the server stored it (Unix seconds, 0: not known).
    void setCampaignSave(bool has, uint32_t crc, uint32_t time = 0) {
        campaignHasSave_ = has;
        campaignSaveCrc_ = crc;
        campaignSaveTime_ = time;
    }

    // Call every frame. `peers` are the other players we know the state of.
    void tick(uint32_t nowMs, const PlayerState& local, const std::vector<Peer>& peers, uint8_t worldOwner, uint8_t me);
    // Story messages: StoryCommand, Camera, Waypoint, SaveChunk, SaveResult. `from` is the sending
    // player for relayed ones.
    void handle(Msg type, uint8_t from, const uint8_t* data, size_t size, uint32_t nowMs);
    // The connection ended: releases everything the story layer holds on to (frozen world, camera,
    // protection, map marker). What outlives a connection is kept: a shared failure still waiting
    // for the end of our scene, what we know about the local saves, transfers to pick up again.
    void reset();

    bool requestCampaignSave();
    bool uploadLatestSave();

    uint16_t extraFlags() const { return worldHeld_ ? PF_Holding : 0; }
    bool holding() const { return worldHeld_; }
    bool spectating() const { return spectating_; }
    // World owner: stream the scene at full rate (someone watches it through the camera).
    bool boostStreaming() const { return boost_; }
    Vec3 viewPoint(const Vec3& localPos) const { return spectating_ ? camPos_ : localPos; }
    // Percent of the running transfer (a download first), -1 when none runs.
    int saveProgress() const;
    bool missionMismatch() const { return mismatch_; }

private:
    void setWorldHeld(bool on);
    void releaseHold(const std::string& why);
    void regroup();
    void startDownload(const std::string& why);
    bool startUpload(bool automatic, const std::string& why);
    void sendSaveChunks();
    void dropUpload();
    void uploadAnswered(const SaveResult& r);
    void receiveSaveChunk(const SaveChunk& c, uint32_t nowMs);

    IGame& game_;
    Link& link_;
    const SessionConfig& config_;
    std::function<void(const std::string&)> notify_;
    ISaveStore* store_ = nullptr;
    StoryMode mode_ = StoryMode::Shared;

    bool first_ = true;
    uint32_t nowMs_ = 0;
    uint16_t prevLocal_ = 0;
    PlayerState local_;
    bool iOwn_ = false;
    uint32_t lastScriptCheckMs_ = 0, sentScript_ = 0;
    bool mismatch_ = false;
    bool protected_ = false;
    // shared failure: carried out after our scene, if we still play the mission it was for
    bool pendingFail_ = false;
    uint32_t failScript_ = 0, failSinceMs_ = 0;
    uint32_t killedAtMs_ = 0;  // when we killed ourselves for a shared failure
    // cutscene barrier: the server wants us to wait (holding_); the world is frozen (worldHeld_)
    bool holding_ = false, worldHeld_ = false;
    uint32_t holdStartMs_ = 0;
    // a move to the mission start, carried out once our own scene is over
    bool regroupPending_ = false;
    Vec3 regroupPos_;
    float regroupDistance_ = 0;
    std::string regroupText_;
    uint8_t worldOwner_ = 0;    // as of the last tick (0: not known yet)
    uint8_t regroupOwner_ = 0;  // whom the regroup is toward: the world owner when it came in
    bool regroupWasInMission_ = false;  // we already played a mission when it came in
    bool regroupDeferred_ = false;      // a scene of ours ran since: its position is out of date
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
    // saves: what we know about the local files
    uint32_t lastStampCheckMs_ = 0;
    uint64_t lastStamp_ = 0;
    bool stampKnown_ = false, seedChecked_ = false;
    bool campaignHasSave_ = true;
    uint32_t campaignSaveCrc_ = 0;
    uint32_t campaignSaveTime_ = 0;
    // the newest file the server and we agree on (sent up, or received): never sent again by itself
    uint32_t uploadedCrc_ = 0, downloadedCrc_ = 0;
    bool haveUploadedCrc_ = false, haveDownloadedCrc_ = false;
    // a file the server refused: not offered again by itself
    uint32_t refusedCrc_ = 0;
    bool haveRefusedCrc_ = false;
    // the stamp of the newest file the server gave an answer for (stored or refused)
    uint64_t answeredStamp_ = 0;
    // upload: queued into the link until sendingSave_ ends, then unconfirmed until the server's
    // SaveResult (or until it is overdue: a server may drop a refused upload without a word)
    std::vector<uint8_t> up_;
    size_t upOffset_ = 0;
    uint32_t upCrc_ = 0;
    uint64_t upStamp_ = 0;
    uint32_t upQueuedMs_ = 0;  // when the last piece went into the link
    bool upAsOwner_ = false;   // started while we owned the world
    bool sendingSave_ = false, uploadUnconfirmed_ = false, uploadAgain_ = false;
    // download
    std::vector<uint8_t> down_;
    uint32_t downTotal_ = 0, downCrc_ = 0, lastChunkMs_ = 0;
    bool receivingSave_ = false, downloadAgain_ = false;
};

}  // namespace coop
