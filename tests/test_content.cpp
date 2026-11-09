#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "rm/content.h"
#include "test_support.h"

using namespace rm;
using nlohmann::json;

namespace {

using Edit = std::function<void(json&, json&, json&, json&, json&)>;

std::vector<std::string> ErrorsFor(const Edit& edit) {
    ContentLoadResult r = Content::LoadFromStrings(test::EditFixture(edit));
    CHECK_FALSE(r.ok());
    CHECK(r.content == nullptr);
    return r.errors;
}

bool HasError(const std::vector<std::string>& errors, const std::string& needle) {
    const bool found = std::any_of(errors.begin(), errors.end(),
                                   [&](const std::string& e) { return e.find(needle) != std::string::npos; });
    if (!found) {
        for (const std::string& e : errors) UNSCOPED_INFO("got: " << e);
    }
    return found;
}

// Lines the native menu's help takes (adapters/re5/src/native_menu.cpp Wrap: words onto lines
// of 54; it shows two).
int HelpLines(const std::string& text) {
    int lines = 0;
    std::size_t line = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t end = text.find(' ', i);
        if (end == std::string::npos) end = text.size();
        const std::size_t word = end - i;
        if (line > 0 && line + 1 + word > 54) {
            ++lines;
            line = 0;
        }
        line += (line > 0 ? 1 : 0) + word;
        i = end + 1;
    }
    return lines + (line > 0 ? 1 : 0);
}

json& PerkById(json& perks, const std::string& id) {
    for (json& p : perks["perks"]) {
        if (p["id"] == id) return p;
    }
    FAIL("perk not found: " << id);
    return perks;
}

}  // namespace

TEST_CASE("shipped configs load without errors and meet content requirements", "[content]") {
    ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    for (const std::string& e : r.errors) UNSCOPED_INFO(e);
    REQUIRE(r.ok());
    const Content& c = *r.content;

    std::set<PerkCategory> categories;
    std::set<Rarity> rarities;
    int curses = 0;
    int enabled_perks = 0;
    for (const PerkDef& p : c.perks()) {
        if (!p.enabled) continue;
        ++enabled_perks;
        categories.insert(p.category);
        rarities.insert(p.rarity);
        if (p.IsCurse()) ++curses;
    }
    CHECK(c.perks().size() >= 14);
    CHECK(enabled_perks >= 14);
    for (PerkCategory cat : {PerkCategory::Style, PerkCategory::Supply, PerkCategory::Support, PerkCategory::Skin}) {
        CHECK(categories.count(cat) == 1);
    }
    CHECK(rarities.size() >= 4);
    CHECK(curses >= 2);
    // Every perk the menus list has an icon, and its picture ships in configs/icons.
    for (const PerkDef& p : c.perks()) {
        if (!p.enabled || p.category == PerkCategory::Skin || p.IsCurse()) continue;
        INFO(p.id);
        CHECK_FALSE(p.icon.empty());
        CHECK(std::filesystem::exists(test::ConfigDir() / "icons" / (p.icon + ".png")));
    }
    // Every skin is a build with a name and an icon (M8); a special's icon ships too.
    for (const CharacterDef& ch : c.characters()) {
        for (const SkinDef& s : ch.skins) {
            if (s.build.name.empty() && s.build.summary.empty()) continue;  // a plain default skin
            INFO(s.id);
            CHECK_FALSE(s.build.name.empty());
            CHECK_FALSE(s.build.icon.empty());
            CHECK(std::filesystem::exists(test::ConfigDir() / "icons" / (s.build.icon + ".png")));
            CHECK(HelpLines(s.build.summary) <= 2);  // SKINS and PERKS show it whole
            if (s.build.special.Any()) {
                CHECK(std::filesystem::exists(test::ConfigDir() / "icons" / (s.build.special.icon + ".png")));
            }
        }
    }

    // Character restrictions and capability flags.
    const CharacterDef* chris = c.FindCharacter("chris");
    const CharacterDef* sheva = c.FindCharacter("sheva");
    REQUIRE(chris != nullptr);
    REQUIRE(sheva != nullptr);
    CHECK(chris->enabled);
    CHECK(sheva->enabled);
    CHECK(chris->unlock_cost_credits == 0);
    CHECK(sheva->unlock_cost_credits == 0);
    // Only Chris and Sheva with their first skins are open from the start; everything else is earned
    // (the user, 2026-11-08: "except Sheva base and Chris base all the other characters should be
    // unlocked via meta achievements").
    const auto earnable = [&c](const Challenge& u) {
        CHECK(u.count >= 1);
        // Drawn by the game font; the menus add the full stop.
        CHECK_FALSE(u.text.empty());
        CHECK(u.text.back() != '.');
        CHECK(u.text.size() <= 44);
        for (const char ch : u.text) CHECK((std::isalnum(static_cast<unsigned char>(ch)) || ch == ' ' || ch == ',' || ch == '.'));
        if (u.kind == ChallengeKind::RunsCompletedAs) CHECK(c.FindCharacter(u.character) != nullptr);
        if (u.kind == ChallengeKind::BossBeaten) {
            // A boss of the ladder, one a run really fights; Uroboros Mkono's end is not known yet.
            bool reachable = false;
            const auto& ladder = c.economy().ladder;
            std::vector<int> keys{ladder.finale.boss};
            for (const auto& l : ladder.lengths) {
                keys.push_back(l.easy_boss);
                keys.insert(keys.end(), l.hard_bosses.begin(), l.hard_bosses.end());
            }
            for (const int key : keys) {
                const MapDef* m = c.FindMapByStage(key);
                reachable = reachable || (m != nullptr && m->boss && "boss:" + m->boss->unit_class == u.boss);
            }
            CHECK(reachable);
            CHECK(u.boss != "boss:uEm42");
        }
    };
    for (const CharacterDef& ch : c.characters()) {
        INFO(ch.id);
        CHECK(ch.enabled);
        CHECK(ch.unlock_cost_credits == 0);
        REQUIRE_FALSE(ch.skins.empty());
        const bool base = ch.id == "chris" || ch.id == "sheva";
        if (base) {
            CHECK(ch.unlock.kind == ChallengeKind::None);
            CHECK(CheckCharacter(&ch, {}) == CharacterGate::Selectable);
        } else if (ch.id != "wesker") {  // his two skins are his locks
            CHECK(ch.unlock.kind != ChallengeKind::None);
            earnable(ch.unlock);
            CHECK(CheckCharacter(&ch, {}) == CharacterGate::Locked);
        }
        for (std::size_t k = 0; k < ch.skins.size(); ++k) {
            const SkinDef& skin = ch.skins[k];
            INFO(skin.id);
            if (k == 0 && ch.id != "wesker") {
                CHECK(skin.unlock.kind == ChallengeKind::None);  // it comes with the character
                continue;
            }
            CHECK(skin.unlock.kind != ChallengeKind::None);
            CHECK(CheckSkin(skin, {}) == CharacterGate::Locked);
            earnable(skin.unlock);
        }
    }
    const PerkDef* boulder = c.FindPerk("chris_pocket_grenades");
    const PerkDef* marksman = c.FindPerk("sheva_flash_tactics");
    REQUIRE(boulder != nullptr);
    REQUIRE(marksman != nullptr);
    CHECK(CharacterMayUsePerk(*boulder, *chris));
    CHECK_FALSE(CharacterMayUsePerk(*boulder, *sheva));
    CHECK(CharacterMayUsePerk(*marksman, *sheva));
    CHECK_FALSE(CharacterMayUsePerk(*marksman, *chris));
    auto requires_cap = [](const PerkDef& p, std::string_view cap) {
        return std::find(p.requires_capabilities.begin(), p.requires_capabilities.end(), cap) !=
               p.requires_capabilities.end();
    };
    // Character traits hand out items instead of scaling damage: the loader derives the inventory need.
    CHECK(requires_cap(*boulder, cap::kInventory));
    CHECK(requires_cap(*boulder, cap::kKillEvents));
    CHECK(requires_cap(*marksman, cap::kInventory));
    CHECK(requires_cap(*c.FindPerk("field_medic"), cap::kHeal));
    CHECK(c.weapons().size() >= 8);
    CHECK(c.unique_affixes().size() >= 2);
    CHECK(c.economy().run.arena_count == 4);  // three arenas, then the boss (M6)
    CHECK(std::any_of(c.maps().begin(), c.maps().end(), [](const MapDef& m) { return m.boss.has_value(); }));
}

TEST_CASE("loader derives implied capabilities and symmetric exclusions", "[content]") {
    auto content = test::LoadFixture();
    const PerkDef* execute = content->FindPerk("execute");
    REQUIRE(execute != nullptr);
    CHECK(execute->requires_capabilities == std::vector<std::string>{"enemy_hp", "outgoing_damage"});
    const PerkDef* medic = content->FindPerk("medic");
    CHECK(medic->requires_capabilities == std::vector<std::string>{"heal", "kill_events"});
    const PerkDef* sweeper = content->FindPerk("sweeper");
    CHECK(sweeper->requires_capabilities == std::vector<std::string>{"incoming_damage"});
    const PerkDef* stocker = content->FindPerk("stocker");
    CHECK(stocker->requires_capabilities.empty());

    // One-sided exclusion is normalised to both directions.
    ContentSources src = test::EditFixture([](json& perks, json&, json&, json&, json&) {
        PerkById(perks, "turtle").erase("exclusive_with");
    });
    auto edited = test::Load(src);
    const PerkDef* turtle = edited->FindPerk("turtle");
    CHECK(std::count(turtle->exclusive_with.begin(), turtle->exclusive_with.end(), "berserk") == 1);
}

TEST_CASE("broken configs report human-readable errors", "[content]") {
    SECTION("duplicate ids") {
        auto errors = ErrorsFor([](json& perks, json&, json& chars, json&, json&) {
            perks["perks"].push_back(PerkById(perks, "dmg_up"));
            chars["characters"].push_back(chars["characters"][0]);
        });
        CHECK(HasError(errors, "perks.json: duplicate perk id 'dmg_up'"));
        CHECK(HasError(errors, "characters.json: duplicate character id 'alpha'"));
    }
    SECTION("unknown references") {
        auto errors = ErrorsFor([](json& perks, json&, json& chars, json&, json& eco) {
            PerkById(perks, "berserk")["exclusive_with"] = json::array({"nope"});
            PerkById(perks, "alpha_only")["characters"] = json::array({"omega"});
            chars["characters"][0]["starting_weapons"] = json::array({"railgun"});
            chars["characters"][1]["starting_passive"] = "ghost";
            chars["characters"][0]["skill_pool"] = json::array({"missing_perk"});
            eco["run"]["starting_route"] = "nowhere";
        });
        CHECK(HasError(errors, "perk 'berserk': exclusive_with references unknown perk 'nope'"));
        CHECK(HasError(errors, "perk 'alpha_only': characters references unknown character 'omega'"));
        CHECK(HasError(errors, "character 'alpha': starting_weapons references unknown weapon 'railgun'"));
        CHECK(HasError(errors, "character 'beta': starting_passive references unknown perk 'ghost'"));
        CHECK(HasError(errors, "skill_pool references unknown perk 'missing_perk'"));
        CHECK(HasError(errors, "run.starting_route references unknown route 'nowhere'"));
    }
    SECTION("invalid ranges") {
        auto errors = ErrorsFor([](json& perks, json&, json&, json& routes, json& eco) {
            PerkById(perks, "dmg_up")["max_stacks"] = 0;
            PerkById(perks, "crit_up")["effects"][0]["chance"] = 1.5;
            PerkById(perks, "armor")["effects"][0]["per_stack"]["value"] = -0.9;  // -1.1 at max stacks
            routes["routes"][0]["enemy_budget"] = 0;
            routes["routes"][1]["difficulty_tier"] = 4;
            routes["routes"][2]["elite_chance"] = 0.2;
            eco["upgrades"]["cost_to_next"] = json::array({10, 20});
            eco["damage"]["outgoing_min"] = 6.0;
        });
        CHECK(HasError(errors, "perk 'dmg_up': field 'max_stacks' = 0 must be in [1, 99]"));
        CHECK(HasError(errors, "perk 'crit_up': effect[0]: field 'chance' = 1.5 must be in (0, 1]"));
        CHECK(HasError(errors, "perk 'armor': effect[0] at max_stacks=2: field 'value' = -1.1"));
        CHECK(HasError(errors, "route 'r1a': field 'enemy_budget' = 0 must be in [1, 1000]"));
        CHECK(HasError(errors, "route 'r1b': field 'difficulty_tier' = 4 must be in [1, 3]"));
        CHECK(HasError(errors, "route 'r2': field 'elite_chance' is reserved"));
        CHECK(HasError(errors, "cost_to_next must have max_level - 1 = 4 entries (has 2)"));
        CHECK(HasError(errors, "outgoing clamp must satisfy"));
    }
    SECTION("exclusions that make a perk unpickable") {
        auto errors = ErrorsFor([](json& perks, json&, json& chars, json&, json&) {
            PerkById(perks, "dmg_up")["exclusive_with"] = json::array({"dmg_up"});
            // beta's passive excludes beta-only perk -> nobody can ever take it.
            json beta_only = {{"id", "beta_only"}, {"name", "Beta Only"}, {"category", "offensive"},
                              {"rarity", "rare"}, {"characters", json::array({"beta"})},
                              {"effects", json::array({{{"kind", "outgoing_damage_mult"}, {"value", 0.1}}})}};
            perks["perks"].push_back(beta_only);
            PerkById(perks, "beta_passive")["exclusive_with"] = json::array({"beta_only"});
            // alpha's skill_pool perk excluded by alpha's own passive.
            chars["characters"][0]["starting_passive"] = "berserk";
            chars["characters"][0]["skill_pool"] = json::array({"alpha_only", "turtle"});
        });
        CHECK(HasError(errors, "perk 'dmg_up': excludes itself, so it can never be picked"));
        CHECK(HasError(errors, "perk 'beta_only': every character allowed to take it starts with an excluding passive"));
        CHECK(HasError(errors, "skill_pool perk 'turtle' is excluded by the starting passive 'berserk'"));
    }
    SECTION("unsupported kinds, unknown capabilities and fields") {
        auto errors = ErrorsFor([](json& perks, json& weapons, json&, json&, json&) {
            PerkById(perks, "dmg_up")["effects"][0]["kind"] = "teleport";
            PerkById(perks, "execute")["effects"][0]["condition"]["type"] = "target_hp_above";
            PerkById(perks, "armor")["effects"][0]["condition"] = {{"type", "kill_streak"}, {"window_seconds", 3},
                                                                   {"per_kill", 0.1}, {"max_bonus", 0.2}};
            PerkById(perks, "medic")["requires_capabilities"] = json::array({"telepathy"});
            PerkById(perks, "scav")["max_stack"] = 2;
            PerkById(perks, "turtle")["icon"] = "Shield.png";
            PerkById(perks, "crit_up")["effects"][0]["per_stack"] = {{"value", 0.1}};
            weapons["affixes"][0]["kind"] = "lifesteal";
        });
        CHECK(HasError(errors, "unsupported kind 'teleport'"));
        CHECK(HasError(errors, "unsupported condition 'target_hp_above'"));
        CHECK(HasError(errors, "condition 'kill_streak' is not supported for effect kind 'incoming_damage_mult'"));
        CHECK(HasError(errors, "perk 'medic': unknown capability 'telepathy'"));
        CHECK(HasError(errors, "perk 'scav': unknown field 'max_stack'"));
        CHECK(HasError(errors, "perk 'turtle': field 'icon' 'Shield.png' must use only a-z, 0-9 and '_'"));
        CHECK(HasError(errors, "per_stack key 'value' is not a parameter of this effect"));
        CHECK(HasError(errors, "unsupported kind 'lifesteal'"));
    }
    SECTION("invalid JSON and route tiers") {
        ContentSources src = test::FixtureSources();
        src.perks = src.perks.substr(0, src.perks.size() / 2);  // truncated
        src.routes = R"({"schema_version": 1, "routes": [
            {"id": "only", "name": "Only", "difficulty_tier": 1, "enemy_budget": 5, "reward_mult": 1, "credit_mult": 1,
             "modifiers": {"incoming_damage_mult": 1, "outgoing_damage_mult": 1, "par_time_seconds": 60}}]})";
        ContentLoadResult r = Content::LoadFromStrings(src);
        CHECK_FALSE(r.ok());
        CHECK(HasError(r.errors, "perks.json: invalid JSON"));
        CHECK(HasError(r.errors, "at least two difficulty tiers"));
    }
    SECTION("missing directory") {
        ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir() / "does_not_exist");
        CHECK_FALSE(r.ok());
        CHECK(HasError(r.errors, "perks.json: cannot read"));
    }
}

TEST_CASE("every perk with a price fits it on its menu row", "[content]") {
    ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    constexpr std::size_t kMenuRowChars = 34;  // adapter native_menu.cpp kRowChars: a longer row is cut
    for (const PerkDef& p : r.content->perks()) {
        if (p.unlock_cost_credits <= 0) continue;
        INFO(p.id);
        CHECK(p.name.size() + 3 + std::to_string(p.unlock_cost_credits).size() + 8 <= kMenuRowChars);  // "NAME   150 CREDITS"
    }
}
