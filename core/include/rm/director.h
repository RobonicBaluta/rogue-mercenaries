#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "rm/common.h"
#include "rm/rng.h"

namespace rm {

/// Named spawn settings the adapter writes into the enemy's descriptor (weapon, plaga type,
/// mask, ...). Names and meaning belong to the adapter; the core only picks one.
using SpawnFields = std::vector<std::pair<std::string, double>>;

/// One way the game itself spawned this enemy type (surveyed), picked by weight.
struct EnemyProfile {
    double weight = 1.0;
    SpawnFields fields;
};

/// One enemy type a map can host (its model is loaded there). `profiles` are the variants
/// the game used there; without them, `arms` are the weapon ids seen (empty = the default).
struct MapEnemy {
    std::string unit_class;          // engine class, e.g. "uEm10"
    std::vector<std::int64_t> arms;  // descriptor weapon ids
    std::vector<EnemyProfile> profiles;
    int min_tier = 1;                // first route tier that may use it
    double weight = 1.0;
    /// The stage's special enemy (the big ones the game itself spawns one at a time): never a
    /// normal pick; once `MapDef::special_after` of the budget is dead, kept at `max_alive`
    /// (default 1) until the budget runs out. Its kills count like any other.
    bool special = false;
    int max_alive = 0;
};

/// A boss (ROADMAP M6): on its map the director spawns only the boss, and the arena clears when
/// it dies. Runs end on a boss arena when one is in their pool.
struct MapBoss {
    std::string unit_class;  // engine class, e.g. "uEm40"
    std::string name;        // shown in the route menu and on the HUD
    int hp = 0;              // > 0: its health once spawned (the game's own suits story weapons)
};

/// A map the director can run an arena on (configs/maps.json).
struct MapDef {
    std::string id;
    std::string name;
    /// Mercenaries maps: the stage index 0..7; campaign areas: their stage number (100 and up),
    /// which the adapter's host loads in place of a Mercenaries stage.
    int stage = -1;
    bool rotation = true;  // runs may pick it (false keeps it for tests)
    /// Where the player starts (campaign areas: the game picks one of the area's entrances at
    /// random, so the adapter moves the player here when an arena begins far from it).
    std::optional<std::array<double, 3>> start;
    std::vector<std::array<double, 3>> points;  // spawn positions known to be on walkable ground
    std::vector<std::array<double, 3>> chests;  // chest positions (where the game puts its time bonuses)
    std::vector<MapEnemy> roster;
    /// Archives (Image\Archive\<name>) the adapter loads with the arena's enemies: everything the
    /// roster's own stage loads for them (weapons they carry, plagas, effects), for maps whose
    /// stage does not load them itself (campaign areas).
    std::vector<std::string> preload;
    int max_alive = 8;
    double min_player_distance = 800.0;  // engine units; points closer than this are avoided
    double max_player_distance = 0.0;    // > 0: points farther than this are avoided (large areas)
    double special_after = 0.25;         // share of the budget dead before specials join
    int max_specials = 1;                // specials alive at once on tier 1 (+1 per tier above)
    std::optional<MapBoss> boss;         // a boss arena (the roster is not used)
};

/// How dangerous an enemy is (ROADMAP M3.4): 1 a Majini with a blade or bare hands, 2 big blades,
/// fire, bows and handguns, 3 explosives, stun rods, shotguns and the big specials, 4 machine
/// guns, rockets, gatlings and the chainsaw. A spawn's threat is the higher of its class's and
/// its weapon's (economy.json enemy_threat, written by scripts/write_threats.py).
struct ThreatTable {
    int none = 1;      // no weapon (no arm, or the arm 0x70100)
    int fallback = 2;  // a class or weapon not listed
    std::unordered_map<int, int> weapons;  // by weapon class: the arm 0x701NN's NN
    StringMap<int> classes;

    [[nodiscard]] int Weapon(std::int64_t arm) const;
    [[nodiscard]] int Class(const std::string& unit_class) const;
    [[nodiscard]] int Of(const MapEnemy& enemy, std::int64_t arm) const;
    /// Each variant of an entry with its weight: its profiles, else its arms, else the class alone.
    [[nodiscard]] std::vector<std::pair<int, double>> Variants(const MapEnemy& enemy) const;
    [[nodiscard]] int Lowest(const MapEnemy& enemy) const;
    /// The mildest enemy a map's normal (not special) roster can field; 0 for a boss map.
    [[nodiscard]] int Lowest(const MapDef& map) const;
};

/// The weapon a profile gives (its "arm" field), -1 without one.
std::int64_t ProfileArm(const EnemyProfile& profile);

/// What an arena may field (the run builds it from economy enemy_threat).
struct ArenaThreat {
    int level = 0;          // 0: no limit (every enemy, the map's max_alive, specials by tier)
    int max_alive = 0;      // > 0: at most this many alive at once (within the map's own)
    int specials_from = 1;  // the stage's specials join from this level on
    ThreatTable table;
};

/// What the adapter must create now. `ticket` identifies the request until it is answered.
struct SpawnRequest {
    std::uint64_t ticket = 0;
    std::string unit_class;
    std::int64_t arm = -1;
    std::array<double, 3> pos{};
    SpawnFields fields;  // from the picked profile (empty = adapter defaults)
    int hp = 0;          // > 0: the unit's health once spawned (bosses)
    bool boss = false;
};

/// Runs one arena: `budget` enemies in total, at most `max_alive` at once, refilled as they
/// die. Only units it spawned count. Deterministic for a given seed. On a boss map the boss is
/// the only unit (budget 1).
class Director {
public:
    /// `threat` (level > 0) holds the arena to enemies at or under its level: entries and
    /// variants above it are left out (a map with nothing that mild fields its mildest), at most
    /// threat.max_alive are alive at once, and specials join only from threat.specials_from.
    Director(const MapDef& map, int budget, int tier, std::uint64_t seed, ArenaThreat threat = {});

    /// Requests to spawn now. `player` (when known) steers spawns away from the player.
    std::vector<SpawnRequest> Tick(double now, const std::optional<std::array<double, 3>>& player);
    /// The adapter created the request's unit; `unit_id` is how its death will be reported.
    void Spawned(std::uint64_t ticket, const std::string& unit_id);
    /// The adapter could not create it; the slot goes back to the budget (the point is
    /// avoided after repeated failures).
    void SpawnFailed(std::uint64_t ticket);
    /// A unit died or vanished. True (once) when it was one of ours.
    bool Died(const std::string& unit_id);
    /// Picks `count` distinct chest positions of the map for this arena (all of them when the
    /// map has fewer); they stay available as chests().
    const std::vector<std::array<double, 3>>& PickChests(int count);
    [[nodiscard]] const std::vector<std::array<double, 3>>& chests() const { return chests_; }

    [[nodiscard]] int budget() const { return budget_; }
    [[nodiscard]] int dead() const { return dead_; }
    [[nodiscard]] int alive() const { return static_cast<int>(alive_.size()); }
    [[nodiscard]] int pending() const { return static_cast<int>(pending_.size()); }
    [[nodiscard]] int remaining() const { return budget_ - dead_; }
    [[nodiscard]] bool complete() const { return dead_ >= budget_; }
    [[nodiscard]] const MapDef& map() const { return map_; }
    /// The arena's threat level (0: none), and the level spawns are held to (the map's mildest
    /// when that is above the arena's).
    [[nodiscard]] int threat_level() const { return threat_.level; }
    [[nodiscard]] int pick_level() const { return pick_level_; }

private:
    const MapEnemy* PickEnemy();
    const MapEnemy* PickSpecial();
    [[nodiscard]] double Share(const MapEnemy& enemy) const;  // the part of an entry this arena may field
    [[nodiscard]] bool Fits(int threat) const { return pick_level_ <= 0 || threat <= pick_level_; }
    [[nodiscard]] int Active(const MapEnemy* enemy) const;  // alive + pending of that roster entry
    int PickPoint(const std::optional<std::array<double, 3>>& player);

    MapDef map_;
    int budget_ = 0;
    int tier_ = 1;
    ArenaThreat threat_;
    int pick_level_ = 0;
    Rng rng_;
    std::uint64_t next_ticket_ = 1;
    int requested_ = 0;  // spawns asked for and not failed (alive + pending + dead)
    int dead_ = 0;
    double next_spawn_time_ = 0.0;
    int last_point_ = -1;
    double last_now_ = 0.0;
    StringMap<std::uint64_t> alive_;                 // unit id -> ticket
    std::vector<std::pair<std::uint64_t, int>> pending_;  // ticket -> point index
    std::vector<int> point_failures_;
    std::unordered_map<std::uint64_t, const MapEnemy*> ticket_enemy_;  // pending and alive tickets
    std::vector<std::array<double, 3>> chests_;
};

}  // namespace rm
