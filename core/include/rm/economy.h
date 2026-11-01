#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rm/common.h"
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
    };
    std::array<WeaponRarityRule, kRarityCount> weapon_rarity{};

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
        std::array<std::int64_t, kRarityCount> perk_price{};
        std::array<std::int64_t, kRarityCount> weapon_price{};
        int perk_offers = 2;
        int item_offers = 0;  // distinct inventory items on sale (price from loadouts.json)
    } shop;

    struct Routes {
        double third_choice_chance = 0.5;
    } routes;

    /// Chests (the adapter opens one for each time-bonus pickup during an arena): supplies
    /// always, plus a catalog item (item_chance) or a weapon the match can place (weapon_chance).
    struct Chests {
        std::int64_t supplies_base = 10;
        std::int64_t supplies_per_arena = 5;
        double item_chance = 0.6;
        double weapon_chance = 0.1;
    } chests;
};

/// Grades an arena: time points (2/1/0 vs par) + damage points (2 no damage, 1 low, 0).
/// 4 = S, 3 = A, 2 = B, otherwise C. Any death forces C.
Rating ComputeRating(const EconomyConfig::RatingRules& rules, double time_seconds,
                     double par_seconds, double damage_taken, double hp_max, int deaths);

/// Supplies to raise a weapon from current_level to current_level+1 (nullopt at max level).
std::optional<std::int64_t> UpgradeCost(const EconomyConfig& economy, int current_level);

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
