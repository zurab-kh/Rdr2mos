// Characters: every player picks who they are (once per campaign), becomes that character in their
// own game outside missions and scenes, and is seen by the others dressed exactly as they are.
#include <functional>

#include "harness.h"
#include "characters.h"
#include "../plugin/character_menu.h"

namespace {

const uint32_t kArthur = joaat("player_zero");
const uint32_t kJohn = joaat("player_three");
const uint32_t kCharles = joaat("cs_charlessmith");
const uint32_t kSadie = joaat("cs_mrsadler");

Player& joinAs(Cluster& c, const std::string& name, const std::string& avatar, bool wantsWorld,
               const std::function<void(Player&)>& before = nullptr) {
    c.startServer();
    c.extra.push_back(std::make_unique<Player>());
    Player& p = *c.extra.back();
    // Everybody plays the story as Arthur in their own game, whoever they want to be.
    p.game.localModelHash = kArthur;
    if (before) before(p);  // the game as it is when the player connects
    p.connect(c.net, 0x0A000100u + static_cast<uint32_t>(c.extra.size()), name, avatar, wantsWorld, c.config, c.now);
    c.players.push_back(&p);
    for (int i = 0; i < 300 && !p.session->status().welcomed; ++i) c.run(16);
    c.run(200);
    return p;
}

bool saw(Player& p, const char* needle) { return sawNotice(*p.session, needle); }

// How `viewer` shows `who`: the model of the puppet and what it wears.
const FakeGame::Ent* puppetOf(Player& viewer, const std::string& name) {
    for (const auto& [h, e] : viewer.game.ents)
        if (e.role == SpawnRole::PlayerPuppet && e.ours && e.label == name) return &e;
    return nullptr;
}

}  // namespace

COOP_TEST(testCharactersOncePerCampaign, "characters: every character once per campaign, the taken one is replaced") {
    Cluster c;
    Player& a = joinAs(c, "A", "player_zero", true);
    Player& b = joinAs(c, "B", "player_zero", false);  // wants Arthur too
    c.run(1500);
    std::printf("  A is %s, B is %s\n", a.session->status().character.c_str(), b.session->status().character.c_str());
    CHECK(a.session->status().character == "player_zero");
    CHECK(b.session->status().character == "player_three");  // the first free one
    CHECK(saw(b, "Arthur Morgan is taken - you are John Marston"));
    CHECK(a.session->status().characters.size() == 2);
    const FakeGame::Ent* bAtA = puppetOf(a, "B");
    CHECK(bAtA && bAtA->model == kJohn);  // A meets John, not a second Arthur
    const FakeGame::Ent* aAtB = puppetOf(b, "A");
    CHECK(aAtB && aAtB->model == kArthur);
}

COOP_TEST(testCharactersChooseAnother, "characters: choosing another character, the taken one is refused") {
    Cluster c;
    Player& a = joinAs(c, "A", "player_zero", true);
    Player& b = joinAs(c, "B", "player_three", false);
    c.run(1500);
    CHECK(puppetOf(a, "B") && puppetOf(a, "B")->model == kJohn);
    // B takes Charles: A's game swaps B's character, and B's horse stays.
    CHECK(b.session->chooseCharacter("cs_charlessmith"));
    c.run(1500);
    CHECK(b.session->status().character == "cs_charlessmith");
    const FakeGame::Ent* bAtA = puppetOf(a, "B");
    CHECK(bAtA && bAtA->model == kCharles);
    CHECK(a.game.count(SpawnRole::PlayerPuppet) == 1);  // the old one is gone
    CHECK(journalHas(c, "default", "B is now Charles Smith"));
    // Arthur is A's: refused, and B stays Charles.
    CHECK(b.session->chooseCharacter("player_zero"));
    c.run(1000);
    CHECK(saw(b, "Arthur Morgan is already taken by A"));
    CHECK(b.session->status().character == "cs_charlessmith");
    CHECK(!b.session->chooseCharacter("cs_charlessmith"));  // already ours: nothing to ask
    // Once A lets Arthur go, B may have him.
    CHECK(a.session->chooseCharacter("cs_mrsadler"));
    c.run(1000);
    CHECK(b.session->chooseCharacter("player_zero"));
    c.run(1000);
    CHECK(b.session->status().character == "player_zero" && a.session->status().character == "cs_mrsadler");
}

COOP_TEST(testCharactersExactClothes, "characters: the others see every piece of clothing, only on its own model") {
    Cluster c;
    Player& a = joinAs(c, "A", "player_zero", true);
    Player& b = joinAs(c, "B", "player_three", false);
    b.game.localModelHash = kJohn;  // B plays as John in its game (the epilogue)
    // B buys a hat and a coat and rolls the sleeves up.
    b.game.playerItems = {{0x1111, 0}, {0x2222, 0xABCD}, {0x3333, 0}};
    b.game.playerOutfit = 77;
    c.run(2500);
    const FakeGame::Ent* bAtA = puppetOf(a, "B");
    CHECK(bAtA && bAtA->model == kJohn);
    CHECK(bAtA && bAtA->look.items == b.game.playerItems && bAtA->look.model == kJohn);
    // A new hat: sent once, put on once.
    int looks = bAtA ? bAtA->looks : 0;
    b.game.playerItems[0].component = 0x4444;
    c.run(2500);
    bAtA = puppetOf(a, "B");
    CHECK(bAtA && bAtA->look.items == b.game.playerItems && bAtA->looks == looks + 1);
    c.run(3000);
    CHECK(bAtA && bAtA->looks == looks + 1);  // nothing new: nothing sent again

    // B chooses Charles but plays on as John (becomeCharacter is off here): John's clothes are
    // never put on Charles, and are not even sent.
    CHECK(b.session->chooseCharacter("cs_charlessmith"));
    c.run(1500);
    bAtA = puppetOf(a, "B");
    CHECK(bAtA && bAtA->model == kCharles && bAtA->looks == 0);
    b.game.playerItems[1].component = 0x5555;
    c.run(2500);
    bAtA = puppetOf(a, "B");
    CHECK(bAtA && bAtA->looks == 0);
}

COOP_TEST(testCharactersBecomeAndHeroForStory, "characters: becoming the character, the story hero for missions and scenes") {
    Cluster c;
    c.config.becomeCharacter = true;
    Player& a = joinAs(c, "A", "player_zero", true);
    // B comes as Arthur in the clothes it bought in the shops, and wants to be Charles.
    Player& b = joinAs(c, "B", "cs_charlessmith", false, [](Player& p) { p.game.playerItems = {{0xA1, 0}, {0xA2, 0}}; });
    c.run(5000);
    std::printf("  B's game: %s, %d switch(es)\n", b.game.localModelHash == kCharles ? "Charles" : "not Charles",
                b.game.modelSwitches);
    CHECK(b.game.localModelHash == kCharles);
    CHECK(b.game.playerPreset == 0);  // a new character comes dressed in its first outfit
    CHECK(saw(b, "You are Charles Smith"));
    CHECK(puppetOf(a, "B") && puppetOf(a, "B")->model == kCharles);
    CHECK(a.game.localModelHash == kArthur && a.game.modelSwitches == 0);  // A chose Arthur: nothing to do

    // The wardrobe: Charles' third outfit; A sees it.
    b.session->cycleOutfit(+1);
    b.session->cycleOutfit(+1);
    CHECK(b.game.playerPreset == 2);
    c.run(1500);
    const FakeGame::Ent* bAtA = puppetOf(a, "B");
    CHECK(bAtA && bAtA->look.preset == 2 && bAtA->look.model == kCharles);

    // A starts a mission: B is Arthur again before walking into the marker, in Arthur's clothes.
    a.game.player.flags |= PF_InMission;
    c.run(1500);
    CHECK(b.game.localModelHash == kArthur);
    CHECK(b.game.playerItems == (std::vector<ShopItem>{{0xA1, 0}, {0xA2, 0}}));
    CHECK(b.session->status().playingHero);
    CHECK(saw(b, "You are Arthur Morgan for the story now"));
    // ... and A keeps seeing Charles, in his outfit (no copy of Arthur next to A).
    bAtA = puppetOf(a, "B");
    CHECK(bAtA && bAtA->model == kCharles && bAtA->look.preset == 2);
    // B in the mission, then its scene: B stays Arthur throughout.
    b.game.player.flags |= PF_InMission;
    c.run(3000);
    b.game.player.flags |= PF_Cutscene;
    c.run(3000);
    b.game.player.flags &= static_cast<uint16_t>(~PF_Cutscene);
    CHECK(b.game.localModelHash == kArthur);
    int switches = b.game.modelSwitches;
    // The mission is over for both: a few seconds later, on foot, B is Charles again, in outfit 3.
    a.game.player.flags &= static_cast<uint16_t>(~PF_InMission);
    b.game.player.flags &= static_cast<uint16_t>(~PF_InMission);
    c.run(1000);
    CHECK(b.game.localModelHash == kArthur);  // not at once
    c.run(4000);
    CHECK(b.game.localModelHash == kCharles && b.game.playerPreset == 2);
    CHECK(b.game.modelSwitches == switches + 1);
    CHECK(!b.session->status().playingHero);

    // Disconnecting: B leaves the co-op as Arthur, in Arthur's clothes.
    b.session->cleanup();
    CHECK(b.game.localModelHash == kArthur);
    CHECK(b.game.playerItems == (std::vector<ShopItem>{{0xA1, 0}, {0xA2, 0}}));
}

COOP_TEST(testCharactersSwitchOnlyWhenSafe, "characters: no swap in a scene, on a horse, in a fight; a mission never waits") {
    Cluster c;
    c.config.becomeCharacter = true;
    Player& a = joinAs(c, "A", "player_zero", true);
    // B rides when it connects: B stays Arthur until it gets off.
    Player& b = joinAs(c, "B", "cs_mrsadler", false, [](Player& p) { p.game.player.flags = PF_OnMount; });
    c.run(6000);
    CHECK(b.game.localModelHash == kArthur);
    b.game.player.flags = PF_Aiming;  // off the horse, but in a gunfight
    c.run(3000);
    CHECK(b.game.localModelHash == kArthur);
    b.game.player.flags = 0;
    c.run(1500);
    CHECK(b.game.localModelHash == kSadie);
    // A mission starts while B is on a horse: B becomes Arthur at once anyway.
    b.game.player.flags = PF_OnMount;
    a.game.player.flags = PF_InMission;
    c.run(1000);
    CHECK(b.game.localModelHash == kArthur);
    // B's own scene runs before the swap could happen: B is left alone until it ends.
    a.game.player.flags = 0;
    b.game.player.flags = 0;
    c.run(8000);
    CHECK(b.game.localModelHash == kSadie);
    b.game.player.flags = PF_Cutscene | PF_InMission;
    c.run(3000);
    CHECK(b.game.localModelHash == kSadie);  // a running scene keeps its actor
    b.game.player.flags = PF_InMission;
    c.run(500);
    CHECK(b.game.localModelHash == kArthur);
    // A model that keeps loading: asked again until it works.
    b.game.player.flags = 0;
    b.game.switchFailures = 5;
    c.run(8000);
    CHECK(b.game.localModelHash == kSadie && b.game.switchFailures == 0);
}

COOP_TEST(testCharactersHostWorldAndMissingModel, "characters: owner's world mode, a model the game does not have") {
    {
        // In the owner's world only the owner plays missions: a guest stays its character.
        Cluster c;
        c.config.becomeCharacter = true;
        c.serverConfig.storyMode = StoryMode::HostWorld;
        Player& a = joinAs(c, "A", "player_zero", true);
        Player& b = joinAs(c, "B", "cs_charlessmith", false);
        c.run(5000);
        CHECK(b.game.localModelHash == kCharles);
        a.game.player.flags = PF_InMission | PF_Cutscene;
        c.run(4000);
        CHECK(b.game.localModelHash == kCharles);
    }
    {
        Cluster c;
        c.config.becomeCharacter = true;
        Player& a = joinAs(c, "A", "player_zero", true);
        (void)a;
        c.extra.push_back(std::make_unique<Player>());
        Player& b = *c.extra.back();
        b.game.localModelHash = kArthur;
        b.game.missingModels = {kSadie};
        b.connect(c.net, 0x0A000177u, "B", "cs_mrsadler", false, c.config, c.now);
        c.players.push_back(&b);
        c.run(6000);
        CHECK(b.game.localModelHash == kArthur && b.game.modelSwitches == 0);  // stays Arthur, no crash
    }
}

COOP_TEST(testCharactersCatalogAndWire, "characters: catalog helpers, a full wardrobe fits one message") {
    CHECK(characterName("CS_CharlesSmith") == "Charles Smith");
    CHECK(characterNameOf(kSadie) == "Sadie Adler");
    CHECK(isStoryHero(kArthur) && isStoryHero("PLAYER_THREE") && !isStoryHero(kCharles));
    CHECK(freeCharacter("player_zero", {}) == "player_zero");
    CHECK(freeCharacter("player_zero", {"Player_Zero"}) == "player_three");
    CHECK(freeCharacter("", {"player_zero", "player_three"}) == "cs_charlessmith");
    CHECK(normalizeAvatar("CS_Mr Sadler!") == "cs_mrsadler");

    Appearance full;
    full.outfit = 1;
    full.model = kArthur;
    full.preset = 3;
    for (size_t i = 0; i < kMaxShopItems + 10; ++i) full.items.push_back({static_cast<uint32_t>(i + 1), static_cast<uint32_t>(i)});
    std::vector<uint8_t> bytes = encode(full);
    CHECK(bytes.size() + 1 <= kMaxMessage);  // + the sender id the server puts in front
    Appearance back;
    CHECK(decode(bytes, back));
    CHECK(back.items.size() == kMaxShopItems && back.preset == 3 && back.model == kArthur);
    CharacterRequest req{"cs_lenny"}, req2;
    CHECK(decode(encode(req), req2) && req2.avatar == "cs_lenny");
    CHECK(!decode(encode(CharacterRequest{""}), req2));
}

COOP_TEST(testCharactersMenu, "characters: the menu lists what the game has, marks ours and the taken ones") {
    SessionStatus st;
    st.character = "cs_charlessmith";
    st.characters = {{"cs_charlessmith", "Me"}, {"player_zero", "A"}, {"cs_mrsadler", "B"}};
    CharacterMenu menu;
    menu.open(st, [](const std::string& model) { return model != "cs_kieran"; });  // not in this game
    CHECK(menu.isOpen());
    CHECK(menu.items().size() == characterCatalog().size() - 1);
    CHECK(menu.items()[menu.cursor()].avatar == "cs_charlessmith");  // starts on ours
    std::string why;
    CHECK(menu.pick(why).empty() && why.find("already") != std::string::npos);
    menu.move(-2);  // up to Arthur, A's
    CHECK(menu.items()[menu.cursor()].avatar == "player_zero");
    CHECK(menu.pick(why).empty() && why == "Arthur Morgan is taken by A");
    menu.move(+3);  // past ours to Sadie, B's, then the next one is free
    CHECK(menu.pick(why).empty());
    menu.move(+1);
    CHECK(menu.pick(why) == "cs_javierescuella");
    menu.move(-100);  // wraps around, never out of range
    CHECK(menu.cursor() < menu.items().size());
    std::vector<std::string> rows = menu.lines(5);
    CHECK(rows.size() == 5);
    int cursors = 0;
    for (const std::string& r : rows) cursors += r.rfind("> ", 0) == 0;
    CHECK(cursors == 1);
    CHECK(std::any_of(rows.begin(), rows.end(), [](const std::string& r) { return r.find("(") != std::string::npos; }) ||
          menu.cursor() > 6);
    menu.close();
    CHECK(!menu.isOpen() && menu.lines(5).empty());
}
