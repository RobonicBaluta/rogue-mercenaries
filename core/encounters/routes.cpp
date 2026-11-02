#include "rm/routes.h"

#include <algorithm>
#include <set>

#include "rm/content.h"

namespace rm {
namespace {

bool Playable(const RouteDef& r) { return r.enabled && r.enemy_budget > 0; }

// A random element of `pool` that is not in `avoid`; failing that, any element (-1 if empty).
int DrawArena(Rng& rng, const std::vector<int>& pool, const std::set<int>& avoid) {
    std::vector<int> fresh;
    for (const int key : pool) {
        if (avoid.count(key) == 0) fresh.push_back(key);
    }
    const std::vector<int>& from = fresh.empty() ? pool : fresh;
    return from.empty() ? -1 : from[rng.Index(from.size())];
}

}  // namespace

RunMap GenerateRunMap(const Content& content, Rng& rng, const std::vector<int>& arenas, int first) {
    const EconomyConfig& eco = content.economy();
    RunMap map;
    std::set<int> tier_set;
    for (const RouteDef& r : content.routes()) {
        if (Playable(r)) tier_set.insert(r.difficulty_tier);
    }
    const std::vector<int> tiers(tier_set.begin(), tier_set.end());
    const int layers = std::max(1, eco.run.arena_count);
    const int width = std::max(1, eco.run_map.width);

    RunMapNode start;
    start.route_id = eco.run.starting_route;
    start.map_key = first >= 0 ? first : DrawArena(rng, arenas, {});
    map.layers.push_back({start});

    for (int d = 1; d < layers; ++d) {
        std::vector<RunMapNode> layer(static_cast<std::size_t>(width));
        // Links into this layer: the start reaches every node; node j of a wider layer reaches
        // j and j+1 (the last one its left neighbour and itself).
        std::vector<RunMapNode>& prev = map.layers.back();
        for (std::size_t j = 0; j < prev.size(); ++j) {
            if (d == 1 || width == 1) {
                for (int k = 0; k < (d == 1 ? width : 1); ++k) prev[j].next.push_back(k);
            } else {
                const int a = std::min(static_cast<int>(j), width - 2);
                prev[j].next = {a, a + 1};
            }
        }
        // Tiers: neighbours alternate between the layer's lower and higher tier.
        const int low = tiers.empty() ? 1 : tiers[static_cast<std::size_t>(std::min<int>(d - 1, static_cast<int>(tiers.size()) - 1))];
        const int high = tiers.empty() ? 1 : tiers[static_cast<std::size_t>(std::min<int>(d, static_cast<int>(tiers.size()) - 1))];
        const std::size_t phase = rng.Index(2);
        std::set<std::string> routes_used;
        std::set<int> arenas_used;
        for (std::size_t k = 0; k < layer.size(); ++k) {
            const int tier = (k + phase) % 2 == 0 ? low : high;
            std::vector<const RouteDef*> pool;
            std::vector<const RouteDef*> fresh;
            for (const RouteDef& r : content.routes()) {
                if (!Playable(r) || r.difficulty_tier != tier) continue;
                pool.push_back(&r);
                if (routes_used.count(r.id) == 0) fresh.push_back(&r);
            }
            const std::vector<const RouteDef*>& from = fresh.empty() ? pool : fresh;
            if (!from.empty()) layer[k].route_id = from[rng.Index(from.size())]->id;
            routes_used.insert(layer[k].route_id);

            std::set<int> avoid = arenas_used;
            for (const RunMapNode& p : prev) {
                if (std::find(p.next.begin(), p.next.end(), static_cast<int>(k)) != p.next.end()) avoid.insert(p.map_key);
            }
            layer[k].map_key = DrawArena(rng, arenas, avoid);
            arenas_used.insert(layer[k].map_key);
        }
        map.layers.push_back(std::move(layer));
    }
    return map;
}

void RollRouteCurses(const Content& content, Rng& rng, std::vector<RouteChoice>& choices,
                     const std::function<bool(const PerkDef&)>& curse_allowed) {
    StringSet curses_used;
    for (RouteChoice& choice : choices) {
        const RouteDef* r = content.FindRoute(choice.route_id);
        if (r == nullptr || r->curse_chance <= 0.0 || !rng.Chance(r->curse_chance)) continue;
        std::vector<const PerkDef*> curses;
        for (const PerkDef& p : content.perks()) {
            if (p.IsCurse() && curses_used.find(p.id) == curses_used.end() && (!curse_allowed || curse_allowed(p))) {
                curses.push_back(&p);
            }
        }
        if (!curses.empty()) {
            const PerkDef* curse = curses[rng.Index(curses.size())];
            choice.curse_perk_id = curse->id;
            curses_used.insert(curse->id);
        }
    }
}

}  // namespace rm
