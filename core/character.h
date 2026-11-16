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

    // Every frame. `needHero`: a mission or scene is on (or about to be), the story hero plays it.
    void tick(uint32_t nowMs, const PlayerState& local, bool needHero);
    // The session ends: be the story hero again, so the story goes on as it was.
    void restoreHero();

    // For the HUD: we are the story hero for now, although we chose another character.
    bool playingHero() const { return heroForStory_; }
    uint32_t heroModel() const { return hero_; }
    int switches() const { return switches_; }

private:
    void remember(uint32_t model, uint32_t nowMs, bool force);
    void sendLook(uint32_t model);
    bool become(uint32_t model, uint32_t nowMs);

    IGame& game_;
    Link& link_;
    const SessionConfig& config_;
    std::function<void(const std::string&)> notify_;

    std::string mine_;
    uint32_t mineModel_ = 0;
    uint32_t hero_ = 0;                       // the story hero this game was playing as
    std::map<uint32_t, Appearance> looks_;    // the last look of each model we were
    Appearance sent_;
    bool sentAny_ = false;
    bool heroForStory_ = false;
    uint32_t lastLookMs_ = 0, lastSwitchMs_ = 0, lastTryMs_ = 0, heroNeededMs_ = 0;
    bool everNeededHero_ = false;
    int switches_ = 0;
};

}  // namespace coop
