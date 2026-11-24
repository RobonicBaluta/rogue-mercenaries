#include "rm/rosters.h"

#include <algorithm>
#include <limits>

#include "rm/content.h"

namespace rm {

std::optional<MapEnemy> GuestEntry(const Content& content, const std::string& unit_class) {
    const auto& guests = content.economy().run_rosters.guests;
    const auto it = guests.find(unit_class);
    if (it == guests.end()) return std::nullopt;
    const MapDef* home = content.FindMapByStage(it->second);
    if (home == nullptr) return std::nullopt;
    for (const MapEnemy& e : home->roster) {
        if (e.unit_class != unit_class) continue;
        MapEnemy out = e;
        out.home = home->stage;
        return out;
    }
    return std::nullopt;
}

std::string GuestHomes(const Content& content, const std::vector<std::string>& guests) {
    std::vector<std::string> names;
    for (const std::string& g : guests) {
        const std::optional<MapEnemy> e = GuestEntry(content, g);
        const MapDef* home = e ? content.FindMapByStage(e->home) : nullptr;
        if (home != nullptr && std::find(names.begin(), names.end(), home->name) == names.end()) names.push_back(home->name);
    }
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += i + 1 == names.size() ? " and " : ", ";
        out += names[i];
    }
    return out;
}

void PlanRunRosters(const Content& content, Rng& rng, RunMap& map) {
    const EconomyConfig& eco = content.economy();
    const EconomyConfig::RunRosters& cfg = eco.run_rosters;
    if (!cfg.enabled) return;
    const EconomyConfig::EnemyThreat& threat = eco.enemy_threat;
    // Every guest that can travel, by class (the map is sorted, so the plan is the same for a seed).
    StringMap<MapEnemy> pool;
    for (const auto& [unit_class, home] : cfg.guests) {
        if (std::optional<MapEnemy> e = GuestEntry(content, unit_class)) pool.emplace(unit_class, std::move(*e));
    }
    // What a path into each node has fielded: the classes of every arena on the way, its own
    // included. A node reached from two nodes counts, per class, the paths that fielded it.
    std::vector<std::vector<StringSet>> seen(map.layers.size());
    for (std::size_t d = 0; d < map.layers.size(); ++d) {
        seen[d].resize(map.layers[d].size());
        StringMap<int> layer_guests;  // guests already given in this layer (spread them out)
        for (std::size_t n = 0; n < map.layers[d].size(); ++n) {
            RunMapNode& node = map.layers[d][n];
            node.guests.clear();
            StringMap<int> fielded;  // class -> paths into this node that fielded it
            StringSet& path = seen[d][n];
            if (d > 0) {
                for (std::size_t p = 0; p < map.layers[d - 1].size(); ++p) {
                    const std::vector<int>& next = map.layers[d - 1][p].next;
                    if (std::find(next.begin(), next.end(), static_cast<int>(n)) == next.end()) continue;
                    for (const std::string& c : seen[d - 1][p]) {
                        ++fielded[c];
                        path.insert(c);
                    }
                }
            }
            const MapDef* m = content.FindMapByStage(node.map_key);
            if (m == nullptr || m->boss) continue;
            StringSet classes;
            for (const MapEnemy& e : m->roster) {
                classes.insert(e.unit_class);
                path.insert(e.unit_class);
            }
            const int arena = static_cast<int>(d) + 1;
            if (arena < cfg.from_arena) continue;
            const RouteDef* route = content.FindRoute(node.route_id);
            const int level = threat.Level(arena, route != nullptr ? route->difficulty_tier : 1);  // 0: threat off
            const bool specials_join = level <= 0 || level >= threat.specials_from_level;
            int room = cfg.max_classes - static_cast<int>(classes.size());
            // The ones that come to every arena from their level (the dogs), first.
            if (level > 0) {
                for (const auto& [unit_class, from] : cfg.always_from_level) {
                    const auto e = pool.find(unit_class);
                    if (level < from || e == pool.end() || classes.count(unit_class) != 0) continue;
                    if (threat.table.Lowest(e->second) > level || (e->second.special && !specials_join)) continue;
                    node.guests.push_back(unit_class);
                    classes.insert(unit_class);
                    path.insert(unit_class);
                    ++layer_guests[unit_class];
                    --room;
                }
            }
            for (const bool special : {false, true}) {
                const int want = special ? (specials_join ? cfg.special_guests : 0) : cfg.normal_guests;
                for (int k = 0; k < want && room > 0; ++k) {
                    // The least fielded candidates (then the least given in this layer).
                    std::vector<const MapEnemy*> picks;
                    std::vector<double> weights;
                    std::pair<int, int> best{std::numeric_limits<int>::max(), 0};
                    for (const auto& [unit_class, e] : pool) {
                        if (e.special != special || classes.count(unit_class) != 0) continue;
                        const int lowest = level > 0 ? threat.table.Lowest(e) : 1;
                        if (level > 0 && lowest > level) continue;
                        const auto f = fielded.find(unit_class);
                        const auto g = layer_guests.find(unit_class);
                        const std::pair<int, int> used{f != fielded.end() ? f->second : 0, g != layer_guests.end() ? g->second : 0};
                        if (used > best) continue;
                        if (used < best) {
                            best = used;
                            picks.clear();
                            weights.clear();
                        }
                        picks.push_back(&e);
                        weights.push_back(static_cast<double>(std::max(1, lowest)));  // harder: likelier
                    }
                    if (picks.empty()) break;
                    const std::size_t i = rng.WeightedIndex(weights);
                    const MapEnemy& chosen = *picks[i < picks.size() ? i : 0];
                    node.guests.push_back(chosen.unit_class);
                    classes.insert(chosen.unit_class);
                    path.insert(chosen.unit_class);
                    ++layer_guests[chosen.unit_class];
                    --room;
                }
            }
        }
    }
}

MapDef WithGuests(const Content& content, const MapDef& map, const std::vector<std::string>& guests) {
    MapDef out = map;
    if (guests.empty() || map.boss) return out;
    const EconomyConfig::RunRosters& cfg = content.economy().run_rosters;
    double own_normal = 0.0, own_special = 0.0;
    for (const MapEnemy& e : map.roster) (e.special ? own_special : own_normal) += std::max(0.0, e.weight);
    std::vector<MapEnemy> normal, special;
    for (const std::string& g : guests) {
        const bool present = std::any_of(out.roster.begin(), out.roster.end(), [&](const MapEnemy& e) { return e.unit_class == g; }) ||
                             std::any_of(normal.begin(), normal.end(), [&](const MapEnemy& e) { return e.unit_class == g; }) ||
                             std::any_of(special.begin(), special.end(), [&](const MapEnemy& e) { return e.unit_class == g; });
        if (present) continue;
        if (std::optional<MapEnemy> e = GuestEntry(content, g)) (e->special ? special : normal).push_back(std::move(*e));
    }
    // The guests of a kind together weigh `share` of that kind's picks: G / (own + G) = share.
    auto weigh = [](std::vector<MapEnemy>& list, double own, double share) {
        if (list.empty() || own <= 0.0) return;  // nothing of their own to share with: home weights
        const double each = own * share / (1.0 - share) / static_cast<double>(list.size());
        for (MapEnemy& e : list) e.weight = each;
    };
    weigh(normal, own_normal, cfg.normal_share);
    weigh(special, own_special, cfg.special_share);
    for (MapEnemy& e : normal) out.roster.push_back(std::move(e));
    for (MapEnemy& e : special) out.roster.push_back(std::move(e));
    return out;
}

}  // namespace rm
