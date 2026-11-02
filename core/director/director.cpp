#include "rm/director.h"

#include <algorithm>
#include <cmath>

namespace rm {
namespace {

constexpr double kSpawnInterval = 0.6;   // seconds between two spawns of a refill
constexpr int kMaxPointFailures = 3;     // a point that failed this often is skipped

double DistanceSq(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    const double dx = a[0] - b[0];
    const double dy = a[1] - b[1];
    const double dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

}  // namespace

Director::Director(const MapDef& map, int budget, int tier, std::uint64_t seed)
    : map_(map), budget_(std::max(0, budget)), tier_(std::max(1, tier)), rng_(seed), point_failures_(map.points.size(), 0) {}

const MapEnemy* Director::PickEnemy() {
    std::vector<const MapEnemy*> pool;
    std::vector<double> weights;
    // Normal enemies at this tier; failing that, any tier; a roster of specials only, as normal.
    constexpr std::pair<bool, bool> kPasses[] = {{true, false}, {false, false}, {true, true}, {false, true}};
    for (const auto& [tierGate, withSpecial] : kPasses) {
        for (const MapEnemy& e : map_.roster) {
            if ((tierGate && e.min_tier > tier_) || e.weight <= 0.0 || (e.special && !withSpecial)) continue;
            pool.push_back(&e);
            weights.push_back(e.weight);
        }
        if (!pool.empty()) break;
    }
    if (pool.empty()) return nullptr;
    const std::size_t i = rng_.WeightedIndex(weights);
    return i < pool.size() ? pool[i] : pool.front();
}

int Director::Active(const MapEnemy* enemy) const {
    int n = 0;
    for (const auto& [ticket, e] : ticket_enemy_) n += e == enemy ? 1 : 0;
    return n;
}

const MapEnemy* Director::PickSpecial() {
    if (static_cast<double>(dead_) < map_.special_after * static_cast<double>(budget_)) return nullptr;
    int specials = 0;
    for (const auto& [ticket, e] : ticket_enemy_) specials += e->special ? 1 : 0;
    if (specials >= std::max(1, map_.max_specials) + tier_ - 1) return nullptr;
    std::vector<const MapEnemy*> pool;
    std::vector<double> weights;
    for (const MapEnemy& e : map_.roster) {
        if (!e.special || e.min_tier > tier_ || Active(&e) >= std::max(1, e.max_alive)) continue;
        pool.push_back(&e);
        weights.push_back(std::max(e.weight, 0.001));
    }
    if (pool.empty()) return nullptr;
    const std::size_t i = rng_.WeightedIndex(weights);
    return i < pool.size() ? pool[i] : pool.front();
}

const std::vector<std::array<double, 3>>& Director::PickChests(int count) {
    std::vector<std::array<double, 3>> pool = map_.chests;
    rng_.Shuffle(pool);
    pool.resize(std::min(pool.size(), static_cast<std::size_t>(std::max(0, count))));
    chests_ = std::move(pool);
    return chests_;
}

int Director::PickPoint(const std::optional<std::array<double, 3>>& player) {
    std::vector<int> usable;
    std::vector<int> far;      // neither too close nor too far, not the last point used
    std::vector<int> inReach;  // neither too close nor too far
    std::vector<int> apart;    // not too close
    for (int i = 0; i < static_cast<int>(map_.points.size()); ++i) {
        if (point_failures_[static_cast<std::size_t>(i)] >= kMaxPointFailures) continue;
        usable.push_back(i);
        const double d = player ? DistanceSq(map_.points[static_cast<std::size_t>(i)], *player) : 0.0;
        const bool tooClose = player && d < map_.min_player_distance * map_.min_player_distance;
        const bool tooFar = player && map_.max_player_distance > 0.0 && d > map_.max_player_distance * map_.max_player_distance;
        if (tooClose) continue;
        apart.push_back(i);
        if (tooFar) continue;
        inReach.push_back(i);
        if (i != last_point_) far.push_back(i);
    }
    if (usable.empty() && !map_.points.empty()) {
        // Every point failed often: forget the failures and keep trying (slowly, see SpawnFailed).
        std::fill(point_failures_.begin(), point_failures_.end(), 0);
        return PickPoint(player);
    }
    const std::vector<int>& from = !far.empty() ? far : !inReach.empty() ? inReach : !apart.empty() ? apart : usable;
    if (from.empty()) return -1;
    return from[rng_.Index(from.size())];
}

std::vector<SpawnRequest> Director::Tick(double now, const std::optional<std::array<double, 3>>& player) {
    std::vector<SpawnRequest> out;
    last_now_ = now;
    const int cap = std::max(1, map_.max_alive);
    while (requested_ < budget_ && alive() + pending() < cap && now >= next_spawn_time_) {
        const MapEnemy* enemy = PickSpecial();
        if (enemy == nullptr) enemy = PickEnemy();
        const int point = PickPoint(player);
        if (enemy == nullptr || point < 0) break;
        SpawnRequest r;
        r.ticket = next_ticket_++;
        r.unit_class = enemy->unit_class;
        r.arm = enemy->arms.empty() ? -1 : enemy->arms[rng_.Index(enemy->arms.size())];
        if (!enemy->profiles.empty()) {
            std::vector<double> weights;
            for (const EnemyProfile& p : enemy->profiles) weights.push_back(std::max(0.0, p.weight));
            const std::size_t i = rng_.WeightedIndex(weights);
            r.fields = enemy->profiles[i < enemy->profiles.size() ? i : 0].fields;
            for (const auto& [name, value] : r.fields) {
                if (name == "arm") r.arm = static_cast<std::int64_t>(value);
            }
        }
        r.pos = map_.points[static_cast<std::size_t>(point)];
        pending_.emplace_back(r.ticket, point);
        ticket_enemy_[r.ticket] = enemy;
        last_point_ = point;
        ++requested_;
        out.push_back(std::move(r));
        // The first fill comes at once; refills are spaced out.
        if (requested_ > cap) next_spawn_time_ = now + kSpawnInterval;
    }
    return out;
}

void Director::Spawned(std::uint64_t ticket, const std::string& unit_id) {
    const auto it = std::find_if(pending_.begin(), pending_.end(), [ticket](const auto& p) { return p.first == ticket; });
    if (it == pending_.end()) return;
    pending_.erase(it);
    alive_[unit_id] = ticket;
}

void Director::SpawnFailed(std::uint64_t ticket) {
    const auto it = std::find_if(pending_.begin(), pending_.end(), [ticket](const auto& p) { return p.first == ticket; });
    if (it == pending_.end()) return;
    if (it->second >= 0 && it->second < static_cast<int>(point_failures_.size())) ++point_failures_[static_cast<std::size_t>(it->second)];
    pending_.erase(it);
    ticket_enemy_.erase(ticket);
    --requested_;
    // No retry in the same frame: a refused spawn waits like a refill.
    next_spawn_time_ = std::max(next_spawn_time_, last_now_ + kSpawnInterval);
}

bool Director::Died(const std::string& unit_id) {
    const auto it = alive_.find(unit_id);
    if (it == alive_.end()) return false;
    ticket_enemy_.erase(it->second);
    alive_.erase(it);
    ++dead_;
    return true;
}

}  // namespace rm
