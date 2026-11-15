#include "session.h"

#include <algorithm>
#include <cmath>

namespace coop {

namespace {
constexpr uint32_t kPlayerIntervalMs = 33;   // ~30 Hz own character
constexpr uint32_t kWorldIntervalMs = 66;    // ~15 Hz world tick; entities update every 1, 2 or 4 ticks
constexpr uint32_t kInfoIntervalMs = 2000;
constexpr uint32_t kKeyframeMs = 1000;       // every streamed entity is fully refreshed this often
constexpr uint32_t kRedundancyMs = 200;      // a change is repeated this long, covering lost packets
constexpr uint32_t kRemovalResendMs = 300;
constexpr uint32_t kMirrorTimeoutMs = 3000;
constexpr uint32_t kForeignScanMs = 500;
constexpr uint32_t kRespawnGraceMs = 1500;
constexpr uint32_t kOwnRideKeepMs = 30000;
constexpr int kSpawnsPerFrame = 3;
constexpr uint32_t kIdlePlayerIntervalMs = 200;  // standing still: 5 Hz is plenty
constexpr uint32_t kOutfitCheckMs = 2000;
constexpr float kStreamHysteresis = 20.0f;       // entities leave the stream this much further out than they enter
constexpr size_t kPoolSize = 24;
constexpr uint32_t kPoolKeepMs = 10000;
constexpr uint32_t kBindWaitMs = 2500;      // shared story: wait this long for the local copy before showing a mirror
constexpr uint32_t kMissionScanMs = 250;
constexpr float kBindDistance = 35.0f;

constexpr int kGroupPos = 0, kGroupHeading = 1, kGroupTilt = 2, kGroupVel = 3, kGroupHealth = 4, kGroupState = 5,
              kGroupStatic = 6;

float horizontalSpeed(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// Remote characters fire "blanks": their shots are aimed above the target so the bullets the
// local game simulates do not hit anything. Real damage only travels through Damage messages,
// decided by the machine that owns the shooter, which keeps both worlds in agreement.
Vec3 blankTarget(Vec3 p) {
    p.z += 1.8f;
    return p;
}

Vec3 forwardPoint(const Vec3& pos, float heading, float dist) {
    float rad = heading * 3.14159265f / 180.0f;
    return {pos.x - std::sin(rad) * dist, pos.y + std::cos(rad) * dist, pos.z + 1.0f};
}

// How often (in world ticks) an entity is refreshed, by distance to the guest.
uint32_t updateInterval(float guestDistance, const EntityState& st) {
    if (st.flags & (NF_Shooting | NF_InCombat | NF_Aiming)) return guestDistance < 60.0f ? 1 : 2;
    if (st.kind == EntityKind::Vehicle && length(st.vel) > 3.0f) return guestDistance < 80.0f ? 1 : 2;
    if (guestDistance < 30.0f) return 1;
    if (guestDistance < 80.0f) return 2;
    return 4;
}
}  // namespace

CoopSession::CoopSession(IGame& game, Link& link, const SessionConfig& config)
    : game_(game), link_(link), config_(config), story_(game, link, config_, [this](const std::string& t) { notice(t); }) {
    nearby_.reserve(512);
    outgoing_.reserve(128);
}

void CoopSession::tick(uint32_t nowMs) {
    float dt = lastTickMs_ ? std::clamp(static_cast<float>(nowMs - lastTickMs_) / 1000.0f, 0.0f, 0.25f) : 0.016f;
    lastTickMs_ = nowMs;

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
        ++frame_;
        sendLocalState(nowMs);
        sendAppearance(nowMs);
        if (isHost()) sendWorld(nowMs);
        updatePuppet(nowMs, dt);
        story_.tick(nowMs, local_.state, puppet_.haveState ? &puppet_.state : nullptr, status_.peerDistance);
        status_.holding = story_.holding();
        status_.spectating = story_.spectating();
        status_.missionMismatch = story_.missionMismatch();
        status_.saveProgress = story_.saveProgress();
        if (!isHost()) {
            if (config_.worldSync && hostWorldSync_) {
                game_.suppressAmbientPopulation();
                updateMirrors(nowMs, dt);
                clearForeign(nowMs);
            } else if (!mirrors_.empty() || !hidden_.empty()) {
                restoreWorld();
            }
        }
    }
    link_.flush(nowMs);
}

bool CoopSession::popNotice(std::string& out) {
    if (notices_.empty()) return false;
    out = notices_.front();
    notices_.erase(notices_.begin());
    return true;
}

void CoopSession::notice(const std::string& text) {
    if (notices_.size() < 8) notices_.push_back(text);
}

void CoopSession::restoreWorld() {
    for (auto& [id, m] : mirrors_) destroyMirror(m, false);
    mirrors_.clear();
    mirrorByHandle_.clear();
    flushPool(0, true);
    for (EntityHandle h : hidden_)
        if (game_.exists(h)) game_.restoreForeign(h);
    hidden_.clear();
    status_.syncedEntities = 0;
}

void CoopSession::cleanup() {
    destroyPuppet();
    puppet_ = Puppet{};
    restoreWorld();
    streamed_.clear();
    handleById_.clear();
    removals_.clear();
    hostWorldSync_ = false;
    haveLocal_ = false;
    lastStateMs_ = lastWorldMs_ = lastInfoMs_ = lastForeignMs_ = 0;
    lastSentFlags_ = 0;
    lastSentMoving_ = false;
    lastOutfitCheckMs_ = sentOutfit_ = 0;
    appearanceSent_ = false;
    story_.reset();
    missionLocal_.clear();
    lastMissionScanMs_ = 0;
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

// ---------------------------------------------------------------- references

EntityHandle CoopSession::resolveRef(uint16_t ref) {
    if (ref == kRefNone || ref > kMaxEntityId) return 0;
    if (isHost()) {
        auto it = handleById_.find(ref);
        return it != handleById_.end() && game_.exists(it->second) ? it->second : 0;
    }
    auto it = mirrors_.find(ref);
    return it != mirrors_.end() && it->second.handle && game_.exists(it->second.handle) ? it->second.handle : 0;
}

uint16_t CoopSession::refOf(EntityHandle h) {
    if (!h) return kRefNone;
    if (isHost()) {
        if (h == game_.localPlayerPed()) return kRefHostPlayer;
        if (h == puppet_.ped) return kRefGuestPlayer;
        auto it = streamed_.find(h);
        return it != streamed_.end() ? it->second.id : kRefNone;
    }
    auto it = mirrorByHandle_.find(h);
    return it != mirrorByHandle_.end() ? it->second : kRefNone;
}

Vec3 CoopSession::refPosition(uint16_t ref, bool& found) {
    found = true;
    bool guestIsLocal = !isHost();
    if (ref == kRefGuestPlayer || ref == kRefHostPlayer) {
        bool local = (ref == kRefGuestPlayer) == guestIsLocal;
        if (local) return local_.state.pos;
        if (puppet_.ped && game_.exists(puppet_.ped)) return game_.position(puppet_.ped);
        found = puppet_.haveState;
        return puppet_.state.pos;
    }
    EntityHandle h = resolveRef(ref);
    found = h != 0;
    return h ? game_.position(h) : Vec3{};
}

bool CoopSession::isSessionEntity(EntityHandle h) const {
    return h && (h == puppet_.ped || h == puppet_.ownMount || h == puppet_.ownVehicle || inPool(h));
}

bool CoopSession::inPool(EntityHandle h) const {
    for (const Parked& p : pool_)
        if (p.handle == h) return true;
    return false;
}

EntityHandle CoopSession::obtain(EntityKind kind, uint32_t model, const Vec3& pos, float heading) {
    for (size_t i = 0; i < pool_.size(); ++i) {
        if (pool_[i].kind != kind || pool_[i].model != model) continue;
        EntityHandle h = pool_[i].handle;
        pool_.erase(pool_.begin() + static_cast<std::ptrdiff_t>(i));
        if (!game_.exists(h)) break;  // the game removed it; create a fresh one
        game_.unpark(h, pos, heading);
        return h;
    }
    return game_.spawn(kind, model, pos, heading, SpawnRole::Mirror);
}

void CoopSession::flushPool(uint32_t nowMs, bool all) {
    for (auto it = pool_.begin(); it != pool_.end();) {
        if (all || nowMs - it->sinceMs > kPoolKeepMs) {
            if (game_.exists(it->handle)) game_.remove(it->handle);
            it = pool_.erase(it);
        } else {
            ++it;
        }
    }
}

uint16_t CoopSession::allocateId() {
    for (int tries = 0; tries < kMaxEntityId; ++tries) {
        uint16_t id = nextId_;
        nextId_ = nextId_ >= kMaxEntityId ? 1 : static_cast<uint16_t>(nextId_ + 1);
        if (!handleById_.count(id)) return id;
    }
    return 0;
}

// ---------------------------------------------------------------- teardown helpers

void CoopSession::destroyPuppet() {
    for (EntityHandle h : {puppet_.ped, puppet_.ownMount, puppet_.ownVehicle})
        if (h && game_.exists(h)) game_.remove(h);
    puppet_.ped = puppet_.ownMount = puppet_.ownVehicle = 0;
    puppet_.ridingOn = puppet_.seatedIn = 0;
    puppet_.weapon = 0;
    puppet_.deadSinceMs = 0;
    puppet_.crouched = puppet_.ragdolled = puppet_.aimingTask = false;
    puppet_.mover = puppet_.bodyMover = Mover{};
}

void CoopSession::destroyMirror(Mirror& m, bool recycle) {
    if (m.bound) {
        // Never delete the guest's own mission entity: just stop driving it.
        mirrorByHandle_.erase(m.handle);
        if (game_.exists(m.handle)) game_.setHarmless(m.handle, false);
        m.handle = 0;
        m.bound = false;
        m.mover = Mover{};
        return;
    }
    if (m.handle) {
        mirrorByHandle_.erase(m.handle);
        if (game_.exists(m.handle)) {
            // Live entities are parked for reuse: the next mirror of the same model appears instantly.
            if (recycle && pool_.size() < kPoolSize && !game_.isDead(m.handle)) {
                game_.park(m.handle);
                pool_.push_back(Parked{m.handle, m.st.kind, m.st.model, lastTickMs_});
            } else {
                game_.remove(m.handle);
            }
        }
    }
    m.outfitApplied = 0;
    m.handle = 0;
    m.ridingOn = m.seatedIn = 0;
    m.weapon = 0;
    m.deadSinceMs = 0;
    m.ragdolled = false;
    m.mover = Mover{};
}

// ---------------------------------------------------------------- incoming

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
                MotionSample sample;
                sample.timeMs = s.timeMs;
                sample.pos = s.pos;
                sample.vel = s.vel;
                sample.hasVel = true;
                sample.heading = s.heading;
                puppet_.motion.push(sample, nowMs);
                break;
            }
            case Msg::EntitySnapshot: {
                EntitySnapshot snap;
                if (!isHost() && decode(payload, snap)) onSnapshot(snap, nowMs);
                break;
            }
            case Msg::Damage: {
                Damage d;
                if (!decode(payload, d) || d.amount == 0 || !game_.ready()) break;
                bool toMe = isHost() ? d.target == kRefHostPlayer : d.target == kRefGuestPlayer;
                if (toMe) {
                    game_.damageLocalPlayer(d.amount);
                } else if (isHost()) {
                    EntityHandle victim = resolveRef(d.target);
                    if (victim) game_.damageNpc(victim, d.amount, puppet_.ped);
                }
                break;
            }
            case Msg::Story:
            case Msg::Camera:
            case Msg::Waypoint:
            case Msg::SaveRequest:
            case Msg::SaveChunk:
                story_.handle(type, payload, nowMs);
                break;
            case Msg::Appearance: {
                Appearance a;
                if (decode(payload, a)) puppet_.outfit = a.outfit;
                break;
            }
            case Msg::WorldInfo: {
                WorldInfo info;
                if (isHost() || !decode(payload, info)) break;
                hostWorldSync_ = info.worldSync != 0;
                if (game_.ready()) game_.applyWorldInfo(info, config_.syncClock, config_.syncWeather);
                break;
            }
            default:
                break;
        }
    }
}

void CoopSession::onSnapshot(const EntitySnapshot& snap, uint32_t nowMs) {
    for (const EntityState& e : snap.entities) {
        auto it = mirrors_.find(e.id);
        if (it == mirrors_.end()) {
            if (mirrors_.size() >= 1024) continue;
            it = mirrors_.emplace(e.id, Mirror{}).first;
            it->second.id = e.id;
            it->second.firstHeardMs = nowMs;
        }
        Mirror& m = it->second;
        m.lastHeardMs = nowMs;
        if (e.fields & EF_Static) {
            if (m.haveStatic && (m.st.model != e.model || m.st.kind != e.kind)) {
                destroyMirror(m, true);  // the host reused the id for something else
                m.motion.clear();
                m.havePos = m.haveVel = false;
            }
            m.st.kind = e.kind;
            m.st.model = e.model;
            m.st.outfit = e.outfit;
            m.st.script = e.script;
            m.haveStatic = true;
        }
        if (e.fields & EF_Health) m.st.health = e.health;
        if (e.fields & EF_State) {
            m.st.flags = e.flags;
            m.st.weapon = e.weapon;
            m.st.mountRef = e.mountRef;
            m.st.vehicleRef = e.vehicleRef;
            m.st.seat = e.seat;
            m.st.targetRef = e.targetRef;
        }
        bool motion = (e.fields & (EF_Pos | EF_Heading | EF_Tilt | EF_Vel)) != 0;
        if (e.fields & EF_Pos) {
            m.st.pos = e.pos;
            m.havePos = true;
        }
        if (e.fields & EF_Heading) m.st.heading = e.heading;
        if (e.fields & EF_Tilt) {
            m.st.pitch = e.pitch;
            m.st.roll = e.roll;
        }
        if (e.fields & EF_Vel) {
            m.st.vel = e.vel;
            m.haveVel = true;
        }
        if (motion && m.havePos) {
            MotionSample s;
            s.timeMs = snap.timeMs;
            s.pos = m.st.pos;
            s.vel = m.st.vel;
            s.hasVel = m.haveVel;
            s.heading = m.st.heading;
            s.pitch = m.st.pitch;
            s.roll = m.st.roll;
            m.motion.push(s, nowMs);
        }
    }
    for (uint16_t id : snap.removed) {
        auto it = mirrors_.find(id);
        if (it == mirrors_.end()) continue;
        destroyMirror(it->second, true);
        mirrors_.erase(it);
    }
}

// ---------------------------------------------------------------- outgoing

void CoopSession::sendLocalState(uint32_t nowMs) {
    bool first = !haveLocal_;
    local_ = game_.readLocalPlayer();
    haveLocal_ = true;
    PlayerState& s = local_.state;
    // Full rate while anything happens, 5 Hz while standing still. Discrete changes (first shot,
    // mounting, starting to move) go out immediately instead of waiting for the next tick.
    bool moving = horizontalSpeed(s.vel) > 0.15f || std::fabs(s.vel.z) > 0.5f ||
                  (s.flags & (PF_Aiming | PF_Shooting | PF_Ragdoll)) != 0;
    uint32_t interval = moving || lastSentMoving_ ? kPlayerIntervalMs : kIdlePlayerIntervalMs;
    bool due = first || nowMs - lastStateMs_ >= interval;
    if (!due && s.flags == lastSentFlags_ && moving == lastSentMoving_) return;
    lastSentMoving_ = moving;
    s.flags |= story_.extraFlags();
    s.timeMs = nowMs;
    s.mountRef = kRefNone;
    s.vehicleRef = kRefNone;
    if (s.flags & PF_OnMount) {
        s.mountRef = refOf(local_.mount);
        if (s.mountRef > kMaxEntityId) s.mountRef = kRefNone;
    }
    if (s.flags & PF_InVehicle) {
        s.vehicleRef = refOf(local_.vehicle);
        if (s.vehicleRef > kMaxEntityId) s.vehicleRef = kRefNone;
    }
    lastStateMs_ = nowMs;
    lastSentFlags_ = s.flags;
    link_.send(Msg::PlayerState, s, false);
}

void CoopSession::sendAppearance(uint32_t nowMs) {
    if (appearanceSent_ && nowMs - lastOutfitCheckMs_ < kOutfitCheckMs) return;
    lastOutfitCheckMs_ = nowMs;
    uint32_t outfit = game_.localOutfit();
    if (appearanceSent_ && outfit == sentOutfit_) return;
    link_.send(Msg::Appearance, Appearance{outfit}, true);
    sentOutfit_ = outfit;
    appearanceSent_ = true;
}

void CoopSession::sendWorld(uint32_t nowMs) {
    if (lastInfoMs_ == 0 || nowMs - lastInfoMs_ >= kInfoIntervalMs) {
        WorldInfo info = game_.readWorldInfo();
        info.worldSync = config_.worldSync ? 1 : 0;
        link_.send(Msg::WorldInfo, info, false);
        lastInfoMs_ = nowMs ? nowMs : 1;
    }
    if (!config_.worldSync || (lastWorldMs_ != 0 && nowMs - lastWorldMs_ < kWorldIntervalMs)) return;
    lastWorldMs_ = nowMs ? nowMs : 1;

    Vec3 hostPos = local_.state.pos;
    Vec3 guestPos = puppet_.haveState ? puppet_.state.pos : hostPos;
    nearby_.clear();
    game_.listNearby(hostPos, guestPos, config_.npcRadius + kStreamHysteresis, nearby_);

    // Closest to the guest first; keep at most maxPeds / maxVehicles. Hysteresis keeps entities at
    // the edge (of the radius or of the budget) from popping in and out every tick.
    order_.clear();
    for (size_t i = 0; i < nearby_.size(); ++i) {
        if (isSessionEntity(nearby_[i].handle)) continue;
        bool streamed = streamed_.count(nearby_[i].handle) != 0;
        float range = std::min(distance(nearby_[i].pos, guestPos), distance(nearby_[i].pos, hostPos));
        if (!streamed && range > config_.npcRadius) continue;
        float d = std::min(distance(nearby_[i].pos, guestPos), distance(nearby_[i].pos, hostPos) + 15.0f);
        order_.emplace_back(streamed ? d - 10.0f : d, i);
    }
    std::sort(order_.begin(), order_.end());

    for (auto& [h, s] : streamed_) s.seen = false;
    int peds = 0, vehicles = 0;
    for (const auto& [d, index] : order_) {
        const NearbyEntity& n = nearby_[index];
        int& count = n.kind == EntityKind::Vehicle ? vehicles : peds;
        int cap = n.kind == EntityKind::Vehicle ? config_.maxVehicles : config_.maxPeds;
        bool mustKeep = n.handle == local_.vehicle || n.handle == puppet_.ridingOn || n.handle == puppet_.seatedIn;
        if (count >= cap && !mustKeep) continue;
        ++count;
        auto it = streamed_.find(n.handle);
        if (it == streamed_.end()) {
            uint16_t id = allocateId();
            if (!id) continue;
            Streamed s;
            s.handle = n.handle;
            s.kind = n.kind;
            s.id = id;
            s.createdMs = nowMs;
            it = streamed_.emplace(n.handle, s).first;
            handleById_[id] = n.handle;
        } else if (it->second.kind != n.kind) {
            continue;  // handle recycled by the game as a different kind; it will be replaced below
        }
        it->second.seen = true;
        it->second.guestDistance = distance(n.pos, guestPos);
    }

    // Whatever was not picked this tick has gone (deleted, out of range, or over budget).
    for (auto it = streamed_.begin(); it != streamed_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        removals_.emplace_back(it->second.id, nowMs + kRemovalResendMs);
        handleById_.erase(it->second.id);
        it = streamed_.erase(it);
    }

    outgoing_.clear();
    for (auto& [h, s] : streamed_) {
        if (!s.fresh && static_cast<int32_t>(nowMs - s.nextDueMs) < 0) continue;
        streamEntity(s, nowMs);
    }

    removedIds_.clear();
    for (auto it = removals_.begin(); it != removals_.end();) {
        if (static_cast<int32_t>(nowMs - it->second) > 0) {
            it = removals_.erase(it);
            continue;
        }
        if (!handleById_.count(it->first)) removedIds_.push_back(it->first);  // skip ids already reused
        ++it;
    }
    status_.syncedEntities = static_cast<int>(streamed_.size());

    if (outgoing_.empty() && removedIds_.empty()) return;
    packets_.clear();
    encodeSnapshots(nowMs, hostPos, outgoing_, removedIds_, kMaxMessage, packets_);
    for (const auto& p : packets_) link_.send(Msg::EntitySnapshot, p, false);
}

void CoopSession::streamEntity(Streamed& s, uint32_t nowMs) {
    EntityRead read;
    game_.readEntity(s.handle, s.kind, read);
    EntityState cur = read.state;
    cur.kind = s.kind;
    cur.mountRef = refOf(read.mount);
    cur.vehicleRef = refOf(read.vehicle);
    if (cur.mountRef > kMaxEntityId) cur.mountRef = kRefNone;
    if (cur.vehicleRef > kMaxEntityId) cur.vehicleRef = kRefNone;
    cur.targetRef = refOf(read.target);

    EntityState& sent = s.sent;
    bool key = s.fresh || nowMs - s.lastKeyMs >= kKeyframeMs;
    auto changed = [&](int group, bool differs) {
        if (differs || s.fresh) s.changedMs[group] = nowMs;
        return key || nowMs - s.changedMs[group] <= kRedundancyMs;
    };

    EntityState out = cur;
    out.id = s.id;
    out.fields = 0;
    if (changed(kGroupStatic, cur.outfit != sent.outfit) || nowMs - s.createdMs <= kRedundancyMs) out.fields |= EF_Static;
    sent.outfit = cur.outfit;
    if (changed(kGroupPos, distance(cur.pos, sent.pos) > 0.03f)) out.fields |= EF_Pos;
    if (changed(kGroupHeading, std::fabs(angleDelta(sent.heading, cur.heading)) > 0.5f)) out.fields |= EF_Heading;
    if (s.kind == EntityKind::Vehicle &&
        changed(kGroupTilt, std::fabs(cur.pitch - sent.pitch) > 1.0f || std::fabs(cur.roll - sent.roll) > 1.0f))
        out.fields |= EF_Tilt;
    if (changed(kGroupVel, distance(cur.vel, sent.vel) > 0.2f)) out.fields |= EF_Vel;
    if (s.kind == EntityKind::Ped && changed(kGroupHealth, cur.health != sent.health)) out.fields |= EF_Health;
    bool stateDiffers = cur.flags != sent.flags || cur.weapon != sent.weapon || cur.mountRef != sent.mountRef ||
                        cur.vehicleRef != sent.vehicleRef || cur.seat != sent.seat || cur.targetRef != sent.targetRef;
    if (changed(kGroupState, stateDiffers)) out.fields |= EF_State;

    // Remember what the guest now has. Fields under threshold keep their old value, so slow
    // drift still accumulates until it is worth sending.
    if (out.fields & EF_Pos) sent.pos = cur.pos;
    if (out.fields & EF_Heading) sent.heading = cur.heading;
    if (out.fields & EF_Tilt) {
        sent.pitch = cur.pitch;
        sent.roll = cur.roll;
    }
    if (out.fields & EF_Vel) sent.vel = cur.vel;
    sent.health = cur.health;
    sent.flags = cur.flags;
    sent.weapon = cur.weapon;
    sent.mountRef = cur.mountRef;
    sent.vehicleRef = cur.vehicleRef;
    sent.seat = cur.seat;
    sent.targetRef = cur.targetRef;
    sent.kind = cur.kind;

    if (key) s.lastKeyMs = s.fresh ? nowMs - (s.id * 97u) % kKeyframeMs : nowMs;  // stagger keyframes
    s.fresh = false;
    // While the guest watches the host's scene through the camera, the scene gets the full rate.
    uint32_t ticks = updateInterval(s.guestDistance, cur);
    if (story_.boostStreaming() && distance(cur.pos, local_.state.pos) < 60.0f) ticks = 1;
    s.nextDueMs = nowMs + ticks * kWorldIntervalMs - 5;
    if (out.fields) outgoing_.push_back(out);
}

// ---------------------------------------------------------------- driving

void CoopSession::movePed(EntityHandle h, Mover& m, const MotionSample& t, float dt, uint32_t nowMs) {
    Vec3 pos = game_.position(h);
    Vec3 err = t.pos - pos;
    float dist = length(err);
    float speed = horizontalSpeed(t.vel);
    float snap = 5.0f + speed * 0.5f;  // faster movers (horses) get more slack before a warp
    if ((m.mode == Mover::None && dist > 1.0f) || dist > snap) {
        game_.teleport(h, t.pos, t.heading);
        m.mode = Mover::None;
        m.lastTaskMs = nowMs;
        return;
    }
    if (speed < 0.35f && dist < 0.5f) {
        if (m.mode != Mover::Idle) {
            game_.standStill(h, t.heading);
            m.mode = Mover::Idle;
            m.lastTaskMs = nowMs;
        } else if (nowMs - m.lastFaceMs >= 100 && std::fabs(angleDelta(game_.heading(h), t.heading)) > 4.0f) {
            game_.face(h, t.heading);  // turn on the spot smoothly, following the stream
            m.lastFaceMs = nowMs;
        }
        return;
    }
    // Head for where the entity will be shortly, so it never stops between updates, and
    // walk a little faster while behind to close the gap.
    Vec3 goal = t.pos + Vec3{t.vel.x, t.vel.y, 0} * 0.5f;
    float want = std::max(speed, 0.8f) * (1.0f + std::clamp(dist * 0.3f, 0.0f, 0.6f));
    bool retask = m.mode != Mover::Moving || nowMs - m.lastTaskMs >= 500 || distance(goal, m.lastGoal) > 1.0f ||
                  std::fabs(want - m.lastSpeed) > 0.25f * std::max(m.lastSpeed, 1.0f);
    if (retask) {
        game_.moveTo(h, goal, want, t.heading);
        m.mode = Mover::Moving;
        m.lastTaskMs = nowMs;
        m.lastGoal = goal;
        m.lastSpeed = want;
    }
    if (config_.softCorrection && dist > 0.35f) {
        Vec3 flat{err.x, err.y, 0};
        game_.nudge(h, flat * std::min(1.0f, dt * 3.0f));
    }
}

void CoopSession::steerVehicle(EntityHandle h, Mover& m, const MotionSample& t, float dt) {
    Vec3 pos = game_.position(h);
    Vec3 err = t.pos - pos;
    float dist = length(err);
    float snap = 8.0f + length(t.vel) * 0.6f;
    if ((m.mode == Mover::None && dist > 2.0f) || dist > snap) {
        game_.teleport(h, t.pos, t.heading);
        m.mode = Mover::Moving;
        return;
    }
    m.mode = Mover::Moving;
    // Physics-friendly: steer with velocity (follow the stream + pull onto the path).
    Vec3 pull = err * 2.0f;
    float pullLen = length(pull);
    if (pullLen > 6.0f) pull = pull * (6.0f / pullLen);
    game_.steerVehicle(h, t.vel + pull, t.heading, t.pitch, t.roll, dt);
}

void CoopSession::forwardDamage(EntityHandle h, int& expectedHealth, uint16_t target, bool onlyLocalPlayer) {
    int current = game_.health(h);
    if (current < expectedHealth) {
        bool byLocalPlayer = game_.wasDamagedByLocalPlayer(h);
        bool forward;
        if (onlyLocalPlayer) {
            // A linked mission NPC also fights with its own AI here; the host simulates the same
            // fight, so only the guest's own hits count. The rest is undone by the host's health.
            forward = byLocalPlayer;
        } else if (target != kRefGuestPlayer && target != kRefHostPlayer) {
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

// ---------------------------------------------------------------- the friend's character

void CoopSession::updatePuppet(uint32_t nowMs, float dt) {
    status_.peerVisible = false;
    if (!puppet_.haveState) return;
    Puppet& p = puppet_;
    const PlayerState& s = p.state;
    status_.peerPos = s.pos;
    status_.peerDistance = haveLocal_ ? distance(local_.state.pos, s.pos) : 0;
    status_.peerDead = (s.flags & PF_Dead) != 0;
    status_.peerInMission = (s.flags & PF_InMission) != 0;
    status_.peerInCutscene = (s.flags & PF_Cutscene) != 0;
    uint16_t peerRef = isHost() ? kRefGuestPlayer : kRefHostPlayer;

    if (p.ped && !game_.exists(p.ped)) {  // the game removed it behind our back
        p.ped = 0;
        p.ridingOn = p.seatedIn = 0;
        p.weapon = 0;
        p.mover = Mover{};
    }
    if (p.ownMount && !game_.exists(p.ownMount)) p.ownMount = 0;
    if (p.ownVehicle && !game_.exists(p.ownVehicle)) p.ownVehicle = 0;
    if (!p.ped) {
        const std::string& avatar = link_.peer().avatar;
        p.ped = game_.spawn(EntityKind::Ped, joaat(avatar.c_str()), s.pos, s.heading, SpawnRole::PlayerPuppet);
        if (!p.ped) return;
        game_.setHealth(p.ped, std::max<int>(s.health, 1), s.maxHealth);
        game_.setLabel(p.ped, link_.peer().name.c_str());
        p.expectedHealth = game_.health(p.ped);
        p.outfitApplied = 0;
    }
    if (p.outfit && p.outfit != p.outfitApplied) {
        game_.setOutfit(p.ped, p.outfit);
        p.outfitApplied = p.outfit;
    }
    status_.peerVisible = true;
    MotionSample target = p.motion.sample(nowMs);
    status_.bufferMs = static_cast<int>(p.motion.delayMs());

    if (s.flags & PF_Dead) {
        if (!game_.isDead(p.ped)) game_.kill(p.ped);
        p.expectedHealth = 0;
        return;
    }
    forwardDamage(p.ped, p.expectedHealth, peerRef);
    if (game_.isDead(p.ped)) {
        // Dead here but alive for its owner (they respawned, or survived the hit): recreate it.
        if (!p.deadSinceMs) p.deadSinceMs = nowMs ? nowMs : 1;
        else if (nowMs - p.deadSinceMs > kRespawnGraceMs) destroyPuppet();
        return;
    }
    p.deadSinceMs = 0;
    if (s.health != p.expectedHealth && s.health > 0) {
        game_.setHealth(p.ped, s.health, s.maxHealth);
        p.expectedHealth = game_.health(p.ped);
    }

    // --- what the friend is sitting on: a shared entity, or one of their own
    EntityHandle wantMount = 0, wantVehicle = 0;
    bool controlsShared = false;  // they steer an entity of the shared world (host side moves it)
    if (s.flags & PF_InVehicle) {
        wantVehicle = resolveRef(s.vehicleRef);
        if (!wantVehicle && s.vehicleRef == kRefNone && s.vehicleModel) {
            if (p.ownVehicle && p.ownVehicleModel != s.vehicleModel) {
                game_.remove(p.ownVehicle);
                p.ownVehicle = 0;
            }
            if (!p.ownVehicle) {
                p.ownVehicle = game_.spawn(EntityKind::Vehicle, s.vehicleModel, s.pos, s.heading, SpawnRole::PuppetVehicle);
                p.ownVehicleModel = s.vehicleModel;
            }
            wantVehicle = p.ownVehicle;
        } else if (wantVehicle && s.seat == -1) {
            controlsShared = isHost();
        }
    } else if (s.flags & PF_OnMount) {
        wantMount = resolveRef(s.mountRef);
        if (!wantMount && s.mountRef == kRefNone && s.mountModel) {
            if (p.ownMount && p.ownMountModel != s.mountModel) {
                if (p.ridingOn == p.ownMount) {
                    game_.setMount(p.ped, 0);
                    p.ridingOn = 0;
                }
                game_.remove(p.ownMount);
                p.ownMount = 0;
            }
            if (!p.ownMount) {
                p.ownMount = game_.spawn(EntityKind::Ped, s.mountModel, s.pos, s.heading, SpawnRole::PuppetMount);
                p.ownMountModel = s.mountModel;
            }
            wantMount = p.ownMount;
        } else if (wantMount) {
            controlsShared = isHost();
        }
    }

    if (wantVehicle != p.seatedIn || (wantVehicle && s.seat != p.seat)) {
        if (p.ridingOn) {
            game_.setMount(p.ped, 0);
            p.ridingOn = 0;
        }
        game_.setVehicle(p.ped, wantVehicle, s.seat);
        if (!wantVehicle) game_.teleport(p.ped, target.pos, target.heading);
        p.seatedIn = wantVehicle;
        p.seat = s.seat;
        p.mover = p.bodyMover = Mover{};
    }
    if (!wantVehicle && wantMount != p.ridingOn) {
        if (wantMount && wantMount == p.ownMount) game_.teleport(p.ownMount, target.pos, target.heading);
        game_.setMount(p.ped, wantMount);
        if (!wantMount) game_.teleport(p.ped, target.pos, target.heading);
        p.ridingOn = wantMount;
        p.mover = p.bodyMover = Mover{};
    }
    // Own rides that are no longer used disappear after a while (or at once if far away).
    auto expire = [&](EntityHandle& h, uint32_t& unusedSince, bool inUse) {
        if (!h) return;
        if (inUse) {
            unusedSince = 0;
            return;
        }
        if (!unusedSince) unusedSince = nowMs ? nowMs : 1;
        if (nowMs - unusedSince > kOwnRideKeepMs || distance(game_.position(h), s.pos) > 120.0f) {
            game_.remove(h);
            h = 0;
            unusedSince = 0;
        }
    };
    expire(p.ownMount, p.ownMountUnusedMs, p.ridingOn == p.ownMount);
    expire(p.ownVehicle, p.ownVehicleUnusedMs, p.seatedIn == p.ownVehicle);

    if (s.weapon != p.weapon) {
        game_.setWeapon(p.ped, s.weapon);
        p.weapon = s.weapon;
    }
    bool crouch = (s.flags & PF_Crouching) != 0;
    if (crouch != p.crouched) {
        game_.setCrouch(p.ped, crouch);
        p.crouched = crouch;
    }
    bool ragdoll = (s.flags & PF_Ragdoll) != 0;
    if (ragdoll && !p.ragdolled && !p.seatedIn && !p.ridingOn) game_.ragdoll(p.ped);
    p.ragdolled = ragdoll;

    // --- motion
    bool ownRide = (p.seatedIn && p.seatedIn == p.ownVehicle) || (p.ridingOn && p.ridingOn == p.ownMount);
    if (p.seatedIn) {
        if (p.seat == -1 && (ownRide || controlsShared)) steerVehicle(p.seatedIn, p.bodyMover, target, dt);
    } else if (p.ridingOn) {
        if (ownRide || controlsShared) movePed(p.ridingOn, p.bodyMover, target, dt, nowMs);
    }
    bool shooting = (s.flags & PF_Shooting) != 0;
    bool aiming = (s.flags & PF_Aiming) != 0;
    bool onFoot = !p.seatedIn && !p.ridingOn;
    // Mounted or seated the arms are free; on foot aiming replaces walking only when (nearly) standing.
    bool aimTask = shooting || (aiming && (!onFoot || horizontalSpeed(target.vel) < 1.0f));
    if (aimTask) {
        uint32_t interval = shooting ? 200u : 400u;
        if (!p.aimingTask || nowMs - p.lastAimMs >= interval) {
            if (shooting) game_.shootAt(p.ped, blankTarget(s.aim));
            else game_.aimAt(p.ped, s.aim);
            p.lastAimMs = nowMs;
            p.aimingTask = true;
            if (onFoot) p.mover.mode = Mover::None;  // walking resumes with a fresh task
        }
        if (onFoot && distance(game_.position(p.ped), target.pos) > 4.0f)
            game_.teleport(p.ped, target.pos, target.heading);  // keep a shooting puppet from drifting away
    } else {
        p.aimingTask = false;
        if (onFoot && !p.ragdolled) movePed(p.ped, p.mover, target, dt, nowMs);
    }
}

// ---------------------------------------------------------------- the host's world, on the guest

void CoopSession::bindMissionEntities(uint32_t nowMs) {
    if (config_.storyMode != StoryMode::Shared || nowMs - lastMissionScanMs_ < kMissionScanMs) return;
    lastMissionScanMs_ = nowMs;
    missionLocal_.clear();
    game_.listMissionEntities(missionLocal_);
    if (missionLocal_.empty()) return;
    // Both games run the same mission script, which creates the same NPCs (same model, same place).
    // Pair each host mission entity with the guest's own copy: the copy keeps its AI and its role in
    // the guest's mission, while health, death and position follow the host. A mission objective
    // like "kill them all" therefore completes in both games.
    for (auto& [id, m] : mirrors_) {
        if (m.bound || !m.st.script || !m.haveStatic || !m.havePos) continue;
        const MissionEntity* best = nullptr;
        float bestDistance = kBindDistance;
        for (const MissionEntity& e : missionLocal_) {
            if (e.kind != m.st.kind || e.model != m.st.model || e.script != m.st.script) continue;
            if (mirrorByHandle_.count(e.handle) || isSessionEntity(e.handle)) continue;
            float d = distance(e.pos, m.st.pos);
            if (d < bestDistance) {
                bestDistance = d;
                best = &e;
            }
        }
        if (!best || !game_.exists(best->handle)) continue;
        if (m.handle) destroyMirror(m, true);  // replace the stand-in shown so far
        m.handle = best->handle;
        m.bound = true;
        m.mover = Mover{};
        m.lastUpdateMs = 0;
        m.deadSinceMs = 0;
        m.outfitApplied = m.st.outfit;  // it is the real one: already dressed by its script
        mirrorByHandle_[m.handle] = id;
        game_.setHarmless(m.handle, true);
        m.expectedHealth = game_.health(m.handle);
    }
}

void CoopSession::followBound(EntityHandle h, const MotionSample& t, float dt) {
    // The local AI animates it; only keep it where the host's copy is.
    Vec3 err = t.pos - game_.position(h);
    float dist = length(err);
    if (dist > 12.0f) {
        game_.teleport(h, t.pos, t.heading);
    } else if (dist > 0.75f) {
        Vec3 flat{err.x, err.y, 0};
        game_.nudge(h, flat * std::min(1.0f, dt * 2.0f));
    }
}

void CoopSession::updateMirrors(uint32_t nowMs, float dt) {
    spawnQueue_.clear();
    bindMissionEntities(nowMs);
    const bool shared = config_.storyMode == StoryMode::Shared;
    const Vec3 view = story_.viewPoint(local_.state.pos);
    int shown = 0;
    for (auto it = mirrors_.begin(); it != mirrors_.end();) {
        Mirror& m = it->second;
        if (nowMs - m.lastHeardMs > kMirrorTimeoutMs) {  // the host stopped mentioning it
            destroyMirror(m, true);
            it = mirrors_.erase(it);
            continue;
        }
        ++it;
        if (m.handle && !game_.exists(m.handle)) {
            mirrorByHandle_.erase(m.handle);
            m.handle = 0;
            m.ridingOn = m.seatedIn = 0;
            m.weapon = 0;
            m.mover = Mover{};
        }
        if (!m.handle) {
            // A mission entity may get its local twin in a moment: do not show a stand-in right away.
            bool awaitTwin = shared && m.st.script && nowMs - m.firstHeardMs < kBindWaitMs;
            if (m.haveStatic && m.havePos && !awaitTwin) spawnQueue_.push_back(&m);
            continue;
        }
        ++shown;
        if (m.bound) {
            float step = m.lastUpdateMs ? std::clamp((nowMs - m.lastUpdateMs) / 1000.0f, 0.0f, 0.25f) : dt;
            m.lastUpdateMs = nowMs;
            MotionSample t = m.motion.sample(nowMs);
            if (m.st.kind == EntityKind::Vehicle) {
                if (!(local_.vehicle == m.handle && local_.state.seat == -1)) steerVehicle(m.handle, m.mover, t, step);
                continue;
            }
            if (m.st.flags & NF_Dead) {
                if (!game_.isDead(m.handle)) game_.kill(m.handle);  // the guest's mission sees the kill
                m.expectedHealth = 0;
                continue;
            }
            forwardDamage(m.handle, m.expectedHealth, m.id, true);
            if (game_.isDead(m.handle)) {
                if (!m.deadSinceMs) m.deadSinceMs = nowMs ? nowMs : 1;
                else if (nowMs - m.deadSinceMs > kRespawnGraceMs && m.st.health > 0) {
                    game_.revive(m.handle, m.st.health);  // the host says it survived
                    m.expectedHealth = game_.health(m.handle);
                    m.deadSinceMs = 0;
                }
                continue;
            }
            m.deadSinceMs = 0;
            if (m.st.health > 0 && m.st.health != m.expectedHealth) {
                game_.setHealth(m.handle, m.st.health, 0);
                m.expectedHealth = game_.health(m.handle);
            }
            if (local_.mount != m.handle) followBound(m.handle, t, step);
            continue;
        }
        if (m.st.outfit && m.st.outfit != m.outfitApplied && m.st.kind == EntityKind::Ped) {
            game_.setOutfit(m.handle, m.st.outfit);
            m.outfitApplied = m.st.outfit;
        }
        // CPU level of detail: what is far from the guest is updated every 2nd or 4th frame
        // (staggered by id so the work spreads evenly). Motion is time-based, so nothing slows down.
        float range = distance(m.st.pos, view);
        uint32_t every = range < 35.0f ? 1u : range < 80.0f ? 2u : 4u;
        if (m.st.kind == EntityKind::Vehicle) every = std::min(every, 2u);  // physics drifts if left alone longer
        if ((frame_ + m.id) % every != 0 && m.lastUpdateMs) continue;
        float step = m.lastUpdateMs ? std::clamp((nowMs - m.lastUpdateMs) / 1000.0f, 0.0f, 0.25f) : dt;
        m.lastUpdateMs = nowMs;
        if (m.st.kind == EntityKind::Vehicle) {
            MotionSample t = m.motion.sample(nowMs);
            // The guest is driving it: the guest's game moves it and the host follows.
            if (local_.vehicle == m.handle && local_.state.seat == -1) continue;
            steerVehicle(m.handle, m.mover, t, step);
        } else {
            updateMirrorPed(m, nowMs, step);
        }
    }
    // Spawn the closest first, vehicles before peds so riders find their seats.
    std::sort(spawnQueue_.begin(), spawnQueue_.end(), [&](const Mirror* a, const Mirror* b) {
        if (a->st.kind != b->st.kind) return a->st.kind == EntityKind::Vehicle;
        return distance(a->st.pos, view) < distance(b->st.pos, view);
    });
    int budget = kSpawnsPerFrame;
    prefetched_.clear();
    for (Mirror* m : spawnQueue_) {
        if (budget <= 0) {
            // Over this frame's budget: at least get the model streaming so it spawns instantly later.
            if (prefetched_.size() < 8 && std::find(prefetched_.begin(), prefetched_.end(), m->st.model) == prefetched_.end()) {
                game_.prefetchModel(m->st.model);
                prefetched_.push_back(m->st.model);
            }
            continue;
        }
        --budget;
        MotionSample t = m->motion.empty() ? MotionSample{} : m->motion.sample(nowMs);
        Vec3 pos = m->motion.empty() ? m->st.pos : t.pos;
        m->handle = obtain(m->st.kind, m->st.model, pos, m->st.heading);
        if (!m->handle) continue;
        mirrorByHandle_[m->handle] = m->id;
        m->weapon = 0xFFFFFFFFu;  // force weapon / outfit to be applied (pooled entities keep old ones)
        m->outfitApplied = 0;
        m->lastUpdateMs = 0;
        if (m->st.kind == EntityKind::Ped) {
            if (!(m->st.flags & NF_Dead) && m->st.health > 0) game_.setHealth(m->handle, m->st.health, 0);
            m->expectedHealth = game_.health(m->handle);
        }
        ++shown;
    }
    status_.syncedEntities = shown;
    flushPool(nowMs, false);
}

void CoopSession::updateMirrorPed(Mirror& m, uint32_t nowMs, float dt) {
    MotionSample t = m.motion.sample(nowMs);
    if (m.st.flags & NF_Dead) {
        if (!game_.isDead(m.handle)) game_.kill(m.handle);
        m.expectedHealth = 0;
        return;
    }
    forwardDamage(m.handle, m.expectedHealth, m.id);
    if (game_.isDead(m.handle)) {
        // We killed it locally; if the host disagrees for long enough, bring it back.
        if (!m.deadSinceMs) m.deadSinceMs = nowMs ? nowMs : 1;
        else if (nowMs - m.deadSinceMs > kRespawnGraceMs) destroyMirror(m, false);
        return;
    }
    m.deadSinceMs = 0;
    if (m.st.health > 0 && m.st.health != m.expectedHealth) {
        game_.setHealth(m.handle, m.st.health, 0);
        m.expectedHealth = game_.health(m.handle);
    }

    // Seats and saddles follow the host's references.
    EntityHandle vehicle = resolveRef(m.st.vehicleRef);
    EntityHandle mount = vehicle ? 0 : resolveRef(m.st.mountRef);
    if (vehicle != m.seatedIn || (vehicle && m.seat != m.st.seat)) {
        if (m.ridingOn) {
            game_.setMount(m.handle, 0);
            m.ridingOn = 0;
        }
        game_.setVehicle(m.handle, vehicle, m.st.seat);
        if (!vehicle) game_.teleport(m.handle, t.pos, t.heading);
        m.seatedIn = vehicle;
        m.seat = m.st.seat;
        m.mover = Mover{};
    }
    if (!vehicle && mount != m.ridingOn) {
        game_.setMount(m.handle, mount);
        if (!mount) game_.teleport(m.handle, t.pos, t.heading);
        m.ridingOn = mount;
        m.mover = Mover{};
    }

    if (m.st.weapon != m.weapon) {
        game_.setWeapon(m.handle, m.st.weapon);
        m.weapon = m.st.weapon;
    }
    bool ragdoll = (m.st.flags & NF_Ragdoll) != 0;
    if (ragdoll && !m.ragdolled && !m.seatedIn && !m.ridingOn) game_.ragdoll(m.handle);
    m.ragdolled = ragdoll;

    bool shooting = (m.st.flags & NF_Shooting) != 0;
    bool aiming = (m.st.flags & NF_Aiming) != 0;
    if ((shooting || aiming) && nowMs - m.lastShotMs >= (shooting ? 300u : 600u)) {
        bool found = false;
        Vec3 aim = refPosition(m.st.targetRef, found);
        if (!found) aim = forwardPoint(t.pos, t.heading, 25.0f);
        if (shooting) game_.shootAt(m.handle, blankTarget(aim));
        else game_.aimAt(m.handle, aim);
        m.lastShotMs = nowMs;
        m.mover.mode = Mover::None;
    }

    // The guest rides this horse: the guest's game moves it.
    bool guestRides = local_.mount == m.handle;
    bool standingShooter = (shooting || aiming) && horizontalSpeed(t.vel) < 1.0f;
    if (!m.seatedIn && !m.ridingOn && !guestRides && !m.ragdolled && !standingShooter)
        movePed(m.handle, m.mover, t, dt, nowMs);
}

void CoopSession::clearForeign(uint32_t nowMs) {
    if (lastForeignMs_ != 0 && nowMs - lastForeignMs_ < kForeignScanMs) return;
    lastForeignMs_ = nowMs ? nowMs : 1;

    auto ours = [&](EntityHandle h) {
        return isSessionEntity(h) || mirrorByHandle_.count(h) || game_.belongsToLocalPlayer(h);
    };
    foreign_.clear();
    game_.listForeign(foreign_);
    for (const ForeignEntity& f : foreign_) {
        if (ours(f.handle) || hidden_.count(f.handle)) continue;
        if (f.mission && config_.storyMode == StoryMode::Shared) continue;  // the guest's own mission needs them
        if (f.draftOf && ours(f.draftOf)) continue;  // horses pulling one of our wagons
        if (game_.removeForeign(f.handle) == ForeignAction::Hidden) hidden_.insert(f.handle);
    }
    // Forget hidden entities the game has removed in the meantime.
    for (auto it = hidden_.begin(); it != hidden_.end();) it = game_.exists(*it) ? std::next(it) : hidden_.erase(it);
}

}  // namespace coop
