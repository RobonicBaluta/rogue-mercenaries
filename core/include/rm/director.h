#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rm/common.h"
#include "rm/rng.h"

namespace rm {

/// One enemy type a map can host (its model is loaded there). `arms` are the descriptor
/// weapon ids seen on that map (empty = the descriptor default).
struct MapEnemy {
    std::string unit_class;          // engine class, e.g. "uEm10"
    std::vector<std::int64_t> arms;  // descriptor weapon ids
    int min_tier = 1;                // first route tier that may use it
    double weight = 1.0;
};

/// A map the director can run an arena on (configs/maps.json).
struct MapDef {
    std::string id;
    std::string name;
    int stage = -1;  // engine stage index (Mercenaries stage for now)
    std::vector<std::array<double, 3>> points;  // spawn positions known to be on walkable ground
    std::vector<MapEnemy> roster;
    int max_alive = 8;
    double min_player_distance = 800.0;  // engine units; points closer than this are avoided
};

/// What the adapter must create now. `ticket` identifies the request until it is answered.
struct SpawnRequest {
    std::uint64_t ticket = 0;
    std::string unit_class;
    std::int64_t arm = -1;
    std::array<double, 3> pos{};
};

/// Runs one arena: `budget` enemies in total, at most `max_alive` at once, refilled as they
/// die. Only units it spawned count. Deterministic for a given seed.
class Director {
public:
    Director(const MapDef& map, int budget, int tier, std::uint64_t seed);

    /// Requests to spawn now. `player` (when known) steers spawns away from the player.
    std::vector<SpawnRequest> Tick(double now, const std::optional<std::array<double, 3>>& player);
    /// The adapter created the request's unit; `unit_id` is how its death will be reported.
    void Spawned(std::uint64_t ticket, const std::string& unit_id);
    /// The adapter could not create it; the slot goes back to the budget (the point is
    /// avoided after repeated failures).
    void SpawnFailed(std::uint64_t ticket);
    /// A unit died or vanished. True (once) when it was one of ours.
    bool Died(const std::string& unit_id);

    [[nodiscard]] int budget() const { return budget_; }
    [[nodiscard]] int dead() const { return dead_; }
    [[nodiscard]] int alive() const { return static_cast<int>(alive_.size()); }
    [[nodiscard]] int pending() const { return static_cast<int>(pending_.size()); }
    [[nodiscard]] int remaining() const { return budget_ - dead_; }
    [[nodiscard]] bool complete() const { return dead_ >= budget_; }
    [[nodiscard]] const MapDef& map() const { return map_; }

private:
    const MapEnemy* PickEnemy();
    int PickPoint(const std::optional<std::array<double, 3>>& player);

    MapDef map_;
    int budget_ = 0;
    int tier_ = 1;
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
};

}  // namespace rm
