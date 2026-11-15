// The co-op session: turns the link + the game interface into a shared story world.
//
//  * Each player owns their own character; the other side shows it as a "puppet" ped.
//  * The host owns the world. NPCs near the players are streamed to the guest, who shows them
//    as "mirror" peds and keeps its own ambient population switched off.
//  * Damage is forwarded to whoever owns the victim: the guest's hits on mirrors go to the
//    host's real NPCs, and hits on a puppet go to the player behind it.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "game_api.h"
#include "interp.h"
#include "link.h"

namespace coop {

struct SessionConfig {
    bool worldSync = true;      // stream host NPCs to the guest
    bool friendlyFire = false;  // let players hurt each other
    bool syncClock = true;
    float npcRadius = 120.0f;
    int maxNpcs = 48;
};

struct SessionStatus {
    bool peerVisible = false;
    Vec3 peerPos;
    float peerDistance = 0;
    bool peerDead = false;
    bool peerInMission = false;
    int syncedNpcs = 0;
};

class CoopSession {
public:
    CoopSession(IGame& game, Link& link, const SessionConfig& config);

    // Call once per game frame.
    void tick(uint32_t nowMs);
    // Removes everything this session created in the game world.
    void cleanup();
    // Moves the local player next to the friend. Returns false if their position is unknown.
    bool teleportToPeer();

    const SessionStatus& status() const { return status_; }

private:
    // How a driven ped was last told to behave, to avoid re-issuing tasks every frame.
    struct Drive {
        enum Mode { None, Idle, Moving, Aiming, Shooting } mode = None;
        uint32_t lastTaskMs = 0;
    };
    struct Puppet {
        EntityHandle ped = 0;
        EntityHandle mount = 0;
        uint32_t mountModel = 0;
        bool riding = false;
        uint32_t weapon = 0;
        int expectedHealth = 0;
        uint32_t deadSinceMs = 0;
        uint32_t lastAimMs = 0;
        Drive drive;
        MotionBuffer motion{100};
        PlayerState state;
        bool haveState = false;
    };
    struct Mirror {
        NpcState state;
        EntityHandle ped = 0;
        EntityHandle ridingOn = 0;
        uint32_t weapon = 0;
        int expectedHealth = 0;
        uint32_t lastSeenMs = 0;
        uint32_t deadSinceMs = 0;
        uint32_t lastAimMs = 0;
        Drive drive;
        MotionBuffer motion{200};
    };

    void handleMessages(uint32_t nowMs);
    void sendLocalState(uint32_t nowMs);
    void sendWorld(uint32_t nowMs);
    void updatePuppet(uint32_t nowMs);
    void updateMirrors(uint32_t nowMs);
    void hideForeignPeds(uint32_t nowMs);
    void drive(EntityHandle ped, Drive& d, const MotionSample& target, uint32_t nowMs);
    void forwardDamage(EntityHandle ped, int& expectedHealth, uint16_t target);
    void destroyMirror(Mirror& m);
    void destroyPuppet();
    bool isHost() const { return link_.local().role == Role::Host; }

    IGame& game_;
    Link& link_;
    SessionConfig config_;
    SessionStatus status_;
    uint32_t epoch_ = 0;
    bool active_ = false;

    Puppet puppet_;
    PlayerState local_;
    bool haveLocal_ = false;
    uint32_t lastStateMs_ = 0;
    uint32_t lastWorldMs_ = 0;
    uint32_t lastInfoMs_ = 0;
    uint32_t lastHideMs_ = 0;

    // host
    std::map<EntityHandle, uint16_t> npcIds_;
    std::map<uint16_t, EntityHandle> npcById_;
    uint16_t nextNpcId_ = 1;
    // guest
    std::map<uint16_t, Mirror> mirrors_;
    std::set<EntityHandle> hidden_;
    bool hostWorldSync_ = false;
};

}  // namespace coop
