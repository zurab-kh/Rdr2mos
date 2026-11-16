#include "character.h"

#include "characters.h"
#include "mathx.h"

namespace coop {

namespace {
constexpr uint32_t kLookCheckMs = 1000;     // how often our own look is read (shops change it)
constexpr uint32_t kSettleMs = 3000;        // free roam this long before we turn back into our character
constexpr uint32_t kMinSwitchGapMs = 5000;  // never flip back and forth faster than this
constexpr uint32_t kRetryMs = 250;          // a model still loading: ask again this often
}  // namespace

void CharacterSync::reset() {
    sentAny_ = false;
    lastTryMs_ = 0;
}

void CharacterSync::setMine(const std::string& avatar) {
    std::string a = normalizeAvatar(avatar);
    if (a == mine_) return;
    mine_ = a;
    mineModel_ = a.empty() ? 0 : joaat(a.c_str());
    sentAny_ = false;  // the others need the look of the new character
    lastTryMs_ = 0;
}

bool CharacterSync::choose(const std::string& avatar) {
    std::string a = normalizeAvatar(avatar);
    if (a.empty() || a == mine_) return false;
    link_.send(Msg::CharacterRequest, CharacterRequest{a}, true);
    return true;
}

void CharacterSync::remember(uint32_t model, uint32_t nowMs, bool force) {
    if (!force && lastLookMs_ && nowMs - lastLookMs_ < kLookCheckMs) return;
    lastLookMs_ = nowMs ? nowMs : 1;
    Appearance look;
    if (!game_.readAppearance(game_.localPlayerPed(), look)) return;
    auto it = looks_.find(model);
    look.preset = it != looks_.end() ? it->second.preset : Appearance::kNoPreset;  // the game cannot tell
    look.model = model;
    looks_[model] = std::move(look);
}

void CharacterSync::sendLook(uint32_t model) {
    // The others see us as our chosen character: only its clothes mean anything to them. While we
    // are the story hero for a mission they keep the last look of the character.
    if (model != mineModel_) return;
    auto it = looks_.find(model);
    if (it == looks_.end() || (sentAny_ && it->second == sent_)) return;
    link_.send(Msg::Appearance, it->second, true);
    sent_ = it->second;
    sentAny_ = true;
}

bool CharacterSync::become(uint32_t model, uint32_t nowMs) {
    uint32_t from = game_.localModel();
    if (from) remember(from, nowMs, true);  // the look we leave, to put it on again later
    if (!game_.setLocalModel(model)) return false;
    ++switches_;
    lastSwitchMs_ = nowMs ? nowMs : 1;
    lastLookMs_ = 0;
    EntityHandle ped = game_.localPlayerPed();
    auto it = looks_.find(model);
    if (it != looks_.end()) {
        game_.applyAppearance(ped, it->second);
    } else if (!isStoryHero(model)) {
        game_.setOutfitPreset(ped, 0);  // a new ped needs clothes to be seen: its first outfit
        remember(model, nowMs, true);
        looks_[model].preset = 0;       // the wardrobe goes on from there
    }
    if (model == hero_ && mineModel_ != hero_) notify_("You are " + characterNameOf(hero_) + " for the story now");
    else notify_("You are " + characterNameOf(model));
    return true;
}

void CharacterSync::tick(uint32_t nowMs, const PlayerState& local, bool needHero) {
    if (!game_.ready()) return;
    uint32_t cur = game_.localModel();
    if (!cur) return;
    // The story hero this game plays: the one we found, or the one the story moved on to (the
    // epilogue turns Arthur into John), unless that is just our chosen character.
    if (!hero_) hero_ = isStoryHero(cur) ? cur : joaat("player_zero");
    else if (isStoryHero(cur) && cur != hero_ && cur != mineModel_) hero_ = cur;
    remember(cur, nowMs, false);
    sendLook(cur);

    if (needHero) {
        heroNeededMs_ = nowMs ? nowMs : 1;
        everNeededHero_ = true;
    }
    bool wantHero = needHero || (everNeededHero_ && nowMs - heroNeededMs_ < kSettleMs);
    heroForStory_ = config_.becomeCharacter && wantHero && mineModel_ && mineModel_ != hero_;
    if (!config_.becomeCharacter || !mineModel_) return;
    uint32_t target = wantHero ? hero_ : mineModel_;
    if (target == mineModel_ && !game_.modelExists(mineModel_)) return;
    if (target == cur) return;

    const uint16_t f = local.flags;
    if (f & (PF_Dead | PF_Cutscene)) return;  // never in the middle of a scene: it would lose its actor
    if (!wantHero) {
        // Back into our character only at an easy moment, and not back and forth.
        if (f & (PF_OnMount | PF_InVehicle | PF_Aiming | PF_Shooting | PF_Ragdoll | PF_InMission | PF_Holding)) return;
        if (lastSwitchMs_ && nowMs - lastSwitchMs_ < kMinSwitchGapMs) return;
    }
    if (lastTryMs_ && nowMs - lastTryMs_ < kRetryMs) return;
    lastTryMs_ = nowMs ? nowMs : 1;
    become(target, nowMs);
}

void CharacterSync::restoreHero() {
    if (!config_.becomeCharacter || !hero_ || !game_.ready()) return;
    uint32_t cur = game_.localModel();
    if (cur && cur != hero_) become(hero_, lastLookMs_);
    heroForStory_ = false;
}

void CharacterSync::cycleOutfit(int step) {
    if (!game_.ready()) return;
    uint32_t cur = game_.localModel();
    EntityHandle ped = game_.localPlayerPed();
    int n = game_.outfitPresets(ped);
    if (!cur || n <= 0) {
        notify_("No outfits to choose from for this character");
        return;
    }
    auto known = looks_.find(cur);
    int preset = known == looks_.end() || known->second.preset == Appearance::kNoPreset ? -1 : known->second.preset;
    if (preset >= n) preset = -1;
    preset = ((preset + step) % n + n) % n;
    game_.setOutfitPreset(ped, preset);
    Appearance look;
    if (!game_.readAppearance(ped, look)) look = Appearance{};
    look.model = cur;
    look.preset = static_cast<uint8_t>(preset);
    looks_[cur] = look;
    lastLookMs_ = 0;
    notify_("Outfit " + std::to_string(preset + 1) + " of " + std::to_string(n));
    sendLook(cur);
}

}  // namespace coop
