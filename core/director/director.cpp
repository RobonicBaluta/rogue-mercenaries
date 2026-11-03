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

int ThreatTable::Weapon(std::int64_t arm) const {
    if (arm < 0) return none;
    if ((arm & ~std::int64_t{0xFF}) != 0x70100) return fallback;
    const int nn = static_cast<int>(arm & 0xFF);
    if (nn == 0) return none;
    const auto it = weapons.find(nn);
    return it != weapons.end() ? it->second : fallback;
}

int ThreatTable::Class(const std::string& unit_class) const {
    const auto it = classes.find(unit_class);
    return it != classes.end() ? it->second : fallback;
}

int ThreatTable::Of(const MapEnemy& enemy, std::int64_t arm) const { return std::max(Class(enemy.unit_class), Weapon(arm)); }

std::vector<std::pair<int, double>> ThreatTable::Variants(const MapEnemy& enemy) const {
    std::vector<std::pair<int, double>> out;
    if (!enemy.profiles.empty()) {
        for (const EnemyProfile& p : enemy.profiles) out.emplace_back(Of(enemy, ProfileArm(p)), std::max(0.0, p.weight));
    } else if (!enemy.arms.empty()) {
        for (const std::int64_t arm : enemy.arms) out.emplace_back(Of(enemy, arm), 1.0);
    } else {
        out.emplace_back(Of(enemy, -1), 1.0);
    }
    return out;
}

int ThreatTable::Lowest(const MapEnemy& enemy) const {
    int lowest = 0;
    for (const auto& [threat, weight] : Variants(enemy)) {
        if (weight > 0.0 && (lowest == 0 || threat < lowest)) lowest = threat;
    }
    return lowest;
}

int ThreatTable::Lowest(const MapDef& map) const {
    if (map.boss) return 0;
    int lowest = 0, lowestAny = 0;
    for (const MapEnemy& e : map.roster) {
        if (e.weight <= 0.0) continue;
        const int t = Lowest(e);
        if (t <= 0) continue;
        if (lowestAny == 0 || t < lowestAny) lowestAny = t;
        if (!e.special && (lowest == 0 || t < lowest)) lowest = t;
    }
    return lowest > 0 ? lowest : lowestAny;  // a roster of specials only: they are its normal enemies
}

std::int64_t ProfileArm(const EnemyProfile& profile) {
    for (const auto& [name, value] : profile.fields) {
        if (name == "arm") return static_cast<std::int64_t>(value);
    }
    return -1;
}

Director::Director(const MapDef& map, int budget, int tier, std::uint64_t seed, ArenaThreat threat)
    : map_(map), budget_(std::max(0, budget)), tier_(std::max(1, tier)), threat_(std::move(threat)), rng_(seed),
      point_failures_(map.points.size(), 0) {
    if (map_.boss) {
        // The boss alone: the roster becomes it, and its death is the whole budget.
        MapEnemy boss;
        boss.unit_class = map_.boss->unit_class;
        map_.roster = {boss};
        budget_ = 1;
        threat_.level = 0;
    }
    if (threat_.level > 0) pick_level_ = std::max(threat_.level, threat_.table.Lowest(map_));
}

double Director::Share(const MapEnemy& enemy) const {
    if (pick_level_ <= 0) return 1.0;
    double total = 0.0, allowed = 0.0;
    for (const auto& [threat, weight] : threat_.table.Variants(enemy)) {
        total += weight;
        if (Fits(threat)) allowed += weight;
    }
    return total > 0.0 ? allowed / total : 0.0;
}

const MapEnemy* Director::PickEnemy() {
    std::vector<const MapEnemy*> pool;
    std::vector<double> weights;
    // Normal enemies at this tier; failing that, any tier; a roster of specials only, as normal.
    constexpr std::pair<bool, bool> kPasses[] = {{true, false}, {false, false}, {true, true}, {false, true}};
    for (const auto& [tierGate, withSpecial] : kPasses) {
        for (const MapEnemy& e : map_.roster) {
            if ((tierGate && e.min_tier > tier_) || e.weight <= 0.0 || (e.special && !withSpecial)) continue;
            const double share = Share(e);
            if (share <= 0.0) continue;
            pool.push_back(&e);
            weights.push_back(e.weight * share);
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
    // With a threat level: none before specials_from, one more alive per level above it.
    const bool byThreat = threat_.level > 0;
    if (byThreat && threat_.level < threat_.specials_from) return nullptr;
    int specials = 0;
    for (const auto& [ticket, e] : ticket_enemy_) specials += e->special ? 1 : 0;
    const int extra = byThreat ? threat_.level - threat_.specials_from : tier_ - 1;
    if (specials >= std::max(1, map_.max_specials) + extra) return nullptr;
    std::vector<const MapEnemy*> pool;
    std::vector<double> weights;
    for (const MapEnemy& e : map_.roster) {
        if (!e.special || e.min_tier > tier_ || Active(&e) >= std::max(1, e.max_alive)) continue;
        if (byThreat && threat_.table.Lowest(e) > threat_.level) continue;
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
    int cap = std::max(1, map_.max_alive);
    if (threat_.max_alive > 0) cap = std::min(cap, threat_.max_alive);
    while (requested_ < budget_ && alive() + pending() < cap && now >= next_spawn_time_) {
        const MapEnemy* enemy = PickSpecial();
        if (enemy == nullptr) enemy = PickEnemy();
        const int point = PickPoint(player);
        if (enemy == nullptr || point < 0) break;
        SpawnRequest r;
        r.ticket = next_ticket_++;
        r.unit_class = enemy->unit_class;
        // The weapon and profile: only variants within the arena's threat (all of them when none fits).
        if (!enemy->arms.empty()) {
            std::vector<std::int64_t> arms;
            for (const std::int64_t arm : enemy->arms) {
                if (Fits(threat_.table.Of(*enemy, arm))) arms.push_back(arm);
            }
            if (arms.empty()) arms = enemy->arms;
            r.arm = arms[rng_.Index(arms.size())];
        } else {
            r.arm = -1;
        }
        if (!enemy->profiles.empty()) {
            std::vector<double> weights;
            bool any = false;
            for (const EnemyProfile& p : enemy->profiles) {
                const bool fits = Fits(threat_.table.Of(*enemy, ProfileArm(p)));
                weights.push_back(fits ? std::max(0.0, p.weight) : 0.0);
                any = any || (fits && p.weight > 0.0);
            }
            if (!any) {
                weights.clear();
                for (const EnemyProfile& p : enemy->profiles) weights.push_back(std::max(0.0, p.weight));
            }
            const std::size_t i = rng_.WeightedIndex(weights);
            r.fields = enemy->profiles[i < enemy->profiles.size() ? i : 0].fields;
            for (const auto& [name, value] : r.fields) {
                if (name == "arm") r.arm = static_cast<std::int64_t>(value);
            }
        }
        r.pos = map_.points[static_cast<std::size_t>(point)];
        if (map_.boss) {
            r.boss = true;
            r.hp = map_.boss->hp;
        }
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
