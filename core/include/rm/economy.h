#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rm/common.h"
#include "rm/director.h"
#include "rm/rarity.h"

namespace rm {

/// Arena performance grade.
enum class Rating { S = 0, A = 1, B = 2, C = 3 };
inline constexpr std::size_t kRatingCount = 4;
std::string_view RatingName(Rating r);
std::optional<Rating> ParseRating(std::string_view name);
constexpr std::size_t RatingIndex(Rating r) { return static_cast<std::size_t>(r); }

/// Tunables from configs/economy.json. Defaults are only placeholders; the loader requires
/// every section so balance changes are always explicit in data.
struct EconomyConfig {
    struct Run {
        int arena_count = 3;
        std::string starting_route;
        std::int64_t starting_supplies = 0;
        /// When false (default) only kills attributed to the player reduce the enemy budget; when
        /// true every enemy death counts and pays out (solo play: explosions, hazards).
        bool non_player_kills_count_toward_budget = false;
        int max_reroll_tokens = 2;
    } run;

    struct Rewards {
        int choices = 3;
        RarityWeights rarity_weights{};
        /// Added to the route rarity_bonus when generating the post-arena offer.
        std::array<double, kRatingCount> rating_rarity_bonus{};
        double perk_weight = 70.0;
        double weapon_weight = 20.0;
        double supplies_weight = 10.0;
        double item_weight = 0.0;  // inventory items from loadouts.json (needs the inventory capability)
        std::int64_t supplies_bundle = 30;
    } rewards;

    struct WeaponRarityRule {
        int affix_count = 0;
        double magnitude_mult = 1.0;
        int unique_count = 0;
        int element_count = 0;
    };
    std::array<WeaponRarityRule, kRarityCount> weapon_rarity{};

    /// Refining a gun one tier up (rm::RefineWeapon): a reward choice now and then (always after
    /// an arena rated `reward_rating` or better), a shop offer now and then, and sometimes a
    /// chest's (on the gun in hand). Legendary is the top until `exotic_unlocked_by` (a
    /// ProfileStats::bosses_beaten key) is beaten. Prices by the tier reached.
    struct Refine {
        bool enabled = false;
        double reward_chance = 0.0;
        std::string reward_rating = "S";
        double shop_chance = 0.0;
        double chest_chance = 0.0;
        std::array<std::int64_t, kRarityCount> price{};
        std::string exotic_unlocked_by;
    } refine;

    struct Upgrades {
        int max_level = 5;
        /// cost_to_next[L-1] = supplies to go from level L to L+1 (size max_level-1).
        std::vector<std::int64_t> cost_to_next;
        /// Percent damage per level above 1.
        double damage_pct_per_level = 8.0;
    } upgrades;

    struct Supplies {
        std::int64_t per_kill = 2;
        std::int64_t per_arena_clear = 25;
    } supplies;

    struct Credits {
        std::int64_t per_kill = 1;
        std::int64_t per_arena_clear = 20;
        std::int64_t run_complete_bonus = 50;
        /// Fraction of pending credits banked when the run fails or is abandoned.
        double death_rate = 0.5;
        std::array<double, kRatingCount> rating_mult{1.5, 1.25, 1.0, 0.75};
    } credits;

    struct RatingRules {
        double s_time_fraction = 1.0;   // time <= par * this scores 2 time points
        double a_time_fraction = 1.5;   // time <= par * this scores 1 time point
        double low_damage_fraction = 0.35;  // damage/hp_max <= this scores 1 damage point (0 dmg = 2)
    } rating;

    struct Damage {
        double outgoing_min = 0.25;
        double outgoing_max = 5.0;
        double incoming_min = 0.25;
        double incoming_max = 4.0;
        double default_crit_multiplier = 1.5;
        double max_crit_chance = 0.75;
    } damage;

    struct Shop {
        double heal_fraction = 0.5;
        std::int64_t heal_price = 25;
        std::int64_t reroll_price = 20;
        /// NEW STOCK: replaces every offer (a fresh weapon upgrade among them) for this many
        /// supplies, `restock_price_step` more for each one already bought in the visit (a build's
        /// free ones come first). 0: no paid restock.
        std::int64_t restock_price = 0;
        std::int64_t restock_price_step = 0;
        std::array<std::int64_t, kRarityCount> perk_price{};
        std::array<std::int64_t, kRarityCount> weapon_price{};
        int perk_offers = 2;
        int item_offers = 0;  // distinct inventory items on sale (price from loadouts.json)
    } shop;

    /// The run map (rm::GenerateRunMap): nodes per layer after the starting arena.
    struct RunMapConfig {
        int width = 3;
    } run_map;

    /// Chests (the adapter opens one for each time-bonus pickup during an arena): supplies
    /// always, plus a catalog item (item_chance) or a weapon the match can place (weapon_chance).
    /// On a director map the arena places `per_arena` of the map's chest positions.
    struct Chests {
        int per_arena = 3;
        std::int64_t supplies_base = 10;
        std::int64_t supplies_per_arena = 5;
        double item_chance = 0.6;
        double weapon_chance = 0.1;
    } chests;

    /// Enemy threat (ROADMAP M3.4, written by scripts/write_threats.py): the first arena is level
    /// 1, later arenas their route's tier plus later_arena_bonus, up to max_level. Disabled: every
    /// enemy of the map, as before.
    struct EnemyThreat {
        bool enabled = false;
        int later_arena_bonus = 1;
        int max_level = 4;
        std::vector<int> max_alive_by_level;  // index level - 1; empty: the map's own max_alive
        int specials_from_level = 3;
        ThreatTable table;

        /// The level of a run's `arena`-th arena (1-based) on a route of `tier`; 0 when disabled.
        [[nodiscard]] int Level(int arena, int tier) const {
            if (!enabled) return 0;
            return std::clamp(tier + (arena > 1 ? later_arena_bonus : 0), 1, std::max(1, max_level));
        }
        [[nodiscard]] ArenaThreat ForArena(int arena, int tier) const {
            ArenaThreat out;
            out.level = Level(arena, tier);
            if (out.level <= 0) return out;
            if (static_cast<std::size_t>(out.level) <= max_alive_by_level.size()) {
                out.max_alive = max_alive_by_level[static_cast<std::size_t>(out.level - 1)];
            }
            out.specials_from = specials_from_level;
            out.table = table;
            return out;
        }
    } enemy_threat;

    /// Run-planned rosters (ROADMAP M3.3, rm::PlanRunRosters): when a run starts, every arena of
    /// its map from `from_arena` on (boss arenas aside) adds enemy classes of other maps to its
    /// map's own, its "guests": up to `normal_guests` normal ones and, where the arena's threat
    /// lets specials in, `special_guests` specials, within `max_classes` classes in all and at or
    /// under the arena's threat level, preferring classes the run has not fielded yet. A guest is
    /// its home map's roster entry (`guests`: class -> home map key; only listed classes travel);
    /// guests take `normal_share` of the normal picks and `special_share` of the special ones.
    /// The adapter loads exactly each arena's classes with its stage. Disabled: every arena
    /// fields its own map's enemies.
    struct RunRosters {
        bool enabled = false;
        int from_arena = 2;
        int normal_guests = 1;
        int special_guests = 1;
        int max_classes = 5;
        double normal_share = 0.35;
        double special_share = 0.5;
        StringMap<int> guests;
    } run_rosters;

    /// Keys (M8): a chest opens only with a key (one used per chest), except for a build with
    /// chest_keys_exempt. Kills of the listed enemy classes (prefixes, e.g. "uEm1" = the Majini)
    /// drop one with drop_chance. Disabled: chests open freely.
    struct Keys {
        bool enabled = false;
        double drop_chance = 0.0;
        std::vector<std::string> classes;
        int start = 0;
        int max = 9;
    } keys;

    /// The run lengths a player climbs (the user's ladder, 2026-11-07). A length first ends on its
    /// easy boss; beating it adds the hard one as a choice; beating the hard one unlocks the length
    /// whose `unlocked_by` names it. The finale is one more arena after its `after` length, once every
    /// key of its `unlocked_by` is beaten. Keys: "<length id>:easy", "<length id>:hard", "finale"
    /// (ProfileStats::bosses_beaten). Arenas from `growth_from_arena` on get harder a step each.
    /// Disabled: runs are run.arena_count long and end on any boss map.
    struct Ladder {
        bool enabled = false;
        struct Length {
            std::string id;
            std::string name;
            int arenas = 4;          // the boss arena included
            int easy_boss = -1;      // boss map keys (maps.json "stage" of a map with a boss)
            // Its alternates ("hard_boss": one map key or a list; empty: none), all offered once the
            // easy boss is beaten; beating any of them is the length's ":hard" (the user's 8: Mkono or
            // Jill, 2026-11-08).
            std::vector<int> hard_bosses;
            [[nodiscard]] bool IsHard(int key) const {
                return std::find(hard_bosses.begin(), hard_bosses.end(), key) != hard_bosses.end();
            }
            std::string unlocked_by;  // a beaten key; empty: open from the start
            double boss_hp_mult = 1.0;
        };
        std::vector<Length> lengths;
        struct Finale {
            int boss = -1;  // -1: none
            std::string after;
            std::vector<std::string> unlocked_by;
            double boss_hp_mult = 1.0;
        } finale;
        int growth_from_arena = 5;
        double incoming_per_arena = 0.0;   // enemies hit harder by this much a step
        double toughness_per_arena = 0.0;  // the player's damage is divided by 1 + this a step
        int budget_per_arena = 0;          // more enemies a step
        [[nodiscard]] const Length* Find(std::string_view id) const {
            for (const Length& l : lengths) {
                if (l.id == id) return &l;
            }
            return nullptr;
        }
        /// Steps of growth at `arena` (1-based): 0 before growth_from_arena.
        [[nodiscard]] int Steps(int arena) const { return enabled && arena >= growth_from_arena ? arena - growth_from_arena + 1 : 0; }
    } ladder;
};

/// Grades an arena: time points (2/1/0 vs par) + damage points (2 no damage, 1 low, 0).
/// 4 = S, 3 = A, 2 = B, otherwise C. Any death forces C.
Rating ComputeRating(const EconomyConfig::RatingRules& rules, double time_seconds,
                     double par_seconds, double damage_taken, double hp_max, int deaths);

/// Supplies to raise a weapon from current_level to current_level+1 (nullopt at max level).
/// An uncapped weapon (a skin build's) keeps levelling: past the cost table each level costs
/// the table's last step plus half of it per level beyond.
std::optional<std::int64_t> UpgradeCost(const EconomyConfig& economy, int current_level, bool uncapped = false);

/// Damage bonus (fraction) contributed by weapon level: (level-1) * damage_pct_per_level / 100.
double LevelDamageBonus(const EconomyConfig& economy, int level);

enum class TxResult { Applied, AlreadyApplied, InvalidAmount, InsufficientFunds, InvalidId };
std::string_view TxResultName(TxResult r);

/// Balance with an idempotency log. Every mutation names a transaction id; re-applying an
/// id is a no-op reporting AlreadyApplied. Negative amounts and overdrafts are rejected, so
/// the balance can never go below zero.
class Ledger {
public:
    Ledger() = default;
    explicit Ledger(std::int64_t opening_balance);

    TxResult Credit(std::string_view tx_id, std::int64_t amount);
    TxResult Debit(std::string_view tx_id, std::int64_t amount);
    /// Reverts an applied transaction (used to roll back a debit when the grant step fails).
    /// Refuses if the id is unknown or reverting would make the balance negative.
    bool Rollback(std::string_view tx_id);

    [[nodiscard]] std::int64_t balance() const { return balance_; }
    [[nodiscard]] bool Contains(std::string_view tx_id) const;
    /// Applied transaction id -> signed delta.
    [[nodiscard]] const StringMap<std::int64_t>& transactions() const { return transactions_; }

    /// Rebuilds a ledger from persisted state (balance must be >= 0).
    static Ledger Restore(std::int64_t balance, StringMap<std::int64_t> transactions);

private:
    std::int64_t balance_ = 0;
    StringMap<std::int64_t> transactions_;
};

}  // namespace rm
