#include "session.h"

#include <algorithm>
#include <cmath>

namespace coop {

namespace {
constexpr uint32_t kStateIntervalMs = 50;    // 20 Hz player state
constexpr uint32_t kWorldIntervalMs = 100;   // 10 Hz NPC snapshots
constexpr uint32_t kInfoIntervalMs = 2000;
constexpr uint32_t kHideIntervalMs = 500;
constexpr uint32_t kMirrorTimeoutMs = 2000;
constexpr uint32_t kRespawnGraceMs = 1500;
constexpr int kMirrorSpawnsPerFrame = 2;

// Remote characters fire "blanks": their shots are aimed above the target so the bullets the
// local game simulates do not hit anything. Real damage only travels through Damage messages,
// decided by the machine that owns the shooter, which keeps both worlds in agreement.
Vec3 blankTarget(Vec3 p) {
    p.z += 1.8f;
    return p;
}
}  // namespace

CoopSession::CoopSession(IGame& game, Link& link, const SessionConfig& config)
    : game_(game), link_(link), config_(config) {}

void CoopSession::tick(uint32_t nowMs) {
    link_.update(nowMs);
    if (!link_.connected()) {
        if (active_) cleanup();
        return;
    }
    if (link_.epoch() != epoch_) {  // new or re-established session: start from a clean slate
        cleanup();
        epoch_ = link_.epoch();
    }
    active_ = true;

    bool ready = game_.ready();
    handleMessages(nowMs);
    if (ready) {
        sendLocalState(nowMs);
        if (isHost()) sendWorld(nowMs);
        updatePuppet(nowMs);
        if (!isHost()) {
            if (config_.worldSync && hostWorldSync_) {
                game_.suppressAmbientPopulation();
                updateMirrors(nowMs);
                hideForeignPeds(nowMs);
            } else if (!mirrors_.empty() || !hidden_.empty()) {
                for (auto& [id, m] : mirrors_) destroyMirror(m);
                mirrors_.clear();
                for (EntityHandle h : hidden_)
                    if (game_.exists(h)) game_.setHidden(h, false);
                hidden_.clear();
                status_.syncedNpcs = 0;
            }
        }
    }
    link_.flush(nowMs);
}

void CoopSession::cleanup() {
    destroyPuppet();
    puppet_ = Puppet{};
    for (auto& [id, m] : mirrors_) destroyMirror(m);
    mirrors_.clear();
    for (EntityHandle h : hidden_)
        if (game_.exists(h)) game_.setHidden(h, false);
    hidden_.clear();
    npcIds_.clear();
    npcById_.clear();
    hostWorldSync_ = false;
    haveLocal_ = false;
    lastStateMs_ = lastWorldMs_ = lastInfoMs_ = lastHideMs_ = 0;
    status_ = SessionStatus{};
    active_ = false;
}

bool CoopSession::teleportToPeer() {
    if (!puppet_.haveState || !game_.ready()) return false;
    Vec3 p = puppet_.state.pos;
    p.x += 1.5f;
    game_.teleportLocalPlayer(p);
    return true;
}

void CoopSession::destroyPuppet() {
    if (puppet_.ped && game_.exists(puppet_.ped)) game_.deletePed(puppet_.ped);
    if (puppet_.mount && game_.exists(puppet_.mount)) game_.deletePed(puppet_.mount);
    puppet_.ped = puppet_.mount = 0;
    puppet_.riding = false;
    puppet_.weapon = 0;
    puppet_.deadSinceMs = 0;
    puppet_.drive = Drive{};
}

void CoopSession::destroyMirror(Mirror& m) {
    if (m.ped && game_.exists(m.ped)) game_.deletePed(m.ped);
    m.ped = 0;
    m.ridingOn = 0;
    m.weapon = 0;
    m.deadSinceMs = 0;
    m.drive = Drive{};
}

void CoopSession::handleMessages(uint32_t nowMs) {
    Msg type;
    std::vector<uint8_t> payload;
    while (link_.poll(type, payload)) {
        switch (type) {
            case Msg::PlayerState: {
                PlayerState s;
                if (!decode(payload, s)) break;
                if (puppet_.haveState && static_cast<int32_t>(s.timeMs - puppet_.state.timeMs) <= 0) break;
                puppet_.state = s;
                puppet_.haveState = true;
                puppet_.motion.push(MotionSample{s.timeMs, s.pos, s.heading, length(s.vel)}, nowMs);
                break;
            }
            case Msg::WorldSnapshot: {
                WorldSnapshot snap;
                if (isHost() || !decode(payload, snap)) break;
                for (const NpcState& npc : snap.npcs) {
                    if (npc.id == 0) continue;
                    auto it = mirrors_.find(npc.id);
                    if (it == mirrors_.end()) {
                        if (mirrors_.size() >= 256) continue;
                        it = mirrors_.emplace(npc.id, Mirror{}).first;
                    } else if (it->second.state.model != npc.model) {
                        destroyMirror(it->second);  // the host reused the id for something else
                        it->second.motion.clear();
                    }
                    Mirror& m = it->second;
                    m.state = npc;
                    m.lastSeenMs = nowMs;
                    m.motion.push(MotionSample{snap.timeMs, npc.pos, npc.heading, npc.speed}, nowMs);
                }
                break;
            }
            case Msg::Damage: {
                Damage d;
                if (!decode(payload, d) || d.amount == 0 || !game_.ready()) break;
                if (d.target == kTargetPlayer) {
                    game_.damageLocalPlayer(d.amount);
                } else if (isHost()) {
                    auto it = npcById_.find(d.target);
                    if (it != npcById_.end() && game_.exists(it->second))
                        game_.damageNpc(it->second, d.amount, puppet_.ped);
                }
                break;
            }
            case Msg::WorldInfo: {
                WorldInfo info;
                if (isHost() || !decode(payload, info)) break;
                hostWorldSync_ = info.worldSync != 0;
                if (config_.syncClock && game_.ready()) game_.applyWorldInfo(info);
                break;
            }
            default:
                break;
        }
    }
}

void CoopSession::sendLocalState(uint32_t nowMs) {
    if (haveLocal_ && nowMs - lastStateMs_ < kStateIntervalMs) return;
    local_ = game_.readLocalPlayer();
    local_.timeMs = nowMs;
    haveLocal_ = true;
    lastStateMs_ = nowMs;
    link_.send(Msg::PlayerState, local_, false);
}

void CoopSession::sendWorld(uint32_t nowMs) {
    if (lastInfoMs_ == 0 || nowMs - lastInfoMs_ >= kInfoIntervalMs) {
        WorldInfo info = game_.readWorldInfo();
        info.worldSync = config_.worldSync ? 1 : 0;
        link_.send(Msg::WorldInfo, info, false);
        lastInfoMs_ = nowMs ? nowMs : 1;
    }
    if (!config_.worldSync || nowMs - lastWorldMs_ < kWorldIntervalMs) return;
    lastWorldMs_ = nowMs;

    Vec3 peerPos = puppet_.haveState ? puppet_.state.pos : local_.pos;
    std::vector<EntityHandle> exclude;
    if (puppet_.ped) exclude.push_back(puppet_.ped);
    if (puppet_.mount) exclude.push_back(puppet_.mount);
    std::vector<LocalNpc> npcs;
    game_.listNpcs(local_.pos, peerPos, config_.npcRadius, exclude, npcs);

    // The guest cares most about what is close to them.
    std::sort(npcs.begin(), npcs.end(), [&](const LocalNpc& a, const LocalNpc& b) {
        return distance(a.state.pos, peerPos) < distance(b.state.pos, peerPos);
    });
    if (npcs.size() > static_cast<size_t>(std::max(config_.maxNpcs, 0))) npcs.resize(std::max(config_.maxNpcs, 0));

    // Keep ids stable for NPCs we already announced; hand out fresh ones to newcomers.
    std::map<EntityHandle, uint16_t> ids;
    std::map<uint16_t, EntityHandle> byId;
    for (const LocalNpc& npc : npcs) {
        auto known = npcIds_.find(npc.handle);
        if (known == npcIds_.end()) continue;
        ids[npc.handle] = known->second;
        byId[known->second] = npc.handle;
    }
    for (const LocalNpc& npc : npcs) {
        if (ids.count(npc.handle)) continue;
        while (nextNpcId_ == 0 || byId.count(nextNpcId_) || npcById_.count(nextNpcId_)) ++nextNpcId_;
        ids[npc.handle] = nextNpcId_;
        byId[nextNpcId_] = npc.handle;
        ++nextNpcId_;
    }
    npcIds_ = std::move(ids);
    npcById_ = std::move(byId);

    WorldSnapshot snap;
    snap.timeMs = nowMs;
    auto sendChunk = [&] {
        link_.send(Msg::WorldSnapshot, snap, false);
        snap.npcs.clear();
    };
    for (LocalNpc& npc : npcs) {
        npc.state.id = npcIds_[npc.handle];
        auto mount = npcIds_.find(npc.mount);
        npc.state.mountId = npc.mount && mount != npcIds_.end() ? mount->second : 0;
        if (npc.fightsPeer) npc.state.flags |= NF_TargetsGuest;
        snap.npcs.push_back(npc.state);
        if (snap.npcs.size() == kMaxNpcsPerPacket) sendChunk();
    }
    if (!snap.npcs.empty()) sendChunk();
}

void CoopSession::drive(EntityHandle ped, Drive& d, const MotionSample& target, uint32_t nowMs) {
    float error = distance(game_.position(ped), target.pos);
    float snapDistance = 6.0f + target.speed;  // faster movers (horses) get more slack before a warp
    if ((d.mode == Drive::None && error > 1.5f) || error > snapDistance) {
        game_.teleport(ped, target.pos, target.heading);
        d.mode = Drive::Idle;
        d.lastTaskMs = nowMs;
        return;
    }
    if (target.speed < 0.4f && error < 1.0f) {
        bool turned = std::fabs(angleDelta(game_.heading(ped), target.heading)) > 20.0f;
        if (d.mode != Drive::Idle || (turned && nowMs - d.lastTaskMs >= 500)) {
            game_.standStill(ped, target.heading);
            d.mode = Drive::Idle;
            d.lastTaskMs = nowMs;
        }
        return;
    }
    if (d.mode != Drive::Moving || nowMs - d.lastTaskMs >= 250) {
        float speed = std::max(target.speed, 1.0f) + error * 0.5f;  // hurry up when lagging behind
        game_.moveTo(ped, target.pos, speed, target.heading);
        d.mode = Drive::Moving;
        d.lastTaskMs = nowMs;
    }
}

void CoopSession::forwardDamage(EntityHandle ped, int& expectedHealth, uint16_t target) {
    int current = game_.health(ped);
    if (current < expectedHealth) {
        bool byLocalPlayer = game_.wasDamagedByLocalPlayer(ped);
        bool forward;
        if (target != kTargetPlayer) {
            // A mirror lost health on the guest. Remote characters only fire blanks here, so
            // whatever hurt it was the guest's doing: tell the host, who owns the real NPC.
            forward = true;
        } else if (isHost()) {
            // The host's NPCs hurt the guest's puppet for real; the host player only with friendly fire.
            forward = !byLocalPlayer || config_.friendlyFire;
        } else {
            forward = byLocalPlayer && config_.friendlyFire;
        }
        if (forward) {
            int amount = std::min(expectedHealth - current, 65535);
            link_.send(Msg::Damage, Damage{target, static_cast<uint16_t>(amount)}, true);
        }
    }
    expectedHealth = current;
}

void CoopSession::updatePuppet(uint32_t nowMs) {
    status_.peerVisible = false;
    if (!puppet_.haveState) return;
    const PlayerState& s = puppet_.state;
    status_.peerPos = s.pos;
    status_.peerDistance = haveLocal_ ? distance(local_.pos, s.pos) : 0;
    status_.peerDead = (s.flags & PF_Dead) != 0;
    status_.peerInMission = (s.flags & PF_InMission) != 0;

    if (puppet_.ped && !game_.exists(puppet_.ped)) {  // the game removed it behind our back
        puppet_.ped = 0;
        puppet_.riding = false;
        puppet_.weapon = 0;
        puppet_.drive = Drive{};
    }
    if (puppet_.mount && !game_.exists(puppet_.mount)) {
        puppet_.mount = 0;
        puppet_.riding = false;
        puppet_.drive = Drive{};
    }
    if (!puppet_.ped) {
        const std::string& avatar = link_.peer().avatar;
        puppet_.ped = game_.spawnPed(joaat(avatar.c_str()), s.pos, s.heading, PedKind::PlayerPuppet);
        if (!puppet_.ped) return;
        game_.setHealth(puppet_.ped, std::max<int>(s.health, 1), s.maxHealth);
        game_.setLabel(puppet_.ped, link_.peer().name.c_str());
        puppet_.expectedHealth = game_.health(puppet_.ped);
    }
    status_.peerVisible = true;

    if (s.flags & PF_Dead) {
        if (!game_.isDead(puppet_.ped)) game_.kill(puppet_.ped);
        puppet_.expectedHealth = 0;
        return;
    }

    forwardDamage(puppet_.ped, puppet_.expectedHealth, kTargetPlayer);
    if (game_.isDead(puppet_.ped)) {
        // Dead here but alive for its owner (they respawned, or survived the hit): recreate it.
        if (!puppet_.deadSinceMs) puppet_.deadSinceMs = nowMs ? nowMs : 1;
        else if (nowMs - puppet_.deadSinceMs > kRespawnGraceMs) destroyPuppet();
        return;
    }
    puppet_.deadSinceMs = 0;
    if (s.health != puppet_.expectedHealth && s.health > 0) {
        game_.setHealth(puppet_.ped, s.health, s.maxHealth);
        puppet_.expectedHealth = game_.health(puppet_.ped);
    }

    bool wantMount = (s.flags & PF_OnMount) && s.mountModel != 0;
    if (wantMount) {
        if (puppet_.mount && puppet_.mountModel != s.mountModel) {
            if (puppet_.riding) game_.setMount(puppet_.ped, 0);
            game_.deletePed(puppet_.mount);
            puppet_.mount = 0;
            puppet_.riding = false;
        }
        if (!puppet_.mount) {
            puppet_.mount = game_.spawnPed(s.mountModel, s.pos, s.heading, PedKind::PuppetMount);
            puppet_.mountModel = s.mountModel;
        }
        if (puppet_.mount && !puppet_.riding) {
            game_.teleport(puppet_.mount, s.pos, s.heading);
            game_.setMount(puppet_.ped, puppet_.mount);
            puppet_.riding = true;
            puppet_.drive = Drive{};
        }
    } else if (puppet_.riding) {
        game_.setMount(puppet_.ped, 0);
        game_.teleport(puppet_.ped, s.pos, s.heading);
        puppet_.riding = false;
        puppet_.drive = Drive{};
    }
    // A horse the friend left far behind is no longer needed.
    if (puppet_.mount && !puppet_.riding && distance(game_.position(puppet_.mount), s.pos) > 80.0f) {
        game_.deletePed(puppet_.mount);
        puppet_.mount = 0;
    }

    if (s.weapon != puppet_.weapon) {
        game_.setWeapon(puppet_.ped, s.weapon);
        puppet_.weapon = s.weapon;
    }

    MotionSample target = puppet_.motion.sample(nowMs);
    bool shooting = (s.flags & PF_Shooting) != 0;
    bool aiming = (s.flags & PF_Aiming) != 0;
    // On a horse the rider's arms are free; on foot aiming replaces walking only when (nearly) standing.
    bool aimTask = shooting || (aiming && (puppet_.riding || target.speed < 1.0f));
    if (aimTask) {
        uint32_t interval = shooting ? 200u : 400u;
        Drive::Mode mode = shooting ? Drive::Shooting : Drive::Aiming;
        Drive& d = puppet_.drive;
        bool changed = puppet_.riding ? false : d.mode != mode;
        if (changed || nowMs - puppet_.lastAimMs >= interval) {
            if (shooting) game_.shootAt(puppet_.ped, blankTarget(s.aim));
            else game_.aimAt(puppet_.ped, s.aim);
            puppet_.lastAimMs = nowMs;
            if (!puppet_.riding) d.mode = mode;
        }
    }
    if (puppet_.riding) {
        drive(puppet_.mount, puppet_.drive, target, nowMs);
    } else if (!aimTask) {
        if (puppet_.drive.mode == Drive::Aiming || puppet_.drive.mode == Drive::Shooting) puppet_.drive.mode = Drive::None;
        drive(puppet_.ped, puppet_.drive, target, nowMs);
    } else if (distance(game_.position(puppet_.ped), target.pos) > 6.0f) {
        game_.teleport(puppet_.ped, target.pos, target.heading);  // keep a shooting puppet from drifting away
    }
}

void CoopSession::updateMirrors(uint32_t nowMs) {
    int spawnBudget = kMirrorSpawnsPerFrame;
    int alive = 0;
    for (auto it = mirrors_.begin(); it != mirrors_.end();) {
        Mirror& m = it->second;
        if (nowMs - m.lastSeenMs > kMirrorTimeoutMs) {  // left the streamed area or was removed on the host
            destroyMirror(m);
            it = mirrors_.erase(it);
            continue;
        }
        ++it;

        if (m.ped && !game_.exists(m.ped)) {
            m.ped = 0;
            m.ridingOn = 0;
            m.weapon = 0;
            m.drive = Drive{};
        }
        bool hostDead = (m.state.flags & NF_Dead) != 0;
        if (!m.ped) {
            if (spawnBudget <= 0) continue;
            --spawnBudget;
            m.ped = game_.spawnPed(m.state.model, m.state.pos, m.state.heading, PedKind::NpcMirror);
            if (!m.ped) continue;
            if (!hostDead && m.state.health > 0) game_.setHealth(m.ped, m.state.health, 0);
            m.expectedHealth = game_.health(m.ped);
        }
        ++alive;

        if (hostDead) {
            if (!game_.isDead(m.ped)) game_.kill(m.ped);
            m.expectedHealth = 0;
            continue;
        }
        forwardDamage(m.ped, m.expectedHealth, m.state.id);
        if (game_.isDead(m.ped)) {
            // We killed it locally; if the host disagrees for long enough, bring it back.
            if (!m.deadSinceMs) m.deadSinceMs = nowMs ? nowMs : 1;
            else if (nowMs - m.deadSinceMs > kRespawnGraceMs) destroyMirror(m);
            continue;
        }
        m.deadSinceMs = 0;
        if (m.state.health > 0 && m.state.health != m.expectedHealth) {
            game_.setHealth(m.ped, m.state.health, 0);
            m.expectedHealth = game_.health(m.ped);
        }

        // Riding: sit on the mirror of whatever the NPC rides on the host; the mount is driven on its own.
        EntityHandle mount = 0;
        if (m.state.mountId) {
            auto mt = mirrors_.find(m.state.mountId);
            if (mt != mirrors_.end() && mt->second.ped && game_.exists(mt->second.ped)) mount = mt->second.ped;
        }
        if (mount != m.ridingOn) {
            game_.setMount(m.ped, mount);
            if (!mount) game_.teleport(m.ped, m.state.pos, m.state.heading);
            m.ridingOn = mount;
            m.drive = Drive{};
        }

        if (m.state.weapon != m.weapon) {
            game_.setWeapon(m.ped, m.state.weapon);
            m.weapon = m.state.weapon;
        }

        bool shooting = (m.state.flags & NF_Shooting) != 0;
        if (shooting && nowMs - m.lastAimMs >= 300) {
            Vec3 victim = (m.state.flags & NF_TargetsGuest) || !puppet_.haveState ? local_.pos : puppet_.state.pos;
            game_.shootAt(m.ped, blankTarget(victim));
            m.lastAimMs = nowMs;
            m.drive.mode = Drive::Shooting;
        }
        if (!m.ridingOn && !(shooting && m.state.speed < 1.0f)) {
            if (m.drive.mode == Drive::Shooting) m.drive.mode = Drive::None;
            drive(m.ped, m.drive, m.motion.sample(nowMs), nowMs);
        }
    }
    status_.syncedNpcs = alive;
}

void CoopSession::hideForeignPeds(uint32_t nowMs) {
    if (lastHideMs_ != 0 && nowMs - lastHideMs_ < kHideIntervalMs) return;
    lastHideMs_ = nowMs ? nowMs : 1;

    std::set<EntityHandle> ours;
    if (puppet_.ped) ours.insert(puppet_.ped);
    if (puppet_.mount) ours.insert(puppet_.mount);
    for (const auto& [id, m] : mirrors_)
        if (m.ped) ours.insert(m.ped);

    std::vector<EntityHandle> all;
    game_.listAllPeds(all);
    std::set<EntityHandle> stillThere;
    for (EntityHandle h : all) {
        if (ours.count(h) || game_.belongsToLocalPlayer(h)) continue;
        stillThere.insert(h);
        if (hidden_.insert(h).second) game_.setHidden(h, true);
    }
    for (auto it = hidden_.begin(); it != hidden_.end();)
        it = stillThere.count(*it) ? std::next(it) : hidden_.erase(it);
}

}  // namespace coop
