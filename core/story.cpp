#include "story.h"

#include <algorithm>
#include <cmath>

namespace coop {

namespace {
constexpr uint32_t kScriptCheckMs = 1000;
constexpr uint32_t kBothDiedMs = 3000;         // both died at about the same time: nothing to pass on
constexpr uint32_t kJoinWaitMs = 45000;        // barrier: how long to wait for a friend who has not started the mission
constexpr uint32_t kPeerSettleMs = 1500;       // friend must look "out of the scene" this long before release
constexpr uint32_t kJoinHintMs = 25000;
constexpr uint32_t kCameraIntervalMs = 33;
constexpr uint32_t kCameraStaleMs = 600;
constexpr uint32_t kWaypointCheckMs = 500;
constexpr size_t kSaveWindow = 24;             // save chunks in flight; leaves room for game traffic
constexpr float kMissionJoinDistance = 15.0f;

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
    prevLocal_ = prevPeer_ = 0;
    localMissionStartMs_ = 0;
    lastScriptCheckMs_ = sentScript_ = peerScript_ = 0;
    mismatch_ = false;
    lastOwnDeathMs_ = 0;
    haveDied_ = false;
    pendingFail_ = false;
    causedByFriend_ = false;
    protected_ = false;
    regroupPending_ = regroupToStart_ = false;
    joinHintMs_ = 0;
    holding_ = false;
    holdStartMs_ = peerReadySinceMs_ = 0;
    camMotion_.clear();
    lastCamMs_ = lastCamSentMs_ = 0;
    spectating_ = boost_ = false;
    lastWaypointCheckMs_ = 0;
    localWaypoint_ = friendWaypoint_ = false;
    saveData_.clear();
    saveOffset_ = 0;
    sendingSave_ = receivingSave_ = false;
    saveProgress_ = -1;
}

bool StorySync::requestHostSave() {
    if (isHost() || !link_.connected() || receivingSave_) return false;
    link_.send(Msg::SaveRequest, SaveRequest{}, true);
    receivingSave_ = true;
    saveProgress_ = 0;
    saveData_.clear();
    saveOffset_ = 0;
    notify_("Asking " + link_.peer().name + " for their save...");
    return true;
}

void StorySync::startHold(uint32_t nowMs, const char* why) {
    holding_ = true;
    holdStartMs_ = nowMs;
    peerReadySinceMs_ = 0;
    game_.holdWorld(true);
    notify_(why);
}

void StorySync::releaseHold(const char* why) {
    if (!holding_) return;
    holding_ = false;
    game_.holdWorld(false);
    if (why) notify_(why);
}

void StorySync::tick(uint32_t nowMs, const PlayerState& local, const PlayerState* peer, float peerDistance) {
    const uint16_t lf = local.flags, pf = peer ? peer->flags : 0;
    const bool localMission = lf & PF_InMission, localCut = lf & PF_Cutscene, localDead = lf & PF_Dead;
    const bool peerMission = pf & PF_InMission, peerCut = pf & PF_Cutscene, peerDead = pf & PF_Dead;
    const std::string& friendName = link_.peer().name;
    if (first_) {
        // Edges are measured from the first frame on, so a state that already holds is not an "event".
        prevLocal_ = lf;
        first_ = false;
    }
    if (localMission && !(prevLocal_ & PF_InMission)) localMissionStartMs_ = nowMs;

    // --- which mission is being played
    if (nowMs - lastScriptCheckMs_ >= kScriptCheckMs || (localMission != ((prevLocal_ & PF_InMission) != 0))) {
        lastScriptCheckMs_ = nowMs;
        uint32_t script = localMission ? game_.localMissionScript() : 0;
        if (script != sentScript_) {
            link_.send(Msg::Story, StoryInfo{StoryEvent::Mission, script}, true);
            sentScript_ = script;
        }
    }
    bool mismatch = shared() && localMission && peerMission && sentScript_ && peerScript_ && sentScript_ != peerScript_;
    if (mismatch && !mismatch_)
        notify_("You and " + friendName + " are in different missions - load the same save to play together");
    mismatch_ = mismatch;

    // --- deaths in a mission: in a shared story, the mission restarts for both
    if (localDead && !(prevLocal_ & PF_Dead) && (localMission || (prevLocal_ & PF_InMission))) {
        lastOwnDeathMs_ = nowMs;
        haveDied_ = true;
        if (causedByFriend_) causedByFriend_ = false;  // the friend already knows: it was their failure
        else if (shared()) link_.send(Msg::Story, StoryInfo{StoryEvent::Died, sentScript_}, true);
    }

    if (pendingFail_ && !localCut) {
        pendingFail_ = false;
        if (localMission && !localDead) {
            releaseHold(nullptr);
            causedByFriend_ = true;
            game_.killLocalPlayer();
        }
    }

    // --- protection: nobody can help you while your friend is in a scene
    bool protect = peer && peerCut && !localCut;
    if (protect != protected_) {
        game_.setLocalProtected(protect);
        protected_ = protect;
        if (protect && !shared()) notify_(friendName + " is in a cutscene - you are protected until it ends");
    }

    // --- guest: follow the host into missions
    if (!isHost() && peer) {
        if (peerMission && !(prevPeer_ & PF_InMission)) {
            missionStartPos_ = peer->pos;
            regroupPending_ = true;
            regroupToStart_ = shared() && !localMission;
            joinHintMs_ = nowMs;
            notify_(shared() && !localMission ? friendName + " started a mission - start it here too" : friendName + " started a mission");
        }
        if (!shared() && !peerCut && (prevPeer_ & PF_Cutscene)) regroupPending_ = true;
        if (regroupPending_) {
            regroupPending_ = false;
            Vec3 target = regroupToStart_ ? missionStartPos_ : peer->pos;
            float limit = regroupToStart_ ? kMissionJoinDistance : config_.regroupDistance;
            if (config_.autoRegroup && !peerDead && distance(local.pos, target) > limit) {
                target.x += 1.5f;
                game_.teleportLocalPlayer(target);
                notify_(regroupToStart_ ? "Moved you to the mission start - walk into the marker" : "Moved you to " + friendName);
            }
        }
        if (shared() && peerMission && !localMission && nowMs - joinHintMs_ > kJoinHintMs) {
            joinHintMs_ = nowMs;
            notify_(friendName + " is in a mission - start the same mission to join");
        }
    }

    // --- cutscene barrier (shared story)
    if (shared() && config_.cutsceneBarrier) {
        bool sceneEnded = (prevLocal_ & PF_Cutscene) && !localCut && localMission;
        bool friendJoining = peer && !peerMission && nowMs - localMissionStartMs_ < kJoinWaitMs;
        // With different missions the friend's scenes have nothing to do with ours: never wait then.
        if (!holding_ && sceneEnded && peer && !localDead && !mismatch_ && (peerCut || friendJoining))
            startHold(nowMs, peerCut ? "Waiting for your friend to finish the cutscene..."
                                     : "Waiting for your friend to start the mission...");
        if (holding_) {
            bool friendReady = peer && peerMission && !peerCut;
            if (friendReady) {
                if (!peerReadySinceMs_) peerReadySinceMs_ = nowMs ? nowMs : 1;
            } else {
                peerReadySinceMs_ = 0;
            }
            bool gaveUpJoining = peer && !peerMission && nowMs - holdStartMs_ > kJoinWaitMs;
            if (!peer || !link_.connected() || localDead || !localMission || peerDead || mismatch_)
                releaseHold(nullptr);
            else if (peerReadySinceMs_ && nowMs - peerReadySinceMs_ >= kPeerSettleMs)
                releaseHold("Continuing together");
            else if (gaveUpJoining)
                releaseHold("Your friend did not join the mission - continuing alone");
            else if (nowMs - holdStartMs_ > config_.barrierTimeoutMs)
                releaseHold("Stopped waiting for your friend");
        }
    }

    // --- spectator camera (host world)
    boost_ = false;
    if (!shared() && config_.spectate) {
        if (isHost() && localCut) {
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
        if (!isHost()) {
            bool want = peer && peerCut && lastCamMs_ && nowMs - lastCamMs_ < kCameraStaleMs && !camMotion_.empty();
            if (want) {
                MotionSample m = camMotion_.sample(nowMs);
                CameraState cam;
                cam.pos = m.pos;
                cam.rot = {m.pitch, m.roll, m.heading};
                cam.fov = camFov_;
                camPos_ = m.pos;
                game_.spectate(true, cam);
                if (!spectating_) notify_("Watching " + friendName + "'s cutscene");
                spectating_ = true;
            } else if (spectating_) {
                game_.spectate(false, CameraState{});
                spectating_ = false;
                camMotion_.clear();
            }
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

    if (sendingSave_) sendSaveChunks();
    (void)peerDistance;
    prevLocal_ = lf;
    prevPeer_ = pf;
}

bool StorySync::handle(Msg type, const std::vector<uint8_t>& payload, uint32_t nowMs) {
    switch (type) {
        case Msg::Story: {
            StoryInfo info;
            if (!decode(payload, info)) return true;
            if (info.event == StoryEvent::Mission) {
                peerScript_ = info.missionScript;
            } else if (info.event == StoryEvent::Died && shared() && config_.sharedFail) {
                bool justDied = haveDied_ && nowMs - lastOwnDeathMs_ < kBothDiedMs;
                if ((prevLocal_ & PF_InMission) && !(prevLocal_ & PF_Dead) && !justDied) {
                    notify_(link_.peer().name + " died - the mission restarts for both of you");
                    pendingFail_ = true;  // carried out by tick(), after any running scene
                }
            }
            return true;
        }
        case Msg::Camera: {
            CameraState cam;
            if (isHost() || !decode(payload, cam)) return true;
            MotionSample s;
            s.timeMs = cam.timeMs;
            s.pos = cam.pos;
            s.heading = cam.rot.z;
            s.pitch = cam.rot.x;
            s.roll = cam.rot.y;
            camMotion_.push(s, nowMs);
            camFov_ = cam.fov;
            lastCamMs_ = nowMs ? nowMs : 1;
            return true;
        }
        case Msg::Waypoint: {
            WaypointInfo w;
            if (!decode(payload, w)) return true;
            friendWaypoint_ = w.active != 0;
            friendWaypointPos_ = {w.x, w.y, 0};
            if (config_.syncWaypoints) game_.showFriendWaypoint(friendWaypoint_, friendWaypointPos_);
            return true;
        }
        case Msg::SaveRequest: {
            if (!isHost()) return true;
            std::string name;
            if (!store_ || !store_->readLatest(saveData_, name) || saveData_.empty() || saveData_.size() > kMaxSaveBytes) {
                link_.send(Msg::SaveChunk, SaveChunk{}, true);  // total 0: nothing to share
                notify_("Your friend asked for your save, but no save was found");
                saveData_.clear();
                return true;
            }
            saveCrc_ = crc32(saveData_.data(), saveData_.size());
            saveOffset_ = 0;
            sendingSave_ = true;
            saveProgress_ = 0;
            notify_("Sending your save (" + name + ") to " + link_.peer().name);
            sendSaveChunks();
            return true;
        }
        case Msg::SaveChunk: {
            SaveChunk c;
            if (!isHost() && receivingSave_ && decode(payload, c)) receiveSaveChunk(c);
            return true;
        }
        default:
            return false;
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
        notify_(link_.peer().name + " has no save to share");
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
        notify_("Saved as " + placed + ". Load it: pause menu - Story - Load Game");
    }
    saveData_.clear();
    saveData_.shrink_to_fit();
}

}  // namespace coop
