#include <algorithm>
#include <string_view>

#include <catch_amalgamated.hpp>

#include "test_support.h"

using namespace rm;
using nlohmann::json;

namespace {

// Alpha with a free base skin and a challenge-locked "night" skin that changes the archetype.
ContentSources SkinSources() {
    return test::EditFixture([](json&, json&, json& characters, json&, json&) {
        characters["characters"][0]["skins"] = json::array({
            json{{"id", "alpha_base"}, {"name", "Base"}, {"costume_index", 0}},
            json{{"id", "alpha_night"},
                 {"name", "Night"},
                 {"costume_index", 1},
                 {"starting_weapons", json::array({"shotgun"})},
                 {"starting_passive", "jackpot"},
                 {"unlock_kind", "total_kills"},
                 {"unlock_count", 3},
                 {"unlock_text", "Kill 3 enemies"}},
        });
    });
}

constexpr std::string_view kLoadouts = R"({
  "schema_version": 1, "budget": 60, "default_weapon_cost": 20,
  "weapon_cost": {"handgun": 10},
  "equipment": [
    {"id": "grenade", "name": "Grenade", "cost": 8, "max": 2},
    {"id": "herb", "name": "Herb", "cost": 5, "max": 3}
  ]
})";

const CharacterAvailability* Find(const std::vector<CharacterAvailability>& list, std::string_view id) {
    const auto it = std::find_if(list.begin(), list.end(), [&](const CharacterAvailability& c) { return c.id == id; });
    return it == list.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE("every character has a default skin and skins gate on challenges", "[characters]") {
    test::Harness h(test::Load(SkinSources()));
    const auto list = h.rc.Characters();
    const CharacterAvailability* alpha = Find(list, "alpha");
    const CharacterAvailability* beta = Find(list, "beta");
    REQUIRE(alpha != nullptr);
    REQUIRE(beta != nullptr);
    REQUIRE(alpha->skins.size() == 2);
    CHECK(alpha->skins[0].gate == CharacterGate::Selectable);
    CHECK(alpha->costume_index == 0);
    CHECK(alpha->skins[1].gate == CharacterGate::Locked);
    CHECK(alpha->skins[1].unlock_text == "Kill 3 enemies");
    CHECK(alpha->skins[1].challenge.target == 3);
    REQUIRE(beta->skins.size() == 1);  // synthesized default
    CHECK(beta->skins[0].id == "beta_base");

    CHECK_FALSE(h.rc.StartRun("alpha", 5, "alpha_night").ok());  // locked
    CHECK_FALSE(h.rc.StartRun("alpha", 5, "nope").ok());         // unknown
    REQUIRE(h.rc.StartRun("alpha", 5).ok());
    CHECK(h.rc.state().skin_id == "alpha_base");
    CHECK(h.rc.state().character.costume_index == 0);
    const std::string& weapon = h.rc.state().loadout.weapon_id;
    CHECK((weapon == "pistol" || weapon == "shotgun"));
    CHECK(h.rc.state().perks.count("jackpot") == 0);
}

TEST_CASE("an unlocked skin changes the starting weapon pool and passive", "[characters]") {
    Profile profile;
    profile.stats.total_kills = 3;
    test::Harness h(test::Load(SkinSources()), profile);
    CHECK(h.rc.profile().unlocked_characters.count("alpha_night") == 1);
    CHECK(h.sink.Count(EventType::CharacterUnlocked) == 1);
    REQUIRE(h.rc.StartRun("alpha", 9, "alpha_night").ok());
    CHECK(h.rc.state().skin_id == "alpha_night");
    CHECK(h.rc.state().loadout.weapon_id == "shotgun");
    CHECK(h.rc.state().perks.count("jackpot") == 1);
    CHECK(h.rc.state().character.costume_index == 1);
    CHECK(h.rc.Snapshot(0.0).skin_id == "alpha_night");
}

TEST_CASE("loadout rolls spend the point budget deterministically", "[characters]") {
    ContentSources sources = SkinSources();
    sources.loadouts = std::string(kLoadouts);
    const auto content = test::Load(sources);
    const std::int64_t starting = content->economy().run.starting_supplies;

    test::Harness a(content);
    test::Harness b(content);
    REQUIRE(a.rc.StartRun("alpha", 42).ok());
    REQUIRE(b.rc.StartRun("alpha", 42).ok());
    const Loadout& la = a.rc.state().loadout;
    const Loadout& lb = b.rc.state().loadout;
    CHECK(la.weapon_id == lb.weapon_id);
    REQUIRE(la.items.size() == lb.items.size());
    for (std::size_t i = 0; i < la.items.size(); ++i) {
        CHECK(la.items[i].id == lb.items[i].id);
        CHECK(la.items[i].count == lb.items[i].count);
    }
    CHECK(la.budget == 60);
    CHECK(la.spent <= la.budget);
    CHECK(la.spent > 0);
    for (const LoadoutItem& item : la.items) {
        if (item.id == "grenade") CHECK(item.count <= 2);
        if (item.id == "herb") CHECK(item.count <= 3);
    }
    // Nothing affordable is left over, and unspent points became supplies.
    const std::int64_t left = la.budget - la.spent;
    int grenades = 0;
    int herbs = 0;
    for (const LoadoutItem& item : la.items) {
        if (item.id == "grenade") grenades = item.count;
        if (item.id == "herb") herbs = item.count;
    }
    CHECK(left >= 0);
    CHECK((grenades == 2 || left < 8));
    CHECK((herbs == 3 || left < 5));
    CHECK(a.rc.state().supplies.balance() == starting + left);
    CHECK_FALSE(a.rc.Snapshot(0.0).loadout.empty());

    test::Harness c(content);
    REQUIRE(c.rc.StartRun("alpha", 43).ok());
    const bool same = c.rc.state().loadout.weapon_id == la.weapon_id && c.rc.state().loadout.items.size() == la.items.size();
    (void)same;  // a different seed may or may not differ; only determinism is asserted
}

TEST_CASE("skin definitions are validated", "[characters]") {
    const auto errors = [](const std::function<void(json&)>& edit) {
        return Content::LoadFromStrings(test::EditFixture([&](json&, json&, json& characters, json&, json&) {
                   edit(characters);
               }))
            .errors;
    };
    auto has = [](const std::vector<std::string>& errs, std::string_view needle) {
        for (const std::string& e : errs) UNSCOPED_INFO("got: " << e);
        return std::any_of(errs.begin(), errs.end(), [&](const std::string& e) { return e.find(needle) != std::string::npos; });
    };
    CHECK(has(errors([](json& c) {
                  c["characters"][0]["skins"] = json::array({json{{"id", "beta"}, {"name", "Clash"}}});
              }),
              "duplicates a character or skin id"));
    CHECK(has(errors([](json& c) {
                  c["characters"][0]["skins"] =
                      json::array({json{{"id", "x"}, {"name", "X"}, {"starting_weapons", json::array({"laser"})}}});
              }),
              "unknown weapon"));
    CHECK(has(errors([](json& c) {
                  c["characters"][0]["skins"] = json::array({json{{"id", "x"}, {"name", "X"}, {"costume_index", -2}}});
              }),
              "costume_index"));
}

TEST_CASE("item perks grant inventory items on kills and arena clears", "[characters]") {
    const auto content = test::Load(test::EditFixture([](json& perks, json&, json& characters, json&, json&) {
        perks["perks"].push_back(json{
            {"id", "pocket_grenades"},
            {"name", "Pocket Grenades"},
            {"description", "Every 2 kills, a grenade; a spray per arena."},
            {"category", "style"},
            {"rarity", "common"},
            {"max_stacks", 1},
            {"effects", json::array({json{{"kind", "item_on_kill"}, {"game_item", 262}, {"amount", 1}, {"every_kills", 2}},
                                     json{{"kind", "item_on_arena_clear"}, {"game_item", 772}, {"amount", 1}}})}});
        characters["characters"][0]["starting_passive"] = "pocket_grenades";
    }));
    const PerkDef* def = content->FindPerk("pocket_grenades");
    REQUIRE(def != nullptr);
    CHECK(std::find(def->requires_capabilities.begin(), def->requires_capabilities.end(), cap::kInventory) !=
          def->requires_capabilities.end());

    test::Harness h(content);
    REQUIRE(h.rc.StartRun("alpha", 3).ok());
    REQUIRE(h.rc.state().perks.count("pocket_grenades") == 1);
    REQUIRE(h.rc.BeginArena(0.0).ok());
    test::KillN(h.rc, 1, 1.0);
    CHECK(h.rc.ConsumeItemGrants().empty());
    test::KillN(h.rc, 1, 2.0);
    auto grants = h.rc.ConsumeItemGrants();
    REQUIRE(grants.size() == 1);
    CHECK(grants[0].game_item == 262);
    CHECK(grants[0].count == 1);
    CHECK(grants[0].source == "pocket_grenades");
    CHECK(h.rc.ConsumeItemGrants().empty());
    test::KillN(h.rc, h.rc.RemainingEnemies(), 3.0);
    REQUIRE(h.rc.IsArenaComplete());
    REQUIRE(h.rc.CompleteArena(30.0).ok());
    grants = h.rc.ConsumeItemGrants();
    REQUIRE_FALSE(grants.empty());
    CHECK(grants.back().game_item == 772);
    CHECK(grants.back().source == "pocket_grenades");

    // Without the inventory capability the perk is never granted (and so stays silent).
    Capabilities caps = Capabilities::All();
    caps.Remove(cap::kInventory);
    test::Harness quiet(content, {}, caps);
    REQUIRE(quiet.rc.StartRun("alpha", 3).ok());
    REQUIRE(quiet.rc.BeginArena(0.0).ok());
    test::KillN(quiet.rc, 2, 1.0);
    CHECK(quiet.rc.ConsumeItemGrants().empty());
}

// Supported third-party mods (2026-11-08): a mod skin is listed but not selectable until the
// adapter finds its mod's files; the content only takes a Nexus Mods page and bare archive names.
TEST_CASE("a mod skin needs its mod installed", "[characters]") {
    const json mod = {{"name", "Night Suit"},
                      {"author", "Someone"},
                      {"url", "https://www.nexusmods.com/residentevil5goldedition/mods/1"},
                      {"files", json::array({json{{"name", "uPl00ChrisCos1.arc"}, {"size", 1234}}})}};
    auto content = test::Load(test::EditFixture([&](json&, json&, json& characters, json&, json&) {
        characters["characters"][0]["skins"] = json::array({
            json{{"id", "alpha_base"}, {"name", "Base"}, {"costume_index", 0}},
            json{{"id", "alpha_mod"}, {"name", "Night Suit"}, {"costume_index", 0}, {"mod", mod}},
        });
    }));
    const SkinDef* skin = FindSkin(*content->FindCharacter("alpha"), "alpha_mod");
    REQUIRE(skin != nullptr);
    REQUIRE(skin->mod.has_value());
    CHECK(skin->mod->author == "Someone");
    REQUIRE(skin->mod->files.size() == 1);
    CHECK(skin->mod->files[0].size == 1234);

    test::Harness h(content);
    const auto gate = [&] {
        const auto characters = h.rc.Characters();
        for (const auto& s : Find(characters, "alpha")->skins) {
            if (s.id == "alpha_mod") return s.gate;
        }
        return CharacterGate::Unknown;
    };
    CHECK(gate() == CharacterGate::NotInstalled);
    CHECK(h.rc.StartRun("alpha", 5, "alpha_mod").code == ErrorCode::NotEligible);
    h.rc.SetInstalledModSkins({"alpha_mod"});
    CHECK(gate() == CharacterGate::Selectable);
    REQUIRE(h.rc.StartRun("alpha", 5, "alpha_mod").ok());
    CHECK(h.rc.state().skin_id == "alpha_mod");

    // Only a Nexus Mods page, only bare archive names, and the costume it stands in for.
    const auto errors = [&](const std::function<void(json&)>& edit) {
        json bad = {{"id", "alpha_mod"}, {"name", "Bad"}, {"costume_index", 0}, {"mod", mod}};
        edit(bad);
        return Content::LoadFromStrings(test::EditFixture([&](json&, json&, json& characters, json&, json&) {
                   characters["characters"][0]["skins"] = json::array({json{{"id", "alpha_base"}, {"name", "Base"}}, bad});
               }))
            .errors;
    };
    const auto has = [](const std::vector<std::string>& errs, std::string_view needle) {
        return std::any_of(errs.begin(), errs.end(), [&](const std::string& e) { return e.find(needle) != std::string::npos; });
    };
    CHECK(has(errors([](json& s) { s["mod"]["url"] = "https://example.com/x"; }), "nexusmods.com"));
    CHECK(has(errors([](json& s) { s["mod"]["files"][0]["name"] = "..\\x.arc"; }), "bare name"));
    CHECK(has(errors([](json& s) { s.erase("costume_index"); }), "costume_index it stands in for"));

    // A mod with variants of one file: any of their sizes.
    SkinModFile variants;
    variants.sizes = {100, 200};
    CHECK(variants.Accepts(200));
    CHECK_FALSE(variants.Accepts(150));
    SkinModFile any;
    CHECK(any.Accepts(12345));
    CHECK(has(errors([](json& s) { s["mod"]["files"][0]["sizes"] = json::array({0}); }), "positive whole number"));
}
