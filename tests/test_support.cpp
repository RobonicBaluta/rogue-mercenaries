#include "test_support.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>

#include <catch_amalgamated.hpp>

namespace rm::test {
namespace {

constexpr const char* kPerks = R"JSON({
  "schema_version": 1,
  "perks": [
    {"id": "dmg_up", "name": "Damage Up", "category": "style", "rarity": "common", "max_stacks": 3,
     "effects": [{"kind": "outgoing_damage_mult", "value": 0.1, "per_stack": {"value": 0.1}}]},
    {"id": "execute", "name": "Execute", "category": "style", "rarity": "uncommon",
     "effects": [{"kind": "outgoing_damage_mult", "value": 0.5, "condition": {"type": "target_hp_below", "fraction": 0.25}}]},
    {"id": "streak", "name": "Streak", "category": "style", "rarity": "rare", "max_stacks": 2,
     "effects": [{"kind": "outgoing_damage_mult",
                  "condition": {"type": "kill_streak", "window_seconds": 5, "per_kill": 0.1, "max_bonus": 0.3},
                  "per_stack": {"per_kill": 0.05, "max_bonus": 0.2}}]},
    {"id": "desperate", "name": "Desperate", "category": "style", "rarity": "uncommon",
     "effects": [{"kind": "outgoing_damage_mult", "value": 0.4, "condition": {"type": "player_hp_below", "fraction": 0.3}}]},
    {"id": "brawl", "name": "Brawl", "category": "style", "rarity": "rare", "requires_capabilities": ["melee_detect"],
     "effects": [{"kind": "outgoing_damage_mult", "value": 0.6, "condition": {"type": "after_melee", "window_seconds": 3}}]},
    {"id": "crit_up", "name": "Crit Up", "category": "style", "rarity": "uncommon",
     "effects": [{"kind": "crit", "chance": 0.25, "multiplier": 2.0}]},
    {"id": "armor", "name": "Armor", "category": "support", "rarity": "common", "max_stacks": 2,
     "effects": [{"kind": "incoming_damage_mult", "value": -0.2, "per_stack": {"value": -0.1}}]},
    {"id": "low_armor", "name": "Low Armor", "category": "support", "rarity": "rare",
     "effects": [{"kind": "incoming_damage_mult", "value": -0.5, "condition": {"type": "player_hp_below", "fraction": 0.25}}]},
    {"id": "medic", "name": "Medic", "category": "supply", "rarity": "uncommon", "requires_capabilities": ["heal"],
     "effects": [{"kind": "heal_on_kill", "amount_fraction_of_max": 0.05}]},
    {"id": "scav", "name": "Scavenger", "category": "supply", "rarity": "common",
     "effects": [{"kind": "supplies_on_kill", "amount": 3, "chance": 0.5}]},
    {"id": "sweeper", "name": "Sweeper", "category": "supply", "rarity": "rare",
     "effects": [{"kind": "supplies_on_arena_clear", "amount": 20, "require_no_damage": true}]},
    {"id": "stocker", "name": "Stocker", "category": "supply", "rarity": "common",
     "effects": [{"kind": "supplies_on_arena_clear", "amount": 10}]},
    {"id": "phoenix", "name": "Phoenix", "category": "support", "rarity": "epic",
     "effects": [{"kind": "cheat_death", "per_arena": 1, "leave_hp_fraction": 0.3}]},
    {"id": "berserk", "name": "Berserk", "category": "style", "rarity": "epic", "exclusive_with": ["turtle"],
     "effects": [{"kind": "outgoing_damage_mult", "value": 1.0}]},
    {"id": "turtle", "name": "Turtle", "category": "support", "rarity": "epic", "exclusive_with": ["berserk"],
     "effects": [{"kind": "incoming_damage_mult", "value": -0.5}]},
    {"id": "jackpot", "name": "Jackpot", "category": "style", "rarity": "legendary", "unlock_cost_credits": 100,
     "effects": [{"kind": "outgoing_damage_mult", "value": 0.2}]},
    {"id": "alpha_only", "name": "Alpha Only", "category": "style", "rarity": "rare", "characters": ["alpha"],
     "effects": [{"kind": "outgoing_damage_mult", "value": 0.15}]},
    {"id": "beta_passive", "name": "Beta Passive", "category": "style", "rarity": "common", "characters": ["beta"],
     "effects": [{"kind": "crit", "chance": 0.05, "multiplier": 1.5}]},
    {"id": "curse_weak", "name": "Curse Weak", "category": "curse", "rarity": "common",
     "effects": [{"kind": "incoming_damage_mult", "value": 0.25}]},
    {"id": "curse_soft", "name": "Curse Soft", "category": "curse", "rarity": "common",
     "effects": [{"kind": "outgoing_damage_mult", "value": -0.2}]},
    {"id": "coop_reserved", "name": "Co-op Reserved", "category": "support", "rarity": "rare", "enabled": false,
     "effects": [{"kind": "supplies_on_kill", "amount": 1}]}
  ]
})JSON";

constexpr const char* kWeapons = R"JSON({
  "schema_version": 1,
  "weapons": [
    {"id": "pistol", "name": "Pistol", "category": "handgun"},
    {"id": "shotgun", "name": "Shotgun", "category": "shotgun"},
    {"id": "rifle", "name": "Rifle", "category": "rifle", "base_damage_mult": 1.0, "unlock_cost_credits": 50}
  ],
  "affixes": [
    {"kind": "damage_pct", "min": 10, "max": 10},
    {"kind": "crit_chance", "min": 0.1, "max": 0.1},
    {"kind": "crit_mult_pct", "min": 20, "max": 20},
    {"kind": "low_hp_target_damage_pct", "min": 50, "max": 50, "threshold": 0.3},
    {"kind": "supplies_on_kill", "min": 2, "max": 2}
  ],
  "unique_affixes": [
    {"id": "executioner_rounds", "name": "Executioner Rounds", "effect": "low_hp_target_bonus", "value": 1.0, "threshold": 0.2},
    {"id": "bloodhound", "name": "Bloodhound", "effect": "kill_streak_extend", "seconds": 2}
  ]
})JSON";

constexpr const char* kCharacters = R"JSON({
  "schema_version": 1,
  "characters": [
    {"id": "alpha", "name": "Alpha", "starting_weapons": ["pistol", "shotgun"], "skill_pool": ["alpha_only"]},
    {"id": "beta", "name": "Beta", "starting_weapons": ["pistol"], "starting_passive": "beta_passive"},
    {"id": "gamma", "name": "Gamma", "starting_weapons": ["pistol"], "unlock_cost_credits": 100},
    {"id": "delta", "name": "Delta", "enabled": false, "starting_weapons": ["pistol"], "unlock_cost_credits": 50}
  ]
})JSON";

constexpr const char* kRoutes = R"JSON({
  "schema_version": 1,
  "routes": [
    {"id": "r1a", "name": "R1A", "difficulty_tier": 1, "enemy_budget": 5, "reward_mult": 1.0, "credit_mult": 1.0,
     "modifiers": {"incoming_damage_mult": 1.0, "outgoing_damage_mult": 1.0, "par_time_seconds": 100}},
    {"id": "r1b", "name": "R1B", "difficulty_tier": 1, "enemy_budget": 6, "reward_mult": 1.1, "credit_mult": 1.1,
     "modifiers": {"incoming_damage_mult": 1.0, "outgoing_damage_mult": 1.0, "par_time_seconds": 100}},
    {"id": "r2", "name": "R2", "difficulty_tier": 2, "enemy_budget": 7, "reward_mult": 1.3, "rarity_bonus": 0.2,
     "credit_mult": 1.3, "curse_chance": 1.0,
     "modifiers": {"incoming_damage_mult": 1.5, "outgoing_damage_mult": 1.0, "par_time_seconds": 120}},
    {"id": "r3", "name": "R3", "difficulty_tier": 3, "enemy_budget": 8, "reward_mult": 1.6, "rarity_bonus": 0.4,
     "credit_mult": 1.6, "modifiers": {"incoming_damage_mult": 2.0, "outgoing_damage_mult": 0.9, "par_time_seconds": 150}}
  ]
})JSON";

constexpr const char* kEconomy = R"JSON({
  "schema_version": 1,
  "run": {"arena_count": 3, "starting_route": "r1a", "starting_supplies": 50, "max_reroll_tokens": 2},
  "rewards": {"choices": 3,
    "rarity_weights": {"common": 50, "uncommon": 25, "rare": 15, "epic": 8, "legendary": 2},
    "rating_rarity_bonus": {"S": 0.3, "A": 0.15, "B": 0.0, "C": -0.1},
    "kind_weights": {"perk": 70, "weapon": 20, "supplies": 10},
    "supplies_bundle": 30},
  "weapon_rarity": {
    "common": {"affix_count": 0, "magnitude_mult": 1.0},
    "uncommon": {"affix_count": 1, "magnitude_mult": 1.0},
    "rare": {"affix_count": 2, "magnitude_mult": 1.0},
    "epic": {"affix_count": 3, "magnitude_mult": 1.0},
    "legendary": {"affix_count": 3, "magnitude_mult": 1.0, "unique_count": 1}},
  "upgrades": {"max_level": 5, "cost_to_next": [10, 20, 30, 40], "damage_pct_per_level": 10},
  "supplies": {"per_kill": 2, "per_arena_clear": 25},
  "credits": {"per_kill": 1, "per_arena_clear": 20, "run_complete_bonus": 50, "death_rate": 0.5,
              "rating_mult": {"S": 1.5, "A": 1.25, "B": 1.0, "C": 0.75}},
  "rating": {"s_time_fraction": 1.0, "a_time_fraction": 1.5, "low_damage_fraction": 0.35},
  "damage": {"outgoing_min": 0.25, "outgoing_max": 5.0, "incoming_min": 0.25, "incoming_max": 4.0,
             "default_crit_multiplier": 1.5, "max_crit_chance": 0.9},
  "shop": {"heal_fraction": 0.5, "heal_price": 20, "reroll_price": 15,
           "perk_price": {"common": 10, "uncommon": 20, "rare": 30, "epic": 40, "legendary": 50},
           "weapon_price": {"common": 10, "uncommon": 20, "rare": 30, "epic": 40, "legendary": 50},
           "perk_offers": 2},
  "routes": {"third_choice_chance": 0.5}
})JSON";

constexpr const char* kLoadouts = R"JSON({
  "schema_version": 1,
  "budget": 0,
  "equipment": [
    {"id": "grenade", "name": "Grenade", "cost": 5, "max": 3, "game_item": 262, "price": 10, "bundle": 2},
    {"id": "spray", "name": "Spray", "cost": 10, "max": 1, "game_item": 772, "price": 20},
    {"id": "herb", "name": "Herb", "cost": 3, "max": 2}
  ]
})JSON";

}  // namespace

ContentSources FixtureSources() {
    return ContentSources{kPerks, kWeapons, kCharacters, kRoutes, kEconomy, kLoadouts};
}

std::shared_ptr<const Content> Load(const ContentSources& sources) {
    ContentLoadResult r = Content::LoadFromStrings(sources);
    for (const std::string& e : r.errors) UNSCOPED_INFO(e);
    REQUIRE(r.ok());
    return r.content;
}

std::shared_ptr<const Content> LoadFixture() { return Load(FixtureSources()); }

ContentSources EditFixture(const std::function<void(nlohmann::json&, nlohmann::json&, nlohmann::json&,
                                                    nlohmann::json&, nlohmann::json&)>& edit) {
    nlohmann::json perks = nlohmann::json::parse(kPerks);
    nlohmann::json weapons = nlohmann::json::parse(kWeapons);
    nlohmann::json characters = nlohmann::json::parse(kCharacters);
    nlohmann::json routes = nlohmann::json::parse(kRoutes);
    nlohmann::json economy = nlohmann::json::parse(kEconomy);
    edit(perks, weapons, characters, routes, economy);
    return ContentSources{perks.dump(), weapons.dump(), characters.dump(), routes.dump(), economy.dump(), kLoadouts};
}

std::filesystem::path ConfigDir() { return std::filesystem::path(RM_SOURCE_DIR) / "configs"; }

TempDir::TempDir(const std::string& tag) {
    static std::atomic<int> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("rm_tests_" + tag + "_" + std::to_string(stamp) + "_" + std::to_string(counter++));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

std::string ReadText(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void WriteText(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

Harness::Harness(std::shared_ptr<const Content> content, Profile profile, Capabilities caps, ProfileStore* store)
    : rc(std::move(content), std::move(profile), RunControllerOptions{std::move(caps), &sink, store, 50}) {}

KillOutcome KillN(RunController& rc, int n, double t, const char* weapon) {
    KillOutcome last;
    for (int i = 0; i < n; ++i) {
        KillEvent k;
        k.now = t + 0.1 * i;
        k.by_player = true;
        if (weapon != nullptr) k.weapon_id = std::string(weapon);
        last = rc.OnEnemyKilled(k);
    }
    return last;
}

ArenaResult ClearArena(RunController& rc, double start_time) {
    REQUIRE(rc.BeginArena(start_time).ok());
    KillN(rc, rc.RemainingEnemies(), start_time + 1.0);
    REQUIRE(rc.IsArenaComplete());
    Result<ArenaResult> r = rc.CompleteArena(start_time + 30.0);
    REQUIRE(r.ok());
    return r.value();
}

}  // namespace rm::test
