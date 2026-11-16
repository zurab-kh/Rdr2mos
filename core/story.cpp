#include "story.h"

#include <algorithm>
#include <cmath>

namespace coop {

namespace {
constexpr uint32_t kScriptCheckMs = 250;                // the server compares missions: report changes quickly
constexpr uint32_t kCameraIntervalMs = 33;
constexpr uint32_t kCameraStaleMs = 600;
constexpr uint32_t kWaypointCheckMs = 500;
constexpr uint32_t kSaveCheckMs = 5000;
constexpr uint32_t kHoldSafetyMs = 15 * 60 * 1000;      // a hold the server forgot about ends eventually
constexpr uint32_t kOwnKillMs = 5000;                   // a death this soon after we killed ourselves is that kill
constexpr uint32_t kPendingFailMs = 10 * 60 * 1000;     // longer than any scene: a failure this old is stale
constexpr uint32_t kDownloadStallMs = 30000;
constexpr size_t kSaveWindow = 48;                      // save chunks in flight; leaves room for game traffic

float distance2d(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y); }
}  // namespace

StorySync::StorySync(IGame& game, Link& link, const SessionConfig& config, std::function<void(const std::string&)> notify)
    : game_(game), link_(link), config_(config), notify_(std::move(notify)) {}

void StorySync::reset() {
    setWorldHeld(false);
    if (spectating_) game_.spectate(false, CameraState{});
    if (protected_) game_.setLocalProtected(false);
    if (friendWaypoint_) game_.showFriendWaypoint(false, Vec3{});
    first_ = true;
    prevLocal_ = 0;
    iOwn_ = false;
    lastScriptCheckMs_ = sentScript_ = 0;
    mismatch_ = protected_ = holding_ = false;
    holdStartMs_ = 0;
    regroupPending_ = false;
    // pendingFail_ stays: the others restart the mission whether or not our connection blipped
    // meanwhile. tick() drops it if the mission ends first.
    camMotion_.clear();
    lastCamMs_ = lastCamSentMs_ = 0;
    camFrom_ = 0;
    spectating_ = boost_ = false;
    lastWaypointCheckMs_ = 0;
    localWaypoint_ = friendWaypoint_ = false;

    // The local save files are what they were; the next Welcome says again whether the campaign
    // has a save, so the "new campaign starts from our save" decision is made again.
    lastStampCheckMs_ = 0;
    seedChecked_ = false;
    if (sendingSave_ || uploadUnconfirmed_) uploadAgain_ = true;
    if (receivingSave_) downloadAgain_ = true;
    up_.clear();
    up_.shrink_to_fit();
    down_.clear();
    down_.shrink_to_fit();
    upOffset_ = 0;
    downTotal_ = 0;
    sendingSave_ = uploadUnconfirmed_ = receivingSave_ = false;
}

int StorySync::saveProgress() const {
    if (receivingSave_) return downTotal_ ? static_cast<int>(uint64_t(down_.size()) * 100 / downTotal_) : 0;
    if (sendingSave_) return up_.empty() ? 0 : static_cast<int>(uint64_t(upOffset_) * 100 / up_.size());
    return -1;
}

bool StorySync::requestCampaignSave() {
    if (!link_.connected() || receivingSave_) return false;
    startDownload("Downloading the campaign save from the server...");
    return true;
}

void StorySync::startDownload(const std::string& why) {
    link_.send(Msg::SaveRequest, SaveRequest{}, true);
    receivingSave_ = true;
    downloadAgain_ = false;
    down_.clear();
    downTotal_ = downCrc_ = 0;
    lastChunkMs_ = 0;  // the stall clock starts on the next tick
    notify_(why);
}

bool StorySync::uploadLatestSave() {
    if (!link_.connected() || !store_) return false;
    if (sendingSave_) {
        notify_("Your save is already being uploaded");
        return false;
    }
    // The server takes the campaign save only from the world owner (or from anyone, to start a campaign).
    if (!iOwn_ && campaignHasSave_) {
        notify_("Only the world owner can update the campaign save - download it with F10 instead");
        return false;
    }
    return startUpload(false, "");
}

bool StorySync::startUpload(bool automatic, const std::string& why) {
    std::vector<uint8_t> data;
    std::string name;
    if (!store_ || !store_->readLatest(data, name) || data.empty()) {
        if (!automatic) notify_("No local save found to upload");
        return false;
    }
    if (data.size() > kMaxSaveBytes) {
        notify_("Your save " + name + " is too large to upload (" + std::to_string(data.size() >> 20) + " MB)");
        return false;
    }
    uint32_t crc = crc32(data.data(), data.size());
    // A file the server already has: our last upload, or the campaign save we got from it (its copy
    // becomes our newest file, and the game touches it again when it is loaded).
    if (automatic && ((haveUploadedCrc_ && crc == uploadedCrc_) || (haveDownloadedCrc_ && crc == downloadedCrc_)))
        return false;
    up_ = std::move(data);
    upCrc_ = crc;
    upOffset_ = 0;
    sendingSave_ = uploadUnconfirmed_ = true;
    uploadAgain_ = false;
    notify_(why.empty() ? "Uploading your save (" + name + ") to the server..." : why);
    sendSaveChunks();
    return true;
}

void StorySync::setWorldHeld(bool on) {
    if (on == worldHeld_) return;
    worldHeld_ = on;
    game_.holdWorld(on);
}

void StorySync::releaseHold(const std::string& why) {
    if (!holding_) return;
    holding_ = false;  // tick() lets the world go (in the same frame)
    if (!why.empty()) notify_(why);
}

void StorySync::regroup() {
    bool far = distance(local_.pos, regroupPos_) > regroupDistance_;
    if (config_.autoRegroup && far && !(local_.flags & PF_Dead)) {
        Vec3 target = regroupPos_;
        target.x += 1.5f;
        game_.teleportLocalPlayer(target);
        local_.pos = target;
        notify_(regroupText_.empty() ? std::string("Moved you to the world owner") : regroupText_);
    } else if (!regroupText_.empty()) {
        notify_(regroupText_);
    }
}

void StorySync::tick(uint32_t nowMs, const PlayerState& local, const std::vector<Peer>& peers, uint8_t worldOwner,
                     uint8_t me) {
    nowMs_ = nowMs;
    local_ = local;
    const uint16_t lf = local.flags;
    const bool localMission = lf & PF_InMission, localCut = lf & PF_Cutscene, localDead = lf & PF_Dead;
    const bool iOwn = worldOwner == me;
    iOwn_ = iOwn;
    if (first_) {
        prevLocal_ = lf;  // a state that already holds is not an event
        first_ = false;
    }

    // --- which mission we play
    bool missionEdge = localMission != ((prevLocal_ & PF_InMission) != 0);
    if (missionEdge || nowMs - lastScriptCheckMs_ >= kScriptCheckMs) {
        lastScriptCheckMs_ = nowMs;
        uint32_t script = localMission ? game_.localMissionScript() : 0;
        if (script != sentScript_ || missionEdge) {
            link_.send(Msg::Story, StoryInfo{StoryEvent::Mission, script}, true);
            sentScript_ = script;
        }
    }

    // --- our deaths in missions (the server fails the mission for the others)
    if (localDead && !(prevLocal_ & PF_Dead) && (localMission || (prevLocal_ & PF_InMission))) {
        // Our own kill for a shared failure is not passed on. Only right after it, though: if the game
        // refused that kill, a real death later on must still count.
        bool ownKill = killedAtMs_ && nowMs - killedAtMs_ < kOwnKillMs;
        killedAtMs_ = 0;
        if (!ownKill) link_.send(Msg::Story, StoryInfo{StoryEvent::Died, sentScript_}, true);
    }
    if (pendingFail_ && nowMs - failSinceMs_ > kPendingFailMs) pendingFail_ = false;
    if (pendingFail_ && !localCut) {
        pendingFail_ = false;
        bool sameMission = !failScript_ || !sentScript_ || failScript_ == sentScript_;
        if (localMission && !localDead && sameMission) {
            holding_ = false;
            setWorldHeld(false);
            killedAtMs_ = nowMs ? nowMs : 1;
            game_.killLocalPlayer();
        }
    }

    // --- cutscene barrier: the server says when to wait, the world is frozen here. Never during a
    // scene of our own (a hold that comes in then starts after it), and the wait ends with our
    // mission or our death without waiting for the server to notice.
    if (holding_ && (!localMission || localDead)) holding_ = false;
    if (holding_ && nowMs - holdStartMs_ > kHoldSafetyMs) releaseHold("Stopped waiting");
    setWorldHeld(holding_ && (worldHeld_ || !localCut));

    // --- move to the mission start, once our own scene is over
    if (regroupPending_ && !localCut) {
        regroupPending_ = false;
        regroup();
    }

    // --- protection: nobody can help us while the others are in a scene
    bool othersInScene = std::any_of(peers.begin(), peers.end(), [](const Peer& p) { return p.flags & PF_Cutscene; });
    bool protect = othersInScene && !localCut;
    if (protect != protected_) {
        game_.setLocalProtected(protect);
        protected_ = protect;
    }

    // --- host world: the owner's scenes, seen through the owner's camera
    boost_ = false;
    if (mode_ == StoryMode::HostWorld && config_.spectate) {
        if (iOwn && localCut) {
            boost_ = true;
            if (nowMs - lastCamSentMs_ >= kCameraIntervalMs) {
                CameraState cam;
                if (game_.readCamera(cam)) {
                    cam.timeMs = nowMs;
                    link_.send(Msg::Camera, cam, false);
                }
                lastCamSentMs_ = nowMs;
            }
        }
        bool ownerInScene = false;
        for (const Peer& p : peers)
            if (p.id == worldOwner && (p.flags & PF_Cutscene)) ownerInScene = true;
        bool want = !iOwn && ownerInScene && camFrom_ == worldOwner && lastCamMs_ && nowMs - lastCamMs_ < kCameraStaleMs &&
                    !camMotion_.empty();
        if (want) {
            MotionSample m = camMotion_.sample(nowMs);
            CameraState cam;
            cam.pos = m.pos;
            cam.rot = {m.pitch, m.roll, m.heading};
            cam.fov = camFov_;
            camPos_ = m.pos;
            game_.spectate(true, cam);
            if (!spectating_) notify_("Watching the world owner's cutscene");
            spectating_ = true;
        } else if (spectating_) {
            game_.spectate(false, CameraState{});
            spectating_ = false;
            camMotion_.clear();
        }
    }

    // --- waypoints
    if (config_.syncWaypoints && nowMs - lastWaypointCheckMs_ >= kWaypointCheckMs) {
        lastWaypointCheckMs_ = nowMs;
        Vec3 wp;
        bool active = game_.readWaypoint(wp);
        bool changed = active != localWaypoint_ || (active && distance2d(wp, localWaypointPos_) > 2.0f);
        if (changed) {
            localWaypoint_ = active;
            localWaypointPos_ = wp;
            bool echo = active && friendWaypoint_ && distance2d(wp, friendWaypointPos_) < 2.0f;
            if (!echo) link_.send(Msg::Waypoint, WaypointInfo{static_cast<uint8_t>(active), wp.x, wp.y}, true);
        }
    }

    // --- campaign saves
    if ((sendingSave_ || uploadUnconfirmed_) && !iOwn && campaignHasSave_) {
        // The world changed hands: the server takes the campaign save from the new owner only, so
        // pieces still on their way are dropped there.
        if (sendingSave_) notify_("The world now belongs to another player - your save was not uploaded");
        sendingSave_ = uploadUnconfirmed_ = false;
        up_.clear();
        up_.shrink_to_fit();
    }
    if (uploadUnconfirmed_ && !sendingSave_ && link_.pendingReliable() == 0) {
        // Every piece is acknowledged: the server has the file.
        uploadUnconfirmed_ = false;
        uploadedCrc_ = upCrc_;
        haveUploadedCrc_ = true;
        campaignHasSave_ = true;
    }
    if (uploadAgain_ && !sendingSave_) {
        // An upload the connection cut off.
        uploadAgain_ = false;
        if (iOwn || !campaignHasSave_) startUpload(true, "Uploading your save again (the connection was interrupted)...");
        else notify_("Your save upload was cut off and the world now belongs to another player");
    }
    if (downloadAgain_ && !receivingSave_) startDownload("Resuming the campaign save download...");
    if (receivingSave_) {
        if (!lastChunkMs_) {
            lastChunkMs_ = nowMs ? nowMs : 1;
        } else if (nowMs - lastChunkMs_ > kDownloadStallMs) {
            receivingSave_ = false;
            down_.clear();
            down_.shrink_to_fit();
            notify_("The campaign save download stopped - press the download key to try again");
        }
    }
    // Every new save of the world owner goes to the server. Everybody keeps track of their save
    // files, so a player who takes over the world later only sends saves made from then on.
    if (store_ && (!seedChecked_ || nowMs - lastStampCheckMs_ >= kSaveCheckMs)) {
        lastStampCheckMs_ = nowMs;
        uint64_t stamp = store_->latestStamp();
        bool seed = false;
        if (!seedChecked_) {
            // A new campaign starts from the save of the player who owns the world when we arrive.
            seedChecked_ = true;
            seed = iOwn && !campaignHasSave_ && stamp;
        }
        bool fresh = stampKnown_ && stamp && stamp != lastStamp_;
        bool upload = config_.autoUploadSaves && iOwn && (fresh || seed);
        if (upload && sendingSave_) {
            if (seed) seedChecked_ = false;  // busy: this save goes up on a later check
        } else {
            if (upload) startUpload(true, fresh ? "New save detected - uploading it to the server..." : "");
            lastStamp_ = stamp;
            stampKnown_ = true;
        }
    }
    if (sendingSave_) sendSaveChunks();

    prevLocal_ = lf;
}

void StorySync::handle(Msg type, uint8_t from, const uint8_t* data, size_t size, uint32_t nowMs) {
    switch (type) {
        case Msg::StoryCommand: {
            StoryCommandMsg cmd;
            if (!decode(data, size, cmd)) return;
            switch (cmd.cmd) {
                case StoryCmd::Hold:
                    if (!holding_) {
                        holding_ = true;  // tick() freezes the world, in this same frame
                        holdStartMs_ = nowMs;
                    }
                    if (!cmd.text.empty()) notify_(cmd.text);
                    break;
                case StoryCmd::Release:
                    releaseHold(cmd.text);
                    break;
                case StoryCmd::Fail:
                    if (!cmd.text.empty()) notify_(cmd.text);
                    pendingFail_ = true;  // carried out by tick(), after any running scene
                    failScript_ = sentScript_;
                    failSinceMs_ = nowMs;
                    break;
                case StoryCmd::Regroup:
                    regroupPending_ = true;  // carried out by tick(), after any running scene
                    regroupPos_ = cmd.pos;
                    regroupDistance_ = cmd.distance;
                    regroupText_ = cmd.text;
                    break;
                case StoryCmd::Notice:
                    notify_(cmd.text);
                    break;
                case StoryCmd::Mismatch:
                    mismatch_ = cmd.flag != 0;
                    if (mismatch_ && !cmd.text.empty()) notify_(cmd.text);
                    break;
            }
            return;
        }
        case Msg::Camera: {
            CameraState cam;
            if (!decode(data, size, cam)) return;
            if (camFrom_ != from) camMotion_.clear();
            camFrom_ = from;
            MotionSample s;
            s.timeMs = cam.timeMs;
            s.pos = cam.pos;
            s.heading = cam.rot.z;
            s.pitch = cam.rot.x;
            s.roll = cam.rot.y;
            camMotion_.push(s, nowMs);
            camFov_ = cam.fov;
            lastCamMs_ = nowMs ? nowMs : 1;
            return;
        }
        case Msg::Waypoint: {
            WaypointInfo w;
            if (!decode(data, size, w)) return;
            friendWaypoint_ = w.active != 0;
            friendWaypointPos_ = {w.x, w.y, 0};
            if (config_.syncWaypoints) game_.showFriendWaypoint(friendWaypoint_, friendWaypointPos_);
            return;
        }
        case Msg::SaveChunk: {
            SaveChunk c;
            if (receivingSave_ && decode(data, size, c)) receiveSaveChunk(c, nowMs);
            return;
        }
        default:
            return;
    }
}

void StorySync::sendSaveChunks() {
    while (sendingSave_ && link_.pendingReliable() < kSaveWindow) {
        SaveChunk c;
        c.total = static_cast<uint32_t>(up_.size());
        c.crc = upCrc_;
        c.offset = static_cast<uint32_t>(upOffset_);
        size_t n = std::min(kSaveChunkBytes, up_.size() - upOffset_);
        c.data.assign(up_.begin() + static_cast<std::ptrdiff_t>(upOffset_),
                      up_.begin() + static_cast<std::ptrdiff_t>(upOffset_ + n));
        link_.send(Msg::SaveChunk, c, true);
        upOffset_ += n;
        if (upOffset_ >= up_.size()) {
            sendingSave_ = false;  // uploadUnconfirmed_ until the server has acknowledged every piece
            up_.clear();
            up_.shrink_to_fit();
        }
    }
}

void StorySync::receiveSaveChunk(const SaveChunk& c, uint32_t nowMs) {
    lastChunkMs_ = nowMs ? nowMs : 1;
    if (c.total == 0) {
        notify_("The server has no campaign save yet - the world owner saves once to create it");
        receivingSave_ = false;
        return;
    }
    if (c.offset == 0) {
        down_.clear();
        down_.reserve(c.total);  // decode() bounds it (kMaxSaveBytes)
        downTotal_ = c.total;
        downCrc_ = c.crc;
    }
    // The channel is reliable and ordered, so pieces arrive one after another.
    if (c.offset != down_.size() || c.total != downTotal_ || c.crc != downCrc_) return;
    down_.insert(down_.end(), c.data.begin(), c.data.end());
    if (down_.size() < downTotal_) return;

    receivingSave_ = false;
    std::string placed, error;
    if (crc32(down_.data(), down_.size()) != downCrc_) {
        notify_("The save arrived damaged - try again");
    } else if (!store_ || !store_->writeCopy(down_, placed, error)) {
        notify_("Could not store the save: " + (error.empty() ? std::string("no save folder") : error));
    } else {
        notify_("Campaign save stored as " + placed + ". Load it: pause menu - Story - Load Game");
        downloadedCrc_ = downCrc_;
        haveDownloadedCrc_ = true;
        campaignHasSave_ = true;
    }
    down_.clear();
    down_.shrink_to_fit();
}

}  // namespace coop
