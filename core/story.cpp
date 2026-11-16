#include "story.h"

#include <algorithm>
#include <cmath>

namespace coop {

namespace {
constexpr uint32_t kScriptCheckMs = 1000;
constexpr uint32_t kCameraIntervalMs = 33;
constexpr uint32_t kCameraStaleMs = 600;
constexpr uint32_t kWaypointCheckMs = 500;
constexpr uint32_t kSaveCheckMs = 5000;
constexpr uint32_t kHoldSafetyMs = 15 * 60 * 1000;  // a hold the server forgot about ends eventually
constexpr size_t kSaveWindow = 48;                  // save chunks in flight; leaves room for game traffic

float distance2d(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y); }
}  // namespace

StorySync::StorySync(IGame& game, Link& link, const SessionConfig& config, std::function<void(const std::string&)> notify)
    : game_(game), link_(link), config_(config), notify_(std::move(notify)) {}

void StorySync::reset() {
    if (holding_) game_.holdWorld(false);
    if (spectating_) game_.spectate(false, CameraState{});
    if (protected_) game_.setLocalProtected(false);
    if (friendWaypoint_) game_.showFriendWaypoint(false, Vec3{});
    first_ = true;
    prevLocal_ = 0;
    lastScriptCheckMs_ = sentScript_ = 0;
    mismatch_ = pendingFail_ = causedByServer_ = protected_ = holding_ = false;
    holdStartMs_ = 0;
    camMotion_.clear();
    lastCamMs_ = lastCamSentMs_ = 0;
    camFrom_ = 0;
    spectating_ = boost_ = false;
    lastWaypointCheckMs_ = 0;
    localWaypoint_ = friendWaypoint_ = false;
    lastStampCheckMs_ = 0;
    stampKnown_ = false;
    saveData_.clear();
    saveOffset_ = 0;
    sendingSave_ = receivingSave_ = false;
    saveProgress_ = -1;
}

bool StorySync::requestCampaignSave() {
    if (!link_.connected() || receivingSave_) return false;
    link_.send(Msg::SaveRequest, SaveRequest{}, true);
    receivingSave_ = true;
    saveProgress_ = 0;
    saveData_.clear();
    saveOffset_ = 0;
    notify_("Downloading the campaign save from the server...");
    return true;
}

bool StorySync::uploadLatestSave() {
    if (!link_.connected() || sendingSave_ || !store_) return false;
    std::string name;
    if (!store_->readLatest(saveData_, name) || saveData_.empty() || saveData_.size() > kMaxSaveBytes) {
        saveData_.clear();
        notify_("No local save found to upload");
        return false;
    }
    saveCrc_ = crc32(saveData_.data(), saveData_.size());
    saveOffset_ = 0;
    sendingSave_ = true;
    saveProgress_ = 0;
    notify_("Uploading your save (" + name + ") to the server...");
    sendSaveChunks();
    return true;
}

void StorySync::releaseHold(const std::string& why) {
    if (!holding_) return;
    holding_ = false;
    game_.holdWorld(false);
    if (!why.empty()) notify_(why);
}

void StorySync::tick(uint32_t nowMs, const PlayerState& local, const std::vector<Peer>& peers, uint8_t worldOwner,
                     uint8_t me) {
    local_ = local;
    const uint16_t lf = local.flags;
    const bool localMission = lf & PF_InMission, localCut = lf & PF_Cutscene, localDead = lf & PF_Dead;
    const bool iOwn = worldOwner == me;
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
        if (causedByServer_) causedByServer_ = false;  // that was the shared failure itself
        else link_.send(Msg::Story, StoryInfo{StoryEvent::Died, sentScript_}, true);
    }
    if (pendingFail_ && !localCut) {
        pendingFail_ = false;
        if (localMission && !localDead) {
            releaseHold("");
            causedByServer_ = true;
            game_.killLocalPlayer();
        }
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

    // --- campaign saves: every new save of the world owner goes to the server
    if (store_ && iOwn && config_.autoUploadSaves && (!stampKnown_ || nowMs - lastStampCheckMs_ >= kSaveCheckMs)) {
        lastStampCheckMs_ = nowMs;
        uint64_t stamp = store_->latestStamp();
        bool fresh = stampKnown_ && stamp && stamp != lastStamp_;
        bool seed = !stampKnown_ && stamp && !campaignHasSave_;  // a new campaign starts from our save
        if ((fresh || seed) && !sendingSave_ && !receivingSave_) {
            if (fresh) notify_("New save detected");
            if (uploadLatestSave()) campaignHasSave_ = true;
        }
        lastStamp_ = stamp;
        stampKnown_ = true;
    }
    if (sendingSave_) sendSaveChunks();
    if (holding_ && nowMs - holdStartMs_ > kHoldSafetyMs) releaseHold("Stopped waiting");

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
                        holding_ = true;
                        holdStartMs_ = nowMs;
                        game_.holdWorld(true);
                    }
                    if (!cmd.text.empty()) notify_(cmd.text);
                    break;
                case StoryCmd::Release:
                    releaseHold(cmd.text);
                    break;
                case StoryCmd::Fail:
                    if (!cmd.text.empty()) notify_(cmd.text);
                    pendingFail_ = true;  // carried out by tick(), after any running scene
                    break;
                case StoryCmd::Regroup: {
                    bool far = distance(local_.pos, cmd.pos) > cmd.distance;
                    if (config_.autoRegroup && far && !(local_.flags & PF_Dead)) {
                        Vec3 target = cmd.pos;
                        target.x += 1.5f;
                        game_.teleportLocalPlayer(target);
                        local_.pos = target;
                        notify_(cmd.text.empty() ? std::string("Moved you to the world owner") : cmd.text);
                    } else if (!cmd.text.empty()) {
                        notify_(cmd.text);
                    }
                    break;
                }
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
            if (receivingSave_ && decode(data, size, c)) receiveSaveChunk(c);
            return;
        }
        default:
            return;
    }
}

void StorySync::sendSaveChunks() {
    while (sendingSave_ && link_.pendingReliable() < kSaveWindow) {
        SaveChunk c;
        c.total = static_cast<uint32_t>(saveData_.size());
        c.crc = saveCrc_;
        c.offset = static_cast<uint32_t>(saveOffset_);
        size_t n = std::min(kSaveChunkBytes, saveData_.size() - saveOffset_);
        c.data.assign(saveData_.begin() + static_cast<std::ptrdiff_t>(saveOffset_),
                      saveData_.begin() + static_cast<std::ptrdiff_t>(saveOffset_ + n));
        link_.send(Msg::SaveChunk, c, true);
        saveOffset_ += n;
        saveProgress_ = static_cast<int>(saveOffset_ * 100 / saveData_.size());
        if (saveOffset_ >= saveData_.size()) {
            sendingSave_ = false;
            saveProgress_ = -1;
            saveData_.clear();
            saveData_.shrink_to_fit();
        }
    }
}

void StorySync::receiveSaveChunk(const SaveChunk& c) {
    if (c.total == 0) {
        notify_("The server has no campaign save yet - the world owner saves once to create it");
        receivingSave_ = false;
        saveProgress_ = -1;
        return;
    }
    if (c.offset == 0) {
        saveData_.assign(c.total, 0);
        saveCrc_ = c.crc;
        saveOffset_ = 0;
    }
    // The channel is reliable and ordered, so pieces arrive one after another.
    if (c.offset != saveOffset_ || c.total != saveData_.size() || c.crc != saveCrc_) return;
    std::copy(c.data.begin(), c.data.end(), saveData_.begin() + static_cast<std::ptrdiff_t>(c.offset));
    saveOffset_ += c.data.size();
    saveProgress_ = static_cast<int>(saveOffset_ * 100 / saveData_.size());
    if (saveOffset_ < saveData_.size()) return;

    receivingSave_ = false;
    saveProgress_ = -1;
    std::string placed, error;
    if (crc32(saveData_.data(), saveData_.size()) != saveCrc_) {
        notify_("The save arrived damaged - try again");
    } else if (!store_ || !store_->writeCopy(saveData_, placed, error)) {
        notify_("Could not store the save: " + (error.empty() ? std::string("no save folder") : error));
    } else {
        notify_("Campaign save stored as " + placed + ". Load it: pause menu - Story - Load Game");
        if (store_) lastStamp_ = store_->latestStamp();  // not a new save of ours: do not upload it back
    }
    saveData_.clear();
    saveData_.shrink_to_fit();
}

}  // namespace coop
