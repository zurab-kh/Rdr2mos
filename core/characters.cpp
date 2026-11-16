#include "characters.h"

#include <algorithm>
#include <cctype>

#include "mathx.h"

namespace coop {

const std::vector<CharacterInfo>& characterCatalog() {
    static const std::vector<CharacterInfo> catalog = {
        {"player_zero", "Arthur Morgan"},
        {"player_three", "John Marston"},
        {"cs_charlessmith", "Charles Smith"},
        {"cs_mrsadler", "Sadie Adler"},
        {"cs_javierescuella", "Javier Escuella"},
        {"cs_lenny", "Lenny Summers"},
        {"cs_billwilliamson", "Bill Williamson"},
        {"cs_micahbell", "Micah Bell"},
        {"cs_hoseamatthews", "Hosea Matthews"},
        {"cs_dutch", "Dutch van der Linde"},
        {"cs_abigailroberts", "Abigail Roberts"},
        {"cs_uncle", "Uncle"},
        {"cs_karen", "Karen Jones"},
        {"cs_marybeth", "Mary-Beth Gaskill"},
        {"cs_mollyoshea", "Molly O'Shea"},
        {"cs_susangrimshaw", "Susan Grimshaw"},
        {"cs_tilly", "Tilly Jackson"},
        {"cs_mrpearson", "Simon Pearson"},
        {"cs_revswanson", "Orville Swanson"},
        {"cs_leostrauss", "Leopold Strauss"},
        {"cs_josiahtrelawny", "Josiah Trelawny"},
        {"cs_sean", "Sean MacGuire"},
        {"cs_kieran", "Kieran Duffy"},
    };
    return catalog;
}

namespace {
// How many entries at the top of the catalog are certain to exist in every game version.
constexpr size_t kCertain = 10;
}  // namespace

bool isStoryHero(uint32_t model) { return model == joaat("player_zero") || model == joaat("player_three"); }

bool isStoryHero(const std::string& avatar) {
    std::string a = normalizeAvatar(avatar);
    return a == "player_zero" || a == "player_three";
}

std::string normalizeAvatar(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_') out += static_cast<char>(std::tolower(u));
        if (out.size() >= 48) break;
    }
    return out;
}

std::string characterName(const std::string& avatar) {
    std::string a = normalizeAvatar(avatar);
    for (const CharacterInfo& c : characterCatalog())
        if (a == c.model) return c.name;
    return a;
}

std::string characterNameOf(uint32_t model) {
    for (const CharacterInfo& c : characterCatalog())
        if (joaat(c.model) == model) return c.name;
    return "a character";
}

std::string freeCharacter(const std::string& wanted, const std::vector<std::string>& taken) {
    auto isTaken = [&](const std::string& avatar) {
        std::string a = normalizeAvatar(avatar);
        return std::any_of(taken.begin(), taken.end(), [&](const std::string& t) { return normalizeAvatar(t) == a; });
    };
    std::string w = normalizeAvatar(wanted);
    if (!w.empty() && !isTaken(w)) return w;
    const std::vector<CharacterInfo>& catalog = characterCatalog();
    for (size_t i = 0; i < catalog.size() && i < kCertain; ++i)
        if (!isTaken(catalog[i].model)) return catalog[i].model;
    return w.empty() ? catalog.front().model : w;
}

}  // namespace coop
