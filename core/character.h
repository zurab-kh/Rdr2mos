// Which character a player is, and what they wear.
//
//  * Every player picks a character (the server lets each one be taken once per campaign), and the
//    others see them as that character, dressed exactly as they are: every shop item, worn the same
//    way (looks only fit their own model, so they are sent with it).
//  * With becomeCharacter on, the player is that character in their own game too. Missions, scenes
//    and shops are made for the story hero (Arthur, or John later), so for those the player is the
//    story hero again: at a mission start (or as soon as the world owner starts one in the shared
//    story), while held for the others' scenes, and when the session ends. The swap waits for a safe
//    moment: never during a running scene, and back to the character only on foot, out of a fight,
//    a few seconds after the mission is over. Each character keeps its last look across the swaps.
//    Meanwhile the others keep seeing the chosen character.
//  * The wardrobe key cycles the character's outfit presets, for characters the shops do not dress.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include "game_api.h"
#include "link.h"
#include "session_config.h"

namespace coop {

// What the hero swap still owes once the session is gone: becoming the story hero again, in its own
// clothes, at the first safe moment (the game ready, not in a scene, the model loaded, on foot
// unless a mission is already on). Kept by whoever outlives the session (the plugin, across script
// restarts), and ticked until done.
class HeroRestore {
public:
    HeroRestore() = default;
    HeroRestore(uint32_t hero, const Appearance* look) : hero_(hero), pending_(hero != 0) {
        if (look) {
            look_ = *look;
            haveLook_ = true;
        }
    }
    bool pending() const { return pending_; }
    uint32_t hero() const { return hero_; }
    void cancel() { pending_ = false; }
    // True while there is still something to do.
    bool tick(IGame& game, uint16_t flags, uint32_t nowMs);

private:
    uint32_t hero_ = 0;
    Appearance look_;
    bool haveLook_ = false;
    bool pending_ = false;
    uint32_t lastTryMs_ = 0;
};

// How much the story needs the hero right now.
enum class HeroNeed {
    None,
    Soon,  // a mission of the others is on: be the hero before walking into its marker (not at a gallop)
    Now,   // our own mission or scene, a hold, or we start the missions ourselves
};

class CharacterSync {
public:
    CharacterSync(IGame& game, Link& link, const SessionConfig& config, std::function<void(const std::string&)> notify)
        : game_(game), link_(link), config_(config), notify_(std::move(notify)) {}

    // A new connection to the server: what was sent before is not known there any more.
    void reset();
    // Our character as the server has it (from every roster).
    void setMine(const std::string& avatar);
    const std::string& mine() const { return mine_; }
    // Asks the server for another character (the menu). False: that is already ours, or not a model.
    bool choose(const std::string& avatar);
    // Wardrobe: the next (or previous) outfit preset of the character we are in our game right now.
    void cycleOutfit(int step);

    // Every frame. `need`: a mission or scene is on (or about to be), the story hero plays it.
    void tick(uint32_t nowMs, const PlayerState& local, HeroNeed need);
    // The session ends: what it takes to be the story hero again (nothing pending if we are).
    HeroRestore heroRestore() const;
    // The story hero, when it is known from before (the plugin keeps it across script restarts: a
    // ped that is our chosen character cannot tell whose story this is).
    void setStoryHero(uint32_t model) {
        if (isStoryHeroModel(model)) hero_ = model;
    }

    // For the HUD: we are the story hero for now, although we chose another character.
    bool playingHero() const { return heroForStory_; }
    uint32_t heroModel() const { return hero_; }
    int switches() const { return switches_; }

private:
    static bool isStoryHeroModel(uint32_t model);
    void remember(uint32_t model, uint32_t nowMs, bool force);
    void sendLook(uint32_t model);
    bool become(uint32_t model, uint32_t nowMs);

    IGame& game_;
    Link& link_;
    const SessionConfig& config_;
    std::function<void(const std::string&)> notify_;

    std::string mine_;
    uint32_t mineModel_ = 0;
    uint32_t hero_ = 0;                       // the story hero this game plays (0: not known yet)
    uint32_t lastSet_ = 0;                    // the model we gave the player last (else the game did)
    uint32_t lastCur_ = 0;                    // the model the player had at the last tick
    std::map<uint32_t, Appearance> looks_;    // the last look of each model we were
    Appearance sent_;
    bool sentAny_ = false;
    bool heroForStory_ = false;
    uint32_t lastLookMs_ = 0, lastSwitchMs_ = 0, lastTryMs_ = 0, heroNeededMs_ = 0;
    bool everNeededHero_ = false;
    int switches_ = 0;
};

}  // namespace coop
