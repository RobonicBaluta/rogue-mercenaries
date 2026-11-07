#include "rm/run_resume.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>

#include <nlohmann/json.hpp>

#include "rm/rarity.h"

namespace rm {
namespace {

using nlohmann::json;

// Finite numbers only (nlohmann writes NaN and infinity as null).
double Fin(double v) { return std::isfinite(v) ? v : 0.0; }

// A field of the expected type, else `def` (missing, null or another type).
template <class T>
T Get(const json& j, const char* key, T def) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if constexpr (std::is_same_v<T, bool>) {
        return it->is_boolean() ? it->get<bool>() : def;
    } else if constexpr (std::is_arithmetic_v<T>) {
        return it->is_number() ? it->get<T>() : def;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return it->is_string() ? it->get<std::string>() : def;
    } else {
        try {
            return it->get<T>();
        } catch (const json::exception&) {
            return def;
        }
    }
}

json MapToJson(const StringMap<int>& m) {
    json out = json::object();
    for (const auto& [k, v] : m) out[k] = v;
    return out;
}

void MapFromJson(const json& j, StringMap<int>& out) {
    if (!j.is_object()) return;
    for (const auto& [k, v] : j.items()) {
        if (v.is_number_integer()) out[k] = v.get<int>();
    }
}

json WeaponToJson(const WeaponInstance& w) {
    json affixes = json::array();
    for (const Affix& a : w.affixes) {
        affixes.push_back({{"kind", static_cast<int>(a.kind)}, {"value", Fin(a.value)}, {"threshold", Fin(a.threshold)}, {"unique_id", a.unique_id}});
    }
    return {{"instance_id", w.instance_id}, {"base_id", w.base_id}, {"level", w.level},
            {"rarity", std::string(RarityName(w.rarity))}, {"affixes", std::move(affixes)}};
}

WeaponInstance WeaponFromJson(const json& j) {
    WeaponInstance w;
    w.instance_id = Get(j, "instance_id", std::string());
    w.base_id = Get(j, "base_id", std::string());
    w.level = Get(j, "level", 1);
    w.rarity = ParseRarity(Get(j, "rarity", std::string("common"))).value_or(Rarity::Common);
    for (const json& a : Get(j, "affixes", json::array())) {
        Affix affix;
        affix.kind = static_cast<AffixKind>(Get(a, "kind", 0));
        affix.value = Get(a, "value", 0.0);
        affix.threshold = Get(a, "threshold", 0.0);
        affix.unique_id = Get(a, "unique_id", std::string());
        w.affixes.push_back(std::move(affix));
    }
    return w;
}

json MetricsToJson(const ArenaMetrics& a) {
    return {{"arena", a.arena},
            {"route_id", a.route_id},
            {"budget", a.budget},
            {"start_time", Fin(a.start_time)},
            {"end_time", Fin(a.end_time)},
            {"player_kills", a.player_kills},
            {"budget_kills", a.budget_kills},
            {"non_player_kills", a.non_player_kills},
            {"duplicate_kills", a.duplicate_kills},
            {"damage_events", a.damage_events},
            {"damage_taken", Fin(a.damage_taken)},
            {"hp_max_seen", Fin(a.hp_max_seen)},
            {"deaths", a.deaths},
            {"supplies_awarded", a.supplies_awarded},
            {"credits_accrued", a.credits_accrued},
            {"cheat_death_used", a.cheat_death_used},
            {"completed", a.completed}};
}

ArenaMetrics MetricsFromJson(const json& j) {
    ArenaMetrics a;
    a.arena = Get(j, "arena", 0);
    a.route_id = Get(j, "route_id", std::string());
    a.budget = Get(j, "budget", 0);
    a.start_time = Get(j, "start_time", 0.0);
    a.end_time = Get(j, "end_time", 0.0);
    a.player_kills = Get(j, "player_kills", 0);
    a.budget_kills = Get(j, "budget_kills", 0);
    a.non_player_kills = Get(j, "non_player_kills", 0);
    a.duplicate_kills = Get(j, "duplicate_kills", 0);
    a.damage_events = Get(j, "damage_events", 0);
    a.damage_taken = Get(j, "damage_taken", 0.0);
    a.hp_max_seen = Get(j, "hp_max_seen", 0.0);
    a.deaths = Get(j, "deaths", 0);
    a.supplies_awarded = Get(j, "supplies_awarded", std::int64_t{0});
    a.credits_accrued = Get(j, "credits_accrued", std::int64_t{0});
    a.cheat_death_used = Get(j, "cheat_death_used", 0);
    a.completed = Get(j, "completed", false);
    return a;
}

json ResultToJson(const ArenaResult& r) {
    return {{"arena", r.arena},
            {"route_id", r.route_id},
            {"time_seconds", Fin(r.time_seconds)},
            {"kills", r.kills},
            {"damage_taken", Fin(r.damage_taken)},
            {"no_damage", r.no_damage},
            {"deaths", r.deaths},
            {"rating", static_cast<int>(r.rating)},
            {"supplies_awarded", r.supplies_awarded},
            {"credits_accrued", r.credits_accrued},
            {"arena_name", r.arena_name},
            {"boss", r.boss}};
}

ArenaResult ResultFromJson(const json& j) {
    ArenaResult r;
    r.arena = Get(j, "arena", 0);
    r.route_id = Get(j, "route_id", std::string());
    r.time_seconds = Get(j, "time_seconds", 0.0);
    r.kills = Get(j, "kills", 0);
    r.damage_taken = Get(j, "damage_taken", 0.0);
    r.no_damage = Get(j, "no_damage", true);
    r.deaths = Get(j, "deaths", 0);
    r.rating = static_cast<Rating>(std::clamp(Get(j, "rating", 3), 0, 3));
    r.supplies_awarded = Get(j, "supplies_awarded", std::int64_t{0});
    r.credits_accrued = Get(j, "credits_accrued", std::int64_t{0});
    r.arena_name = Get(j, "arena_name", std::string());
    r.boss = Get(j, "boss", false);
    return r;
}

}  // namespace

std::string SerializeRunResume(const RunState& run) {
    json j;
    j["version"] = kRunResumeVersion;
    j["run_id"] = run.run_id;
    j["seed"] = run.seed;
    j["character_id"] = run.character_id;
    j["skin_id"] = run.skin_id;
    j["length_id"] = run.length_id;
    json items = json::array();
    for (const LoadoutItem& i : run.loadout.items) {
        items.push_back({{"id", i.id}, {"name", i.name}, {"count", i.count}, {"game_item", i.game_item}});
    }
    j["loadout"] = {{"weapon_id", run.loadout.weapon_id}, {"items", std::move(items)}, {"budget", run.loadout.budget},
                    {"spent", run.loadout.spent}};
    json grants = json::array();
    for (const ItemGrant& g : run.item_grants) grants.push_back({{"game_item", g.game_item}, {"count", g.count}, {"source", g.source}});
    j["item_grants"] = std::move(grants);
    j["perk_kill_counts"] = MapToJson(run.perk_kill_counts);
    j["chests_opened"] = run.chests_opened;
    j["keys"] = run.keys;
    j["stats"] = {{"damage", Fin(run.stats.damage)},         {"defense", Fin(run.stats.defense)}, {"max_health", Fin(run.stats.max_health)},
                  {"crit", Fin(run.stats.crit)},             {"move_speed", Fin(run.stats.move_speed)}, {"heal", Fin(run.stats.heal)},
                  {"supplies", Fin(run.stats.supplies)},     {"gains", run.stats.gains}};
    json specials = json::array();
    for (const RunState::SpecialUse& s : run.specials) specials.push_back({{"used_arena", s.used_arena}, {"uses", s.uses}});
    j["specials"] = std::move(specials);
    j["arena_index"] = run.arena_index;
    j["arena_count"] = run.arena_count;
    j["route_id"] = run.route_id;
    j["map_layer"] = run.map_layer;
    j["map_node"] = run.map_node;
    json layers = json::array();
    for (const auto& layer : run.run_map.layers) {
        json nodes = json::array();
        for (const RunMapNode& n : layer) {
            nodes.push_back({{"map_key", n.map_key}, {"route_id", n.route_id}, {"next", n.next}, {"guests", n.guests}});
        }
        layers.push_back(std::move(nodes));
    }
    j["run_map"] = std::move(layers);
    json txs = json::object();
    for (const auto& [id, amount] : run.supplies.transactions()) txs[id] = amount;
    j["supplies"] = {{"balance", run.supplies.balance()}, {"transactions", std::move(txs)}};
    j["supplies_earned"] = run.supplies_earned;
    j["supplies_spent"] = run.supplies_spent;
    j["credits_pending"] = run.credits_pending;
    j["perks"] = MapToJson(run.perks);
    json weapons = json::array();
    for (const WeaponInstance& w : run.weapons) weapons.push_back(WeaponToJson(w));
    j["weapons"] = std::move(weapons);
    j["next_weapon_serial"] = run.next_weapon_serial;
    j["reroll_tokens"] = run.reroll_tokens;
    j["cheat_death_used_total"] = run.cheat_death_used_total;
    json arenas = json::array();
    for (const ArenaMetrics& a : run.arenas) arenas.push_back(MetricsToJson(a));
    j["arenas"] = std::move(arenas);
    json results = json::array();
    for (const ArenaResult& r : run.results) results.push_back(ResultToJson(r));
    j["results"] = std::move(results);
    if (run.player_hp) j["player_hp"] = Fin(*run.player_hp);
    if (run.player_hp_max) j["player_hp_max"] = Fin(*run.player_hp_max);
    return j.dump();
}

Status ParseRunResume(std::string_view text, RunState& out) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return Status::Error(ErrorCode::ParseError, "resume point is not a JSON object");
    if (Get(j, "version", 0) != kRunResumeVersion) {
        return Status::Error(ErrorCode::ParseError, "resume point version " + std::to_string(Get(j, "version", 0)) + " is not " +
                                                        std::to_string(kRunResumeVersion));
    }
    try {
        RunState r;
        r.run_id = j.at("run_id").get<std::string>();
        r.seed = j.at("seed").get<std::uint64_t>();
        r.character_id = j.at("character_id").get<std::string>();
        r.skin_id = Get(j, "skin_id", std::string());
        r.length_id = Get(j, "length_id", std::string());
        const json& lo = Get(j, "loadout", json::object());
        r.loadout.weapon_id = Get(lo, "weapon_id", std::string());
        r.loadout.budget = Get(lo, "budget", std::int64_t{0});
        r.loadout.spent = Get(lo, "spent", std::int64_t{0});
        for (const json& i : Get(lo, "items", json::array())) {
            r.loadout.items.push_back(LoadoutItem{Get(i, "id", std::string()), Get(i, "name", std::string()), Get(i, "count", 1),
                                                  Get(i, "game_item", std::int64_t{-1})});
        }
        for (const json& g : Get(j, "item_grants", json::array())) {
            r.item_grants.push_back(ItemGrant{Get(g, "game_item", std::int64_t{-1}), Get(g, "count", 1), Get(g, "source", std::string())});
        }
        MapFromJson(Get(j, "perk_kill_counts", json::object()), r.perk_kill_counts);
        r.chests_opened = Get(j, "chests_opened", 0);
        r.keys = Get(j, "keys", 0);
        const json& st = Get(j, "stats", json::object());
        r.stats.damage = Get(st, "damage", 0.0);
        r.stats.defense = Get(st, "defense", 0.0);
        r.stats.max_health = Get(st, "max_health", 0.0);
        r.stats.crit = Get(st, "crit", 0.0);
        r.stats.move_speed = Get(st, "move_speed", 0.0);
        r.stats.heal = Get(st, "heal", 0.0);
        r.stats.supplies = Get(st, "supplies", 0.0);
        r.stats.gains = Get(st, "gains", 0);
        const json& sp = Get(j, "specials", json::array());
        for (std::size_t i = 0; i < r.specials.size() && i < sp.size(); ++i) {
            r.specials[i].used_arena = Get(sp[i], "used_arena", 0);
            r.specials[i].uses = Get(sp[i], "uses", 0);
        }
        r.arena_index = Get(j, "arena_index", 0);
        r.arena_count = Get(j, "arena_count", 3);
        r.route_id = Get(j, "route_id", std::string());
        r.map_layer = Get(j, "map_layer", 0);
        r.map_node = Get(j, "map_node", 0);
        for (const json& layer : Get(j, "run_map", json::array())) {
            std::vector<RunMapNode> nodes;
            for (const json& n : layer) {
                RunMapNode node;
                node.map_key = Get(n, "map_key", -1);
                node.route_id = Get(n, "route_id", std::string());
                node.next = Get(n, "next", std::vector<int>{});
                node.guests = Get(n, "guests", std::vector<std::string>{});
                nodes.push_back(std::move(node));
            }
            r.run_map.layers.push_back(std::move(nodes));
        }
        const json& sup = Get(j, "supplies", json::object());
        StringMap<std::int64_t> txs;
        const json transactions = Get(sup, "transactions", json::object());  // kept: items() of a temporary dangles
        for (const auto& [id, amount] : transactions.items()) {
            if (amount.is_number_integer()) txs[id] = amount.get<std::int64_t>();
        }
        r.supplies = Ledger::Restore(std::max<std::int64_t>(0, Get(sup, "balance", std::int64_t{0})), std::move(txs));
        r.supplies_earned = Get(j, "supplies_earned", std::int64_t{0});
        r.supplies_spent = Get(j, "supplies_spent", std::int64_t{0});
        r.credits_pending = Get(j, "credits_pending", std::int64_t{0});
        MapFromJson(Get(j, "perks", json::object()), r.perks);
        for (const json& w : Get(j, "weapons", json::array())) r.weapons.push_back(WeaponFromJson(w));
        r.next_weapon_serial = Get(j, "next_weapon_serial", 1);
        r.reroll_tokens = Get(j, "reroll_tokens", 0);
        r.cheat_death_used_total = Get(j, "cheat_death_used_total", 0);
        for (const json& a : Get(j, "arenas", json::array())) r.arenas.push_back(MetricsFromJson(a));
        for (const json& res : Get(j, "results", json::array())) r.results.push_back(ResultFromJson(res));
        if (j.contains("player_hp") && j.at("player_hp").is_number()) r.player_hp = j.at("player_hp").get<double>();
        if (j.contains("player_hp_max") && j.at("player_hp_max").is_number()) r.player_hp_max = j.at("player_hp_max").get<double>();
        out = std::move(r);
    } catch (const json::exception& e) {
        return Status::Error(ErrorCode::ParseError, std::string("resume point: ") + e.what());
    }
    return Status::Success();
}

}  // namespace rm
