// Settings of a co-op session (read from RDR2Coop.ini by the plugin).
#pragma once
#include <cstdint>
#include "protocol.h"

namespace coop {

struct SessionConfig {
    bool worldSync = true;       // the world owner streams its world to everyone else
    bool friendlyFire = false;   // let players hurt each other
    bool syncClock = true;
    bool syncWeather = true;
    bool softCorrection = true;  // nudge remote characters onto their path instead of only re-tasking
    bool spectate = true;        // host world: watch the owner's scenes through the owner's camera
    bool syncWaypoints = true;
    bool autoRegroup = true;     // follow the server's "move to the mission start" requests
    bool autoUploadSaves = true; // world owner: put every new save on the server
    // Become the chosen character in our own game too (outside missions and scenes, which the story
    // hero plays). Off: only the others see us as that character. The plugin turns it on by default.
    bool becomeCharacter = false;
    float npcRadius = 150.0f;
    int maxPeds = 64;
    int maxVehicles = 16;
};

}  // namespace coop
