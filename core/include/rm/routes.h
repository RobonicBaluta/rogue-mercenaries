#pragma once

#include <functional>
#include <string>
#include <vector>

#include "rm/rng.h"

namespace rm {

class Content;
struct PerkDef;

struct RouteModifiers {
    double incoming_damage_mult = 1.0;
    double outgoing_damage_mult = 1.0;
    double par_time_seconds = 180.0;
};

/// An encounter route (configs/routes.json): which arena variant to fight next and how it pays.
struct RouteDef {
    std::string id;
    std::string name;
    std::string description;
    bool enabled = true;
    int difficulty_tier = 1;  // 1..3
    int enemy_budget = 0;     // finite number of player kills that completes the arena
    double reward_mult = 1.0;   // scales supplies (per kill, clear, bundles)
    double rarity_bonus = 0.0;  // shifts reward/shop rarity weights (see ShiftRarityWeights)
    double elite_chance = 0.0;  // reserved: must be 0 until an adapter supports elites
    double credit_mult = 1.0;
    /// Chance that this route is offered carrying a curse perk (risk for reward).
    double curse_chance = 0.0;
    RouteModifiers modifiers;
};

/// One option on the route-selection screen; a non-empty curse is granted when chosen.
struct RouteChoice {
    std::string route_id;
    std::string curse_perk_id;
};

/// Tier treated as "safe" after completing a route of current_tier: current_tier when a
/// harder enabled tier exists, otherwise the highest enabled tier below it.
int SafeTier(const Content& content, int current_tier);

/// Draws 2 or 3 distinct enabled routes from `rng`: at least one with tier <= SafeTier()
/// and at least one harder; a third (any tier) with economy routes.third_choice_chance.
/// Routes with curse_chance roll for a curse among perks accepted by `curse_allowed`.
std::vector<RouteChoice> GenerateRouteChoices(
    const Content& content, Rng& rng, int current_tier,
    const std::function<bool(const PerkDef&)>& curse_allowed);

}  // namespace rm
