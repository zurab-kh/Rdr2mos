// The characters a player can be: story characters of RDR2, by model name. Each one can be taken by
// one player of a campaign at a time (the server sees to it), so nobody meets a copy of themselves.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace coop {

struct CharacterInfo {
    const char* model;  // model name, lower case (also the avatar sent to the server)
    const char* name;   // shown in the menu
};

// In menu order. The game may not know every model (the plugin hides the ones it cannot load); the
// first ones are certain and are the ones the server hands out by itself.
const std::vector<CharacterInfo>& characterCatalog();

// The characters the story is played as: missions, scenes and shops are made for them.
bool isStoryHero(uint32_t model);
bool isStoryHero(const std::string& avatar);

// Lower case, letters, digits and '_' only, at most 48: how avatars are compared and stored.
std::string normalizeAvatar(const std::string& raw);

// The name shown for an avatar ("cs_charlessmith" -> "Charles Smith"), the model name if unknown.
std::string characterName(const std::string& avatar);
// The same by model hash ("a character" if unknown).
std::string characterNameOf(uint32_t model);

// `wanted` when nobody in `taken` has it, otherwise the first character of the catalog that is
// free; `wanted` again if every one is taken (then a copy cannot be helped).
std::string freeCharacter(const std::string& wanted, const std::vector<std::string>& taken);

}  // namespace coop
