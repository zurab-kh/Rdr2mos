#include "session.h"

#include <algorithm>
#include <cmath>

namespace coop {

namespace {
constexpr uint32_t kPlayerIntervalMs = 33;       // ~30 Hz own character
constexpr uint32_t kIdlePlayerIntervalMs = 200;  // standing still: 5 Hz is plenty
constexpr uint32_t kStopRepeatMs = 250;          // after stopping, the full rate continues this long
constexpr uint32_t kWorldIntervalMs = 66;        // ~15 Hz world tick; entities update every 1, 2 or 4 ticks
constexpr uint32_t kInfoIntervalMs = 2000;
constexpr uint32_t kKeyframeMs = 1000;           // every streamed entity is fully refreshed this often
constexpr uint32_t kRedundancyMs = 200;          // a change is repeated this long, covering lost packets
constexpr uint32_t kRemovalResendMs = 300;
constexpr uint32_t kMirrorTimeoutMs = 3000;
constexpr uint32_t kForeignScanMs = 500;
constexpr uint32_t kRespawnGraceMs = 1500;
constexpr uint32_t kOwnRideKeepMs = 30000;
constexpr int kSpawnsPerFrame = 3;
constexpr float kStreamHysteresis = 20.0f;       // entities leave the stream this much further out than they enter
constexpr size_t kPoolSize = 24;
constexpr uint32_t kPoolKeepMs = 10000;
constexpr uint32_t kBindWaitMs = 2500;           // shared story: wait this long for our own copy before a stand-in
constexpr uint32_t kMissionScanMs = 250;
constexpr float kBindDistance = 35.0f;
constexpr float kLeadMs = 500.0f;                // walking peds head for where they will be this much later
constexpr uint32_t kIdleRecheckMs = 500;         // a ped standing where it should is looked at this often
// Damage to one victim is added up and sent at most this often. Fire, poison, a drag or a trample
// take a little health every frame: one reliable message per frame and victim would be 60+ a
// second each, which the server's per-player budget cannot (and should not) carry.
constexpr uint32_t kDamageIntervalMs = 100;

constexpr int kGroupPos = 0, kGroupHeading = 1, kGroupTilt = 2, kGroupVel = 3, kGroupHealth = 4, kGroupState = 5,
              kGroupStatic = 6;

float horizontalSpeed(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// Remote characters fire "blanks": their shots are aimed above the target so the bullets the
// local game simulates do not hit anything. Real damage only travels through Damage messages,
// decided by the machine that owns the shooter, which keeps all worlds in agreement.
Vec3 blankTarget(Vec3 p) {
    p.z += 1.8f;
    return p;
}

Vec3 forwardPoint(const Vec3& pos, float heading, float dist) {
    float rad = heading * 3.14159265f / 180.0f;
    return {pos.x - std::sin(rad) * dist, pos.y + std::cos(rad) * dist, pos.z + 1.0f};
}

// How often (in world ticks) an entity is refreshed, by distance to the nearest other player.
uint32_t updateInterval(float viewerDistance, const EntityState& st) {
    if (st.flags & (NF_Shooting | NF_InCombat | NF_Aiming)) return viewerDistance < 60.0f ? 1 : 2;
    if (st.kind == EntityKind::Vehicle && length(st.vel) > 3.0f) return viewerDistance < 80.0f ? 1 : 2;
    if (viewerDistance < 30.0f) return 1;
    if (viewerDistance < 80.0f) return 2;
    return 4;
}
}  // namespace

CoopSession::CoopSession(IGame& game, Link& link, const SessionConfig& config)
    : game_(game),
      link_(link),
      config_(config),
      story_(game, link, config_, [this](const std::string& t) { notice(t); }),
      character_(game, link, config_, [this](const std::string& t) { notice(t); }) {
    nearby_.reserve(512);
    outgoing_.reserve(128);
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

void CoopSession::tick(uint32_t nowMs) {
    float dt = lastTickMs_ ? std::clamp(static_cast<float>(nowMs - lastTickMs_) / 1000.0f, 0.0f, 0.25f) : 0.016f;
    lastTickMs_ = nowMs;

    link_.update(nowMs);
    if (!link_.connected()) {
        if (active_) cleanup();
        return;
    }
    if (link_.epoch() != linkEpoch_) {  // new connection to the server: start from a clean slate
        cleanup();
        linkEpoch_ = link_.epoch();
    }
    active_ = true;

    handleMessages(nowMs);
    if (myId_ && haveRoster_ && game_.ready()) {
        ++frame_;
        localPed_ = game_.localPlayerPed();
        sendLocalState(nowMs);
        character_.tick(nowMs, local_.state, storyHeroNeeded());
        status_.playingHero = character_.playingHero();
        localPed_ = game_.localPlayerPed();  // a new character is a new ped
        if (isOwner()) sendWorld(nowMs);
        updatePuppets(nowMs, dt);
        peers_.clear();
        for (const auto& [id, p] : puppets_)
            if (p.haveState) peers_.push_back({id, p.state.flags, p.state.pos});
        story_.tick(nowMs, local_.state, peers_, owner_, myId_);
        status_.holding = story_.holding();
        status_.spectating = story_.spectating();
        status_.missionMismatch = story_.missionMismatch();
        status_.saveProgress = story_.saveProgress();
        if (!isOwner()) {
            if (config_.worldSync && ownerWorldSync_) {
                game_.suppressAmbientPopulation();
                updateMirrors(nowMs, dt);
                clearForeign(nowMs);
            } else if (!mirrors_.empty() || !hidden_.empty()) {
                restoreWorld();
            }
        }
        flushDamage(nowMs);
    }
    link_.flush(nowMs);
}

// The world as shared with the others (owner side and mirror side); players are kept.
void CoopSession::resetWorld() {
    restoreWorld();
    streamed_.clear();
    handleById_.clear();
    removals_.clear();
    ownerWorldSync_ = false;
    lastWorldMs_ = lastInfoMs_ = lastForeignMs_ = 0;
    missionLocal_.clear();
    lastMissionScanMs_ = 0;
    // Hits on entities of the old world not sent yet: their ids would name something else now.
    for (auto it = damage_.begin(); it != damage_.end();) it = isPlayerRef(it->first) ? std::next(it) : damage_.erase(it);
    for (auto& [id, p] : puppets_) {
        // Seats and saddles referred to the old world.
        if (p.ridingOn && p.ridingOn != p.ownMount) {
            if (p.ped && game_.exists(p.ped)) game_.setMount(p.ped, 0);
            p.ridingOn = 0;
        }
        if (p.seatedIn && p.seatedIn != p.ownVehicle) {
            if (p.ped && game_.exists(p.ped)) game_.setVehicle(p.ped, 0, -1);
            p.seatedIn = 0;
        }
    }
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
    character_.reset();  // the story hero comes back when the co-op ends for good (heroRestore())
    for (auto& [id, p] : puppets_) destroyPuppet(p);
    puppets_.clear();
    resetWorld();
    damage_.clear();
    myId_ = owner_ = 0;
    worldEpoch_ = 0;
    haveRoster_ = false;
    haveLocal_ = false;
    lastStateMs_ = 0;
    lastSentFlags_ = 0;
    lastSentMoving_ = false;
    lastMovingMs_ = 0;
    ownVehicle_ = localPed_ = 0;
    story_.reset();
    std::string denied = status_.denied;
    status_ = SessionStatus{};
    status_.denied = denied;
    active_ = false;
}

bool CoopSession::teleportToPeer() {
    if (!game_.ready()) return false;
    const Puppet* target = nullptr;
    auto owner = puppets_.find(owner_);
    if (owner != puppets_.end() && owner->second.haveState) target = &owner->second;
    if (!target) {
        float best = 1e30f;
        for (const auto& [id, p] : puppets_)
            if (p.haveState && distance(p.state.pos, local_.state.pos) < best) {
                best = distance(p.state.pos, local_.state.pos);
                target = &p;
            }
    }
    if (!target) return false;
    Vec3 pos = target->state.pos;
    pos.x += 1.5f;
    game_.teleportLocalPlayer(pos);
    return true;
}

// ---------------------------------------------------------------- references

EntityHandle CoopSession::resolveRef(uint16_t ref) {
    if (ref == kRefNone || ref > kMaxEntityId) return 0;
    if (isOwner()) {
        auto it = handleById_.find(ref);
        return it != handleById_.end() && game_.exists(it->second) ? it->second : 0;
    }
    auto it = mirrors_.find(ref);
    return it != mirrors_.end() && it->second.handle && game_.exists(it->second.handle) ? it->second.handle : 0;
}

CoopSession::Puppet* CoopSession::puppetByPed(EntityHandle h) {
    if (!h) return nullptr;
    for (auto& [id, p] : puppets_)
        if (p.ped == h) return &p;
    return nullptr;
}

// A vehicle reference that may also name a player: their own vehicle, which exists as a copy in
// every game (a puppet vehicle elsewhere, the real one in their game).
EntityHandle CoopSession::resolveVehicle(uint16_t ref) {
    if (!isPlayerRef(ref)) return resolveRef(ref);
    uint8_t id = playerOfRef(ref);
    if (id == myId_) return ownVehicle_ && game_.exists(ownVehicle_) ? ownVehicle_ : 0;
    auto it = puppets_.find(id);
    return it != puppets_.end() && it->second.ownVehicle && game_.exists(it->second.ownVehicle) ? it->second.ownVehicle : 0;
}

uint16_t CoopSession::refOf(EntityHandle h) {
    if (!h) return kRefNone;
    if (h == localPed_) return playerRef(myId_);
    if (isOwner()) {
        auto it = streamed_.find(h);
        if (it != streamed_.end()) return it->second.id;
    } else {
        auto it = mirrorByHandle_.find(h);
        if (it != mirrorByHandle_.end()) return it->second;
    }
    // A player's own vehicle (ours, or a puppet's) is named after the player.
    if (h == ownVehicle_) return playerRef(myId_);
    for (const auto& [id, p] : puppets_)
        if (h == p.ped || h == p.ownVehicle) return playerRef(id);
    return kRefNone;
}

Vec3 CoopSession::refPosition(uint16_t ref, bool& found) {
    found = true;
    if (isPlayerRef(ref)) {
        uint8_t id = playerOfRef(ref);
        if (id == myId_) return local_.state.pos;
        auto it = puppets_.find(id);
        if (it == puppets_.end()) {
            found = false;
            return {};
        }
        if (it->second.ped && game_.exists(it->second.ped)) return game_.position(it->second.ped);
        found = it->second.haveState;
        return it->second.state.pos;
    }
    EntityHandle h = resolveRef(ref);
    found = h != 0;
    return h ? game_.position(h) : Vec3{};
}

bool CoopSession::isSessionEntity(EntityHandle h) const {
    if (!h) return false;
    for (const auto& [id, p] : puppets_)
        if (h == p.ped || h == p.ownMount || h == p.ownVehicle) return true;
    return inPool(h);
}

bool CoopSession::inPool(EntityHandle h) const {
    for (const Parked& p : pool_)
        if (p.handle == h) return true;
    return false;
}

uint16_t CoopSession::allocateId() {
    for (int tries = 0; tries < kMaxEntityId; ++tries) {
        uint16_t id = nextId_;
        nextId_ = nextId_ >= kMaxEntityId ? 1 : static_cast<uint16_t>(nextId_ + 1);
        if (!handleById_.count(id)) return id;
    }
    return 0;
}

// ---------------------------------------------------------------- entity lifetime

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

// The world owner got out of a vehicle of its own: from now on it streams it as part of the world.
// Our puppet copy of it (maybe with us or others in it) simply becomes the mirror, instead of a
// second wagon appearing on top of it.
EntityHandle CoopSession::takeOverOwnVehicle(uint32_t model, const Vec3& pos, uint16_t id) {
    for (auto& [pid, p] : puppets_) {
        EntityHandle h = p.ownVehicle;
        if (!h || p.ownVehicleModel != model || !game_.exists(h)) continue;
        // Another player's puppet still sitting in its own vehicle means it is not this one. The
        // world owner's can: its game streams the vehicle only once the owner got down, so a state
        // saying otherwise is out of date (the one that said "got down" was lost, and a standing
        // player's next state is up to 200 ms away, while the snapshot is repeated every world tick).
        if (p.seatedIn == h && pid != owner_) continue;
        if (distanceSq(game_.position(h), pos) > 16.0f) continue;
        p.ownVehicle = 0;
        p.ownVehicleUnusedMs = 0;
        if (pid == owner_ && (p.state.flags & PF_InVehicle) && p.state.vehicleRef == kRefNone)
            p.state.flags = static_cast<uint16_t>(p.state.flags & ~PF_InVehicle);
        // Whoever sits in it by the player's reference sits in the mirror now, which is what their
        // next states will say too; until then they stay seated instead of dropping out.
        retargetVehicleRefs(playerRef(pid), id);
        return h;
    }
    return 0;
}

// Puppets whose last state puts them in vehicle `from` are put in `to` instead: what that vehicle
// is called changed here, and their next state will say the same.
void CoopSession::retargetVehicleRefs(uint16_t from, uint16_t to) {
    for (auto& [id, q] : puppets_)
        if (q.haveState && (q.state.flags & PF_InVehicle) && q.state.vehicleRef == from) q.state.vehicleRef = to;
}

// A mirror vehicle we sit in as a passenger loses its original. The player driving it keeps it:
// their game adopts it, and from then on it is their own vehicle, so ours becomes our copy of it
// (their puppet's own vehicle) with us still in it, instead of a second one appearing next to it.
bool CoopSession::handOverToDriver(Mirror& m) {
    for (auto& [id, q] : puppets_) {
        if (!q.ped || q.seatedIn != m.handle || q.seat != -1) continue;
        if (q.ownVehicle && q.ownVehicle != m.handle && game_.exists(q.ownVehicle)) game_.remove(q.ownVehicle);
        q.ownVehicle = m.handle;
        q.ownVehicleModel = m.st.model;
        q.ownVehicleUnusedMs = 0;
        // The driver's next state names it as their own; assume that now, or the puppet would get
        // out until it arrives.
        if (q.haveState && q.state.vehicleRef == m.id) {
            q.state.vehicleRef = kRefNone;
            q.state.vehicleModel = m.st.model;
        }
        retargetVehicleRefs(m.id, playerRef(id));
        return true;
    }
    return false;
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

void CoopSession::destroyPuppet(Puppet& p, bool forGood) {
    if (p.ped && game_.exists(p.ped)) game_.remove(p.ped);
    for (EntityHandle* ride : {&p.ownMount, &p.ownVehicle}) {
        EntityHandle h = *ride;
        if (h && game_.exists(h) && localSitsIn(h)) {
            // The local player is on the friend's horse or wagon: it is never deleted under them.
            // The character only comes back in a moment: it keeps its ride.
            if (!forGood) continue;
            // The friend is gone for good: it stays as part of our own game, like a mirror does
            // when its original goes away, and the others now see it as ours.
            if (game_.adopt(h)) {
                if (h == local_.vehicle) {
                    ownVehicle_ = h;
                    retargetVehicleRefs(playerRef(p.id), playerRef(myId_));
                }
                *ride = 0;
                continue;
            }
        }
        if (h && game_.exists(h)) game_.remove(h);
        *ride = 0;
    }
    p.ped = 0;
    p.ridingOn = p.seatedIn = 0;
    p.weapon = 0;
    p.deadSinceMs = 0;
    p.crouched = p.ragdolled = p.aimingTask = false;
    p.mover = p.bodyMover = Mover{};
    p.lookApplied = false;
}

void CoopSession::destroyMirror(Mirror& m, bool recycle) {
    if (m.bound) {
        // Never delete our own mission entity: just stop driving it.
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
            if (m.st.kind == EntityKind::Vehicle && !localRides(m.handle) && localSitsIn(m.handle) &&
                handOverToDriver(m)) {
                // A passenger: the wagon goes on with the player who drives it (see handOverToDriver).
            } else if (localSitsIn(m.handle) && game_.adopt(m.handle)) {
                // The original is gone, but the player is on this one: it stays, as part of our
                // own game, instead of vanishing under them. A passenger with nobody at the reins
                // keeps it too. Whoever else sits in it now sits in our own vehicle.
                if (m.handle == local_.vehicle) {
                    ownVehicle_ = m.handle;
                    retargetVehicleRefs(m.id, playerRef(myId_));
                }
            } else if (recycle && pool_.size() < kPoolSize && !game_.isDead(m.handle)) {
                // Live entities are parked for reuse: the next mirror of the same model appears instantly.
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

void CoopSession::onRoster(const Roster& roster) {
    owner_ = roster.worldOwner;
    if (!haveRoster_ || roster.worldEpoch != worldEpoch_) {
        // A different world from now on. The owner's world info can overtake the first roster (it is
        // sent unreliably, the roster reliably): the server only passes on the current owner's, so
        // keep it, or the world would stay hidden until the owner's next one, two seconds later.
        bool keepInfo = !haveRoster_ && ownerWorldSync_;
        resetWorld();
        ownerWorldSync_ = keepInfo;
    }
    worldEpoch_ = roster.worldEpoch;
    haveRoster_ = true;
    // Puppets of players who left go away; newcomers get one when their state arrives.
    for (auto it = puppets_.begin(); it != puppets_.end();) {
        bool present = std::any_of(roster.players.begin(), roster.players.end(), [&](const RosterEntry& e) { return e.id == it->first; });
        if (!present) {
            destroyPuppet(it->second);
            it = puppets_.erase(it);
        } else {
            ++it;
        }
    }
    status_.players = static_cast<int>(roster.players.size());
    status_.worldOwner = isOwner();
    status_.ownerName.clear();
    status_.characters.clear();
    for (const RosterEntry& e : roster.players) {
        if (e.id == owner_) status_.ownerName = e.name;
        status_.characters.push_back({e.avatar, e.name});
        if (e.id == myId_) {
            character_.setMine(e.avatar);
            status_.character = e.avatar;
            continue;
        }
        Puppet& p = puppets_[e.id];
        p.id = e.id;
        p.name = e.name;
        if (p.avatar != e.avatar) {
            // Another character now: a new ped of that model takes the place of the old one (its
            // horse and wagon stay), dressed once the look for it arrives.
            if (!p.avatar.empty() && p.ped) {
                if (game_.exists(p.ped)) game_.remove(p.ped);
                p.ped = 0;
                p.ridingOn = p.seatedIn = 0;
                p.weapon = 0;
                p.deadSinceMs = 0;
                p.crouched = p.ragdolled = p.aimingTask = false;
                p.mover = p.bodyMover = Mover{};
            }
            if (!p.avatar.empty()) p.haveLook = false;
            p.avatar = e.avatar;
            p.lookApplied = false;
        }
    }
}

// Missions, scenes and the shops are made for the story hero: whoever chose another character is
// the hero again while one is on. In the shared story the world owner's mission is everybody's, and
// we should be the hero before we walk into its marker; in the owner's world only the owner plays.
HeroNeed CoopSession::storyHeroNeeded() const {
    const uint16_t f = local_.state.flags;
    if (f & (PF_InMission | PF_Cutscene | PF_Holding)) return HeroNeed::Now;
    if (story_.holding()) return HeroNeed::Now;
    // The world owner starts the missions, by walking into a marker nobody can see coming: it plays
    // as the story hero all along (the others still see its character).
    if (isOwner()) return HeroNeed::Now;
    if (mode_ == StoryMode::Shared && owner_) {
        auto it = puppets_.find(owner_);
        if (it != puppets_.end() && it->second.haveState && (it->second.state.flags & PF_InMission)) return HeroNeed::Soon;
    }
    return HeroNeed::None;
}

void CoopSession::handleMessages(uint32_t nowMs) {
    Msg type;
    std::vector<uint8_t> payload;
    while (link_.poll(type, payload)) {
        // Messages relayed from another player start with that player's id.
        uint8_t from = 0;
        const uint8_t* body = payload.data();
        size_t bodySize = payload.size();
        bool relayed = type == Msg::PlayerState || type == Msg::Appearance || type == Msg::Damage ||
                       type == Msg::Camera || type == Msg::Waypoint;
        if (relayed) {
            if (payload.empty()) continue;
            from = payload[0];
            ++body;
            --bodySize;
        }
        switch (type) {
            case Msg::Welcome: {
                Welcome w;
                if (!decode(payload, w)) break;
                myId_ = w.playerId;
                mode_ = w.storyMode;
                story_.setMode(mode_);
                story_.setCampaignSave(w.hasCampaignSave != 0, w.campaignSaveCrc, w.campaignSaveTime);
                status_.welcomed = true;
                status_.playerId = myId_;
                status_.storyMode = mode_;
                status_.campaignHasSave = w.hasCampaignSave != 0;
                status_.denied.clear();
                break;
            }
            case Msg::Denied: {
                Denied d;
                if (decode(payload, d)) status_.denied = d.reason;
                break;
            }
            case Msg::Roster: {
                Roster r;
                if (decode(payload, r)) {
                    onRoster(r);
                    story_.setCampaignSave(r.hasCampaignSave != 0, r.campaignSaveCrc, r.campaignSaveTime);
                    status_.campaignHasSave = r.hasCampaignSave != 0;
                }
                break;
            }
            case Msg::PlayerState: {
                PlayerState s;
                if (!decode(body, bodySize, s) || from == myId_) break;
                auto it = puppets_.find(from);
                if (it == puppets_.end()) break;  // not in the roster (yet)
                Puppet& p = it->second;
                if (p.haveState && static_cast<int32_t>(s.timeMs - p.state.timeMs) <= 0) break;
                p.state = s;
                p.haveState = true;
                MotionSample sample;
                sample.timeMs = s.timeMs;
                sample.pos = s.pos;
                sample.vel = s.vel;
                sample.hasVel = true;
                sample.heading = s.heading;
                p.motion.push(sample, nowMs);
                break;
            }
            case Msg::Appearance: {
                Appearance a;
                auto it = puppets_.find(from);
                if (decode(body, bodySize, a) && it != puppets_.end() && (!it->second.haveLook || a != it->second.look)) {
                    it->second.look = std::move(a);
                    it->second.haveLook = true;
                    it->second.lookApplied = false;
                }
                break;
            }
            case Msg::EntitySnapshot: {
                // Decoded into a reused snapshot: its entity list keeps its capacity across messages.
                if (!isOwner() && decode(payload, snapshot_)) onSnapshot(snapshot_, nowMs);
                break;
            }
            case Msg::Damage: {
                Damage d;
                if (!decode(body, bodySize, d) || d.amount == 0 || !game_.ready()) break;
                if (d.target == playerRef(myId_)) {
                    game_.damageLocalPlayer(d.amount);
                } else if (isOwner() && !isPlayerRef(d.target) && d.worldEpoch == worldEpoch_) {
                    // Only hits on entities of our world: each owner numbers its world from 1, so
                    // a hit sent before the sender heard of a handover names some other entity here.
                    EntityHandle victim = resolveRef(d.target);
                    auto attacker = puppets_.find(from);
                    if (victim) game_.damageNpc(victim, d.amount, attacker != puppets_.end() ? attacker->second.ped : 0);
                }
                break;
            }
            case Msg::WorldInfo: {
                WorldInfo info;
                if (isOwner() || !decode(payload, info)) break;
                ownerWorldSync_ = info.worldSync != 0;
                if (game_.ready()) game_.applyWorldInfo(info, config_.syncClock, config_.syncWeather);
                break;
            }
            case Msg::Camera:
            case Msg::Waypoint:
                story_.handle(type, from, body, bodySize, nowMs);
                break;
            case Msg::StoryCommand:
            case Msg::SaveChunk:
            case Msg::SaveResult:
                story_.handle(type, 0, payload.data(), payload.size(), nowMs);
                break;
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
                destroyMirror(m, true);  // the owner reused the id for something else
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
    s.flags |= story_.extraFlags();
    // The last vehicle of our own that we sat in (not a copy of someone else's): the others may
    // still sit in it after we got out.
    if (local_.vehicle && !mirrorByHandle_.count(local_.vehicle) && !isSessionEntity(local_.vehicle))
        ownVehicle_ = local_.vehicle;
    else if (ownVehicle_ && !game_.exists(ownVehicle_))
        ownVehicle_ = 0;
    // Full rate while anything happens, 5 Hz while standing still. Discrete changes (first shot,
    // mounting, starting to move) go out immediately instead of waiting for the next tick. A stop
    // keeps the full rate a moment longer: if the stopping update is lost, the next one follows
    // at once instead of leaving the others to guess for a fifth of a second.
    bool moving = horizontalSpeed(s.vel) > 0.15f || std::fabs(s.vel.z) > 0.5f ||
                  (s.flags & (PF_Aiming | PF_Shooting | PF_Ragdoll)) != 0;
    if (moving) lastMovingMs_ = nowMs ? nowMs : 1;
    bool active = moving || (lastMovingMs_ && nowMs - lastMovingMs_ < kStopRepeatMs);
    uint32_t interval = active ? kPlayerIntervalMs : kIdlePlayerIntervalMs;
    bool due = first || nowMs - lastStateMs_ >= interval;
    if (!due && s.flags == lastSentFlags_ && moving == lastSentMoving_) return;
    lastSentMoving_ = moving;
    s.timeMs = nowMs;
    s.mountRef = kRefNone;
    s.vehicleRef = kRefNone;
    if (s.flags & PF_OnMount) {
        s.mountRef = refOf(local_.mount);
        if (s.mountRef > kMaxEntityId) s.mountRef = kRefNone;
    }
    if (s.flags & PF_InVehicle) {
        // A shared-world vehicle by its id, or a seat in another player's own vehicle by that
        // player's reference (their copy of it exists in every game), or our own (kRefNone + model).
        s.vehicleRef = refOf(local_.vehicle);
        if (s.vehicleRef > kMaxEntityId && !(isPlayerRef(s.vehicleRef) && playerOfRef(s.vehicleRef) != myId_))
            s.vehicleRef = kRefNone;
    }
    lastStateMs_ = nowMs;
    lastSentFlags_ = s.flags;
    link_.send(Msg::PlayerState, s, false);
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

    // Everybody else looks at the world from their own position. points_[0] is the owner.
    Vec3 hostPos = local_.state.pos;
    points_.clear();
    points_.push_back(hostPos);
    for (const auto& [id, p] : puppets_)
        if (p.haveState) points_.push_back(p.state.pos);
    if (points_.size() == 1) {
        // Nobody to stream to; the server keeps what was sent before.
        status_.syncedEntities = 0;
        return;
    }

    // One query for all the points (the default runs one per pair); duplicates are merged below.
    nearby_.clear();
    float reach = config_.npcRadius + kStreamHysteresis;
    game_.listNearbyAny(points_.data(), points_.size(), reach, nearby_);
    if (points_.size() > 2) {
        std::sort(nearby_.begin(), nearby_.end(), [](const NearbyEntity& a, const NearbyEntity& b) { return a.handle < b.handle; });
        nearby_.erase(std::unique(nearby_.begin(), nearby_.end(),
                                  [](const NearbyEntity& a, const NearbyEntity& b) { return a.handle == b.handle; }),
                      nearby_.end());
    }

    // Closest to a viewer first; keep at most maxPeds / maxVehicles. Hysteresis keeps entities at
    // the edge (of the radius or of the budget) from popping in and out every tick.
    order_.clear();
    for (size_t i = 0; i < nearby_.size(); ++i) {
        const NearbyEntity& n = nearby_[i];
        if (isSessionEntity(n.handle)) continue;
        bool streamed = streamed_.count(n.handle) != 0;
        float viewer2 = 1e30f;
        for (size_t k = 1; k < points_.size(); ++k) viewer2 = std::min(viewer2, distanceSq(n.pos, points_[k]));
        float dv = std::sqrt(viewer2), dh = distance(n.pos, hostPos);
        if (!streamed && std::min(dv, dh) > config_.npcRadius) continue;
        float d = std::min(dv, dh + 15.0f);
        order_.push_back({streamed ? d - 10.0f : d, dv, i});
    }
    std::sort(order_.begin(), order_.end(), [](const Candidate& a, const Candidate& b) { return a.rank < b.rank; });

    for (auto& [h, s] : streamed_) s.seen = false;
    int peds = 0, vehicles = 0;
    for (const Candidate& cand : order_) {
        const NearbyEntity& n = nearby_[cand.index];
        int& count = n.kind == EntityKind::Vehicle ? vehicles : peds;
        int cap = n.kind == EntityKind::Vehicle ? config_.maxVehicles : config_.maxPeds;
        bool mustKeep = n.handle == local_.vehicle || puppetUses(n.handle);
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
        it->second.viewerDistance = cand.viewerDistance;
    }

    for (auto it = streamed_.begin(); it != streamed_.end();) {
        if (!it->second.seen && (puppetUses(it->first) || game_.belongsToLocalPlayer(it->first)) &&
            game_.exists(it->first)) {
            // A shared entity stays shared while we or another player use it, even though the game
            // stops listing what the local player uses: if we take a wagon everybody sees, they
            // keep seeing that one (with us on it) instead of it vanishing for a copy.
            Vec3 at = game_.position(it->first);
            float viewer2 = 1e30f;
            for (size_t k = 1; k < points_.size(); ++k) viewer2 = std::min(viewer2, distanceSq(at, points_[k]));
            if (std::min(viewer2, distanceSq(at, hostPos)) <= reach * reach) {
                it->second.seen = true;
                it->second.viewerDistance = std::sqrt(viewer2);
            }
        }
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
        if (!handleById_.count(it->first)) removedIds_.push_back(it->first);
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
    cur.vehicleRef = refOf(read.vehicle);  // a player ref: sitting in that player's own vehicle
    if (cur.mountRef > kMaxEntityId) cur.mountRef = kRefNone;
    if (cur.vehicleRef > kMaxEntityId && !isPlayerRef(cur.vehicleRef)) cur.vehicleRef = kRefNone;
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

    // Remember what the others now have. Fields under threshold keep their old value, so slow
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
    // While someone watches the owner's scene through the camera, the scene gets the full rate.
    uint32_t ticks = updateInterval(s.viewerDistance, cur);
    if (story_.boostStreaming() && distance(cur.pos, local_.state.pos) < 60.0f) ticks = 1;
    s.nextDueMs = nowMs + ticks * kWorldIntervalMs - 5;
    if (out.fields) outgoing_.push_back(out);
}

// ---------------------------------------------------------------- driving

void CoopSession::movePed(EntityHandle h, Mover& m, const MotionBuffer& motion, const MotionSample& t, float dt,
                          uint32_t nowMs) {
    float speed = horizontalSpeed(t.vel);
    // Standing on its spot, facing the right way, and the stream says the same as before: nothing
    // to do but look now and then whether the local game pushed it away.
    if (m.mode == Mover::Idle && m.still && speed < 0.35f && nowMs - m.lastCheckMs < kIdleRecheckMs &&
        distanceSq(t.pos, m.restPos) < 0.0025f && std::fabs(angleDelta(m.restHeading, t.heading)) < 2.0f)
        return;
    m.lastCheckMs = nowMs;
    m.restPos = t.pos;
    m.restHeading = t.heading;
    m.still = false;
    Vec3 pos = game_.position(h);
    Vec3 err = t.pos - pos;
    float dist = length(err);
    float snap = 5.0f + speed * 0.5f;  // faster movers (horses) get more slack before a warp
    if ((m.mode == Mover::None && dist > 1.0f) || dist > snap) {
        game_.teleport(h, t.pos, t.heading);
        m.mode = Mover::None;
        m.lastTaskMs = nowMs;
        return;
    }
    Vec3 goal;
    float want;
    if (speed < 0.35f) {
        // At rest. Standing still is entered only close to the spot and left only well away from
        // it, so a ped does not shuffle; a ped that stopped short takes a step onto the spot first
        // (for a while at most: some spots cannot be reached exactly).
        bool settled = m.mode == Mover::Idle ? dist < 0.6f
                                             : dist < 0.15f || (m.mode == Mover::Settle && nowMs - m.settleSinceMs > 1500);
        if (settled) {
            if (m.mode != Mover::Idle) {
                game_.standStill(h, t.heading);
                m.mode = Mover::Idle;
                m.lastTaskMs = nowMs;
            } else if (std::fabs(angleDelta(game_.heading(h), t.heading)) > 4.0f) {
                if (nowMs - m.lastFaceMs >= 100) {
                    game_.face(h, t.heading);  // turn on the spot smoothly, following the stream
                    m.lastFaceMs = nowMs;
                }
            } else {
                m.still = true;
            }
            return;
        }
        if (m.mode != Mover::Settle) {
            m.mode = Mover::Settle;
            m.settleSinceMs = nowMs;
            m.lastTaskMs = nowMs - 1000;  // re-task now
        }
        goal = t.pos;
        want = std::clamp(dist * 2.0f, 0.6f, 1.6f);
    } else {
        if (m.mode != Mover::Moving) m.lastTaskMs = nowMs - 1000;
        m.mode = Mover::Moving;
        // Head for where the entity will be in a moment, on its real path (the jitter buffer holds
        // the next part of it), so the ped never stops between updates and does not cut curves.
        // Speed follows the stream, a little faster while behind and slower while ahead.
        Vec3 dir{t.vel.x / speed, t.vel.y / speed, 0};
        float along = err.x * dir.x + err.y * dir.y;  // > 0: the ped is behind
        MotionSample ahead = motion.predict(kLeadMs);
        // A sharp turn within that time: aim at the corner first, or the ped cuts across it.
        MotionSample mid = motion.predict(kLeadMs * 0.5f);
        Vec3 first{mid.pos.x - t.pos.x, mid.pos.y - t.pos.y, 0}, second{ahead.pos.x - mid.pos.x, ahead.pos.y - mid.pos.y, 0};
        float l1 = length(first), l2 = length(second);
        if (l1 > 0.1f && l2 > 0.1f && first.x * second.x + first.y * second.y < 0.85f * l1 * l2) ahead = mid;
        goal = ahead.pos;
        // Ahead of where it should be: slow down, but never turn back (unless the path stops there).
        float forward = (goal.x - pos.x) * dir.x + (goal.y - pos.y) * dir.y;
        if (forward < 0.6f && horizontalSpeed(ahead.vel) > 0.35f) goal = goal + dir * (0.6f - forward);
        want = std::max(speed + std::clamp(along * 1.5f, -0.5f * speed, 0.5f * speed + 0.5f), 0.5f);
    }
    // A new task only when it says something different: another direction, another speed, or the
    // old goal is about to be reached (the ped would stop there).
    Vec3 toNew{goal.x - pos.x, goal.y - pos.y, 0}, toOld{m.lastGoal.x - pos.x, m.lastGoal.y - pos.y, 0};
    float newLen = length(toNew), oldLen = length(toOld);
    float turn = newLen > 1e-3f && oldLen > 1e-3f
                     ? (toNew.x * toOld.x + toNew.y * toOld.y) / (newLen * oldLen)
                     : -1.0f;  // cosine of the change of direction
    bool retask = nowMs - m.lastTaskMs >= 500 || turn < 0.995f || oldLen < 0.4f * newLen ||
                  std::fabs(want - m.lastSpeed) > 0.15f * std::max(m.lastSpeed, 1.0f);
    if (retask) {
        game_.moveTo(h, goal, want, t.heading);
        m.lastTaskMs = nowMs;
        m.lastGoal = goal;
        m.lastSpeed = want;
    }
    if (config_.softCorrection && dist > 0.35f && m.mode == Mover::Moving) {
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

void CoopSession::forwardDamage(EntityHandle h, int& expectedHealth, uint16_t target) {
    int current = game_.health(h);
    if (current < expectedHealth) {
        bool byLocalPlayer = game_.wasDamagedByLocalPlayer(h);
        bool forward;
        if (!isPlayerRef(target)) {
            // A copy of an owner's NPC lost health. Only our own hits count: everything else that
            // happens to it here (a linked mission NPC fighting with its own AI, a fall after a
            // correction, a collision with another copy) the owner simulates with the real one,
            // so passing it on would count it twice. The owner's health undoes the rest.
            forward = byLocalPlayer;
        } else if (isOwner()) {
            // The owner's NPCs hurt other players' puppets for real; the owner only with friendly fire.
            forward = !byLocalPlayer || config_.friendlyFire;
        } else {
            forward = byLocalPlayer && config_.friendlyFire;
        }
        if (forward) {
            PendingDamage& pending = damage_[target];
            pending.amount = std::min<uint32_t>(pending.amount + static_cast<uint32_t>(expectedHealth - current), 1u << 24);
        }
    }
    expectedHealth = current;
}

// The first hit on a victim goes out at once; what follows within kDamageIntervalMs is added up
// and sent as one message, so a burning NPC costs ~10 messages a second instead of one per frame.
void CoopSession::flushDamage(uint32_t nowMs) {
    for (auto it = damage_.begin(); it != damage_.end();) {
        PendingDamage& d = it->second;
        if (d.amount && (!d.lastSentMs || nowMs - d.lastSentMs >= kDamageIntervalMs)) {
            uint32_t amount = std::min<uint32_t>(d.amount, 65535);
            link_.send(Msg::Damage, Damage{it->first, static_cast<uint16_t>(amount), worldEpoch_}, true);
            d.amount -= amount;
            d.lastSentMs = nowMs ? nowMs : 1;
        }
        if (!d.amount && nowMs - d.lastSentMs >= kDamageIntervalMs) it = damage_.erase(it);
        else ++it;
    }
}

// ---------------------------------------------------------------- other players' characters

void CoopSession::updatePuppets(uint32_t nowMs, float dt) {
    driven_.clear();
    status_.peerVisible = false;
    status_.peerInCutscene = false;
    const Puppet* nearest = nullptr;
    float best = 1e30f;
    for (auto& [id, p] : puppets_) {
        updatePuppet(p, nowMs, dt);
        if (!p.haveState) continue;
        if (p.state.flags & PF_Cutscene) status_.peerInCutscene = true;
        float d = distance(local_.state.pos, p.state.pos);
        if (d < best) {
            best = d;
            nearest = &p;
        }
    }
    if (nearest) {
        status_.peerVisible = nearest->ped != 0;
        status_.peerName = nearest->name;
        status_.peerPos = nearest->state.pos;
        status_.peerDistance = best;
        status_.peerDead = (nearest->state.flags & PF_Dead) != 0;
        status_.peerInMission = (nearest->state.flags & PF_InMission) != 0;
        status_.bufferMs = static_cast<int>(nearest->motion.delayMs());
    }
}

void CoopSession::updatePuppet(Puppet& p, uint32_t nowMs, float dt) {
    if (!p.haveState) return;
    const PlayerState& s = p.state;
    uint16_t ref = playerRef(p.id);

    if (p.ped && !game_.exists(p.ped)) {  // the game removed it behind our back
        p.ped = 0;
        p.ridingOn = p.seatedIn = 0;
        p.weapon = 0;
        p.mover = Mover{};
        p.lookApplied = false;
    }
    if (p.ownMount && !game_.exists(p.ownMount)) p.ownMount = 0;
    if (p.ownVehicle && !game_.exists(p.ownVehicle)) p.ownVehicle = 0;
    if (!p.ped) {
        p.ped = game_.spawn(EntityKind::Ped, joaat(p.avatar.c_str()), s.pos, s.heading, SpawnRole::PlayerPuppet);
        if (!p.ped) return;
        game_.setHealth(p.ped, std::max<int>(s.health, 1), s.maxHealth);
        game_.setLabel(p.ped, p.name.c_str());
        p.expectedHealth = game_.health(p.ped);
        p.lookApplied = false;
    }
    if (p.haveLook && !p.lookApplied) {
        // Clothes fit their own model only: a look for another one (the player was someone else
        // when it was sent) waits for the next.
        if (!p.look.model || p.look.model == joaat(p.avatar.c_str())) game_.applyAppearance(p.ped, p.look);
        p.lookApplied = true;
    }
    MotionSample target = p.motion.sample(nowMs);

    if (s.flags & PF_Dead) {
        if (!game_.isDead(p.ped)) game_.kill(p.ped);
        p.expectedHealth = 0;
        return;
    }
    forwardDamage(p.ped, p.expectedHealth, ref);
    if (game_.isDead(p.ped)) {
        // Dead here but alive for its owner (they respawned, or survived the hit): recreate it.
        if (!p.deadSinceMs) p.deadSinceMs = nowMs ? nowMs : 1;
        else if (nowMs - p.deadSinceMs > kRespawnGraceMs) destroyPuppet(p, false);
        return;
    }
    p.deadSinceMs = 0;
    if (s.health != p.expectedHealth && s.health > 0) {
        game_.setHealth(p.ped, s.health, s.maxHealth);
        p.expectedHealth = game_.health(p.ped);
    }

    // --- what the player is sitting on: a shared entity, or one of their own
    EntityHandle wantMount = 0, wantVehicle = 0;
    // They steer an entity of the shared world. The owner moves the real one from their stream, and
    // every other game moves its copy from their stream too: fresher than the owner's echo of it
    // (one network hop instead of three) and exactly the motion the puppet itself shows.
    bool controlsShared = false;
    if (s.flags & PF_InVehicle) {
        wantVehicle = resolveVehicle(s.vehicleRef);
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
            // At the reins of a shared vehicle, or of another player's own one (their copy here, or
            // the real one if it is ours). That player's puppet steers its own vehicle only while it
            // drives it itself; when it sits beside us, this puppet does, or nobody would.
            bool ownerAtReins = false;
            if (isPlayerRef(s.vehicleRef)) {
                auto owner = puppets_.find(playerOfRef(s.vehicleRef));
                ownerAtReins = owner != puppets_.end() && owner->second.seatedIn == wantVehicle && owner->second.seat == -1;
            }
            controlsShared = !ownerAtReins && !(local_.vehicle == wantVehicle && local_.state.seat == -1) &&
                             !puppetDrives(wantVehicle);
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
            controlsShared = local_.mount != wantMount && !puppetDrives(wantMount);
        }
    }
    if (controlsShared) driven_.push_back(wantVehicle ? wantVehicle : wantMount);

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
    // A shared entity is already where it should be (more or less): take it over smoothly instead
    // of snapping it onto the puppet's stream, which runs a little earlier or later than the world's.
    if (controlsShared && p.bodyMover.mode == Mover::None) p.bodyMover.mode = Mover::Moving;
    // Own rides that are no longer used disappear after a while (or at once if far away from their
    // player). Used means by anybody: the player, us, or another player's puppet riding along.
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
    expire(p.ownMount, p.ownMountUnusedMs, localSitsIn(p.ownMount) || puppetUses(p.ownMount));
    expire(p.ownVehicle, p.ownVehicleUnusedMs, localSitsIn(p.ownVehicle) || puppetUses(p.ownVehicle));

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
        if (ownRide || controlsShared) movePed(p.ridingOn, p.bodyMover, p.motion, target, dt, nowMs);
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
        if (onFoot && !p.ragdolled) movePed(p.ped, p.mover, p.motion, target, dt, nowMs);
    }
}

// ---------------------------------------------------------------- the owner's world, everywhere else

void CoopSession::bindMissionEntities(uint32_t nowMs) {
    if (mode_ != StoryMode::Shared || nowMs - lastMissionScanMs_ < kMissionScanMs) return;
    lastMissionScanMs_ = nowMs;
    missionLocal_.clear();
    game_.listMissionEntities(missionLocal_);
    if (missionLocal_.empty()) return;
    // All games run the same mission script, which creates the same NPCs (same model, same place).
    // Pair each owner mission entity with our own copy: the copy keeps its AI and its role in our
    // mission, while health, death and position follow the owner. A mission objective like
    // "kill them all" therefore completes in every game.
    // Copies of the same NPC type are paired closest first over all of them, so the pairing does
    // not depend on the order the mirrors happen to be visited in (that could pair an enemy with
    // the copy of its neighbour, which then slides across to where it should be).
    bindPairs_.clear();
    const float reach2 = kBindDistance * kBindDistance;
    for (auto& [id, m] : mirrors_) {
        if (m.bound || !m.st.script || !m.haveStatic || !m.havePos) continue;
        if (localSitsIn(m.handle)) continue;  // not while the player is on the stand-in
        for (size_t i = 0; i < missionLocal_.size(); ++i) {
            const MissionEntity& e = missionLocal_[i];
            if (e.kind != m.st.kind || e.model != m.st.model || e.script != m.st.script) continue;
            float d2 = distanceSq(e.pos, m.st.pos);
            if (d2 < reach2) bindPairs_.push_back({d2, &m, i});
        }
    }
    std::sort(bindPairs_.begin(), bindPairs_.end(), [](const BindCandidate& a, const BindCandidate& b) { return a.d2 < b.d2; });
    for (const BindCandidate& c : bindPairs_) {
        Mirror& m = *c.mirror;
        const MissionEntity& e = missionLocal_[c.local];
        if (m.bound || mirrorByHandle_.count(e.handle) || isSessionEntity(e.handle) || !game_.exists(e.handle)) continue;
        if (m.handle) destroyMirror(m, true);  // replace the stand-in shown so far
        m.handle = e.handle;
        m.bound = true;
        m.mover = Mover{};
        m.lastUpdateMs = 0;
        m.deadSinceMs = 0;
        m.outfitApplied = m.st.outfit;  // it is the real one: already dressed by its script
        mirrorByHandle_[m.handle] = m.id;
        game_.setHarmless(m.handle, true);
        m.expectedHealth = game_.health(m.handle);
    }
}

void CoopSession::followBound(EntityHandle h, const MotionSample& t, float dt) {
    // The local AI animates it; only keep it where the owner's copy is.
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
    const bool shared = mode_ == StoryMode::Shared;
    const Vec3 view = story_.viewPoint(local_.state.pos);
    int shown = 0;
    for (auto it = mirrors_.begin(); it != mirrors_.end();) {
        Mirror& m = it->second;
        // The owner stopped mentioning it (what we ride or drive waits: our game moves it anyway,
        // and an explicit removal or a new world still ends it).
        if (nowMs - m.lastHeardMs > kMirrorTimeoutMs && !localRides(m.handle)) {
            destroyMirror(m, true);
            it = mirrors_.erase(it);
            continue;
        }
        ++it;
        // CPU level of detail: what is far from us is updated every 2nd or 4th frame (staggered by
        // id so the work spreads evenly). Motion is time-based, so nothing slows down. Skipped
        // frames cost nothing, not even a look whether the entity still exists.
        if (m.handle && !m.bound && m.lastUpdateMs) {
            float range = distance(m.st.pos, view);
            uint32_t every = range < 35.0f ? 1u : range < 80.0f ? 2u : 4u;
            if (m.st.kind == EntityKind::Vehicle) every = std::min(every, 2u);  // physics drifts if left alone longer
            if ((frame_ + m.id) % every != 0) {
                ++shown;
                continue;
            }
        }
        if (m.handle && !game_.exists(m.handle)) {
            mirrorByHandle_.erase(m.handle);
            m.handle = 0;
            m.bound = false;
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
                if (!(local_.vehicle == m.handle && local_.state.seat == -1) && !puppetDrives(m.handle))
                    steerVehicle(m.handle, m.mover, t, step);
                continue;
            }
            if (m.st.flags & NF_Dead) {
                if (!game_.isDead(m.handle)) game_.kill(m.handle);  // our mission sees the kill
                m.expectedHealth = 0;
                continue;
            }
            forwardDamage(m.handle, m.expectedHealth, m.id);
            if (game_.isDead(m.handle)) {
                if (!m.deadSinceMs) m.deadSinceMs = nowMs ? nowMs : 1;
                else if (nowMs - m.deadSinceMs > kRespawnGraceMs && m.st.health > 0) {
                    game_.revive(m.handle, m.st.health);  // the owner says it survived
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
            if (local_.mount != m.handle && !puppetDrives(m.handle)) followBound(m.handle, t, step);
            continue;
        }
        if (m.st.outfit && m.st.outfit != m.outfitApplied && m.st.kind == EntityKind::Ped) {
            game_.setOutfit(m.handle, m.st.outfit);
            m.outfitApplied = m.st.outfit;
        }
        float step = m.lastUpdateMs ? std::clamp((nowMs - m.lastUpdateMs) / 1000.0f, 0.0f, 0.25f) : dt;
        m.lastUpdateMs = nowMs;
        if (m.st.kind == EntityKind::Vehicle) {
            MotionSample t = m.motion.sample(nowMs);
            // We are driving it: our game moves it and the owner follows. Another player drives it:
            // their puppet moves it.
            if ((local_.vehicle == m.handle && local_.state.seat == -1) || puppetDrives(m.handle)) continue;
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
            if (prefetched_.size() < 8 &&
                std::find(prefetched_.begin(), prefetched_.end(), m->st.model) == prefetched_.end()) {
                game_.prefetchModel(m->st.model);
                prefetched_.push_back(m->st.model);
            }
            continue;
        }
        --budget;
        MotionSample t = m->motion.empty() ? MotionSample{} : m->motion.sample(nowMs);
        Vec3 pos = m->motion.empty() ? m->st.pos : t.pos;
        m->handle = m->st.kind == EntityKind::Vehicle ? takeOverOwnVehicle(m->st.model, pos, m->id) : 0;
        if (!m->handle) m->handle = obtain(m->st.kind, m->st.model, pos, m->st.heading);
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
        // We killed it locally; if the owner disagrees for long enough, bring it back.
        if (!m.deadSinceMs) m.deadSinceMs = nowMs ? nowMs : 1;
        else if (nowMs - m.deadSinceMs > kRespawnGraceMs) destroyMirror(m, false);
        return;
    }
    m.deadSinceMs = 0;
    if (m.st.health > 0 && m.st.health != m.expectedHealth) {
        game_.setHealth(m.handle, m.st.health, 0);
        m.expectedHealth = game_.health(m.handle);
    }

    // Seats and saddles follow the owner's references.
    EntityHandle vehicle = resolveVehicle(m.st.vehicleRef);
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

    // We ride this horse: our game moves it. Another player rides it: their puppet moves it.
    bool weRide = local_.mount == m.handle || puppetDrives(m.handle);
    bool standingShooter = (shooting || aiming) && horizontalSpeed(t.vel) < 1.0f;
    if (!m.seatedIn && !m.ridingOn && !weRide && !m.ragdolled && !standingShooter) movePed(m.handle, m.mover, m.motion, t, dt, nowMs);
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
        if (f.draftOf && ours(f.draftOf)) continue;  // horses pulling one of our wagons
        if (f.mission && mode_ == StoryMode::Shared) continue;  // our own mission needs them
        if (game_.removeForeign(f.handle) == ForeignAction::Hidden) hidden_.insert(f.handle);
    }
    for (auto it = hidden_.begin(); it != hidden_.end();) it = game_.exists(*it) ? std::next(it) : hidden_.erase(it);
}

}  // namespace coop
