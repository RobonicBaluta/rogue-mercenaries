#include "rm/routes.h"

#include <algorithm>
#include <set>

#include "rm/content.h"

namespace rm {

int SafeTier(const Content& content, int current_tier) {
    std::set<int> tiers;
    for (const RouteDef& r : content.routes()) {
        if (r.enabled) tiers.insert(r.difficulty_tier);
    }
    if (tiers.empty()) return current_tier;
    const int max_tier = *tiers.rbegin();
    int best = -1;
    for (const int t : tiers) {
        if (t <= current_tier && t < max_tier) best = t;
    }
    if (best == -1) best = tiers.size() > 1 ? *tiers.begin() : max_tier;
    return best;
}

std::vector<RouteChoice> GenerateRouteChoices(
    const Content& content, Rng& rng, int current_tier,
    const std::function<bool(const PerkDef&)>& curse_allowed) {
    const int safe = SafeTier(content, current_tier);
    std::vector<const RouteDef*> safe_pool;
    std::vector<const RouteDef*> hard_pool;
    for (const RouteDef& r : content.routes()) {
        if (!r.enabled) continue;
        (r.difficulty_tier <= safe ? safe_pool : hard_pool).push_back(&r);
    }

    std::vector<const RouteDef*> picked;
    auto take = [&rng, &picked](std::vector<const RouteDef*>& pool) {
        if (pool.empty()) return;
        const std::size_t i = rng.Index(pool.size());
        picked.push_back(pool[i]);
        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(i));
    };
    take(safe_pool);
    take(hard_pool);
    if (rng.Chance(content.economy().routes.third_choice_chance)) {
        std::vector<const RouteDef*> rest;
        for (const RouteDef& r : content.routes()) {
            if (r.enabled && std::find(picked.begin(), picked.end(), &r) == picked.end()) {
                rest.push_back(&r);
            }
        }
        take(rest);
    }
    rng.Shuffle(picked);

    std::vector<RouteChoice> out;
    StringSet curses_used;
    for (const RouteDef* r : picked) {
        RouteChoice choice;
        choice.route_id = r->id;
        if (r->curse_chance > 0.0 && rng.Chance(r->curse_chance)) {
            std::vector<const PerkDef*> curses;
            for (const PerkDef& p : content.perks()) {
                if (p.IsCurse() && curses_used.find(p.id) == curses_used.end() &&
                    (!curse_allowed || curse_allowed(p))) {
                    curses.push_back(&p);
                }
            }
            if (!curses.empty()) {
                const PerkDef* curse = curses[rng.Index(curses.size())];
                choice.curse_perk_id = curse->id;
                curses_used.insert(curse->id);
            }
        }
        out.push_back(std::move(choice));
    }
    return out;
}

}  // namespace rm
