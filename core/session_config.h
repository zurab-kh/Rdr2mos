// Settings of a co-op session (read from RDR2Coop.ini by the plugin).
#pragma once
#include <cstdint>
#include "protocol.h"

namespace coop {

struct SessionConfig {
    bool worldSync = true;       // stream the host's world to the guest
    bool friendlyFire = false;   // let players hurt each other
    bool syncClock = true;
    bool syncWeather = true;
    bool softCorrection = true;  // nudge remote characters onto their path instead of only re-tasking
    StoryMode storyMode = StoryMode::Shared;
    bool cutsceneBarrier = true;    // shared: whoever finishes a scene first waits for the other
    uint32_t barrierTimeoutMs = 120000;
    bool sharedFail = true;         // shared: if one player dies in a mission, it restarts for both
    bool spectate = true;           // host world: the guest watches the host's scenes through the host's camera
    bool syncWaypoints = true;
    bool autoRegroup = true;     // guest: jump to the host when a mission starts or a cutscene ends far away
    float regroupDistance = 150.0f;
    float npcRadius = 150.0f;
    int maxPeds = 64;
    int maxVehicles = 16;
};

}  // namespace coop
