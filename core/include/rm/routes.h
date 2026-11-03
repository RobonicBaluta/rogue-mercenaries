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

/// One option on the route-selection screen: a node of the next layer of the run map. A
/// non-empty curse is granted when chosen.
struct RouteChoice {
    std::string route_id;
    std::string curse_perk_id;
    int map_key = -1;  // the arena (maps.json "stage" key); -1: the adapter picks one
    int node = -1;     // the node's index in its layer
};

/// One arena of the run map: where (map key) and how it is fought (route: tier, budget, rewards).
struct RunMapNode {
    int map_key = -1;
    std::string route_id;
    std::vector<int> next;  // the nodes of the following layer it leads to
    /// Enemy classes of other maps this arena adds to its map's own (ROADMAP M3.3,
    /// rm::PlanRunRosters); empty: the map's own roster.
    std::vector<std::string> guests;
};

/// The arenas of a run, generated when it starts: one layer per arena, the first holding only
/// the starting arena. The start leads to every node of layer 1; every other node to two
/// neighbouring nodes of the next layer, whose tiers differ (a safer and a harder option), so a
/// choice also decides which arenas can follow.
struct RunMap {
    std::vector<std::vector<RunMapNode>> layers;
};

/// economy run.arena_count layers of economy run_map.width nodes. Layer 0 is the starting route
/// on `first` (or an arena drawn from `arenas` when -1); layer d >= 1 alternates the d-th and
/// (d+1)-th lowest enabled tiers (the highest repeats). Arenas come from `arenas` (map keys the
/// adapter can load; empty: every map_key stays -1), distinct within a layer and never the
/// arena a node is reached from, while there are enough of them. Boss arenas (maps with a
/// boss) only fill the last layer: up to `width` of them, reached from every node before, fought
/// with a route of the highest tier.
RunMap GenerateRunMap(const Content& content, Rng& rng, const std::vector<int>& arenas, int first);

/// Rolls the curses of route choices: a choice whose route has curse_chance may carry one curse
/// accepted by `curse_allowed`, different from the other choices' curses.
void RollRouteCurses(const Content& content, Rng& rng, std::vector<RouteChoice>& choices,
                     const std::function<bool(const PerkDef&)>& curse_allowed);

}  // namespace rm
