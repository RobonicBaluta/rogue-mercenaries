#include <algorithm>
#include <cmath>
#include <limits>

#include "rm/economy.h"

namespace rm {

std::string_view TxResultName(TxResult r) {
    switch (r) {
        case TxResult::Applied: return "applied";
        case TxResult::AlreadyApplied: return "already_applied";
        case TxResult::InvalidAmount: return "invalid_amount";
        case TxResult::InsufficientFunds: return "insufficient_funds";
        case TxResult::InvalidId: return "invalid_id";
    }
    return "invalid_id";
}

Ledger::Ledger(std::int64_t opening_balance) : balance_(std::max<std::int64_t>(0, opening_balance)) {}

bool Ledger::Contains(std::string_view tx_id) const {
    return transactions_.find(tx_id) != transactions_.end();
}

TxResult Ledger::Credit(std::string_view tx_id, std::int64_t amount) {
    if (tx_id.empty()) return TxResult::InvalidId;
    if (Contains(tx_id)) return TxResult::AlreadyApplied;
    if (amount < 0) return TxResult::InvalidAmount;
    if (amount > std::numeric_limits<std::int64_t>::max() - balance_) return TxResult::InvalidAmount;
    balance_ += amount;
    transactions_.emplace(std::string(tx_id), amount);
    return TxResult::Applied;
}

TxResult Ledger::Debit(std::string_view tx_id, std::int64_t amount) {
    if (tx_id.empty()) return TxResult::InvalidId;
    if (Contains(tx_id)) return TxResult::AlreadyApplied;
    if (amount < 0) return TxResult::InvalidAmount;
    if (amount > balance_) return TxResult::InsufficientFunds;
    balance_ -= amount;
    transactions_.emplace(std::string(tx_id), -amount);
    return TxResult::Applied;
}

bool Ledger::Rollback(std::string_view tx_id) {
    const auto it = transactions_.find(tx_id);
    if (it == transactions_.end()) return false;
    const std::int64_t reverted = balance_ - it->second;
    if (reverted < 0) return false;
    balance_ = reverted;
    transactions_.erase(it);
    return true;
}

Ledger Ledger::Restore(std::int64_t balance, StringMap<std::int64_t> transactions) {
    Ledger l(balance);
    l.transactions_ = std::move(transactions);
    return l;
}

Rating ComputeRating(const EconomyConfig::RatingRules& rules, double time_seconds,
                     double par_seconds, double damage_taken, double hp_max, int deaths) {
    if (deaths > 0) return Rating::C;
    int points = 0;
    if (par_seconds > 0.0) {
        if (time_seconds <= par_seconds * rules.s_time_fraction) {
            points += 2;
        } else if (time_seconds <= par_seconds * rules.a_time_fraction) {
            points += 1;
        }
    }
    if (damage_taken <= 0.0) {
        points += 2;
    } else if (hp_max > 0.0 && damage_taken / hp_max <= rules.low_damage_fraction) {
        points += 1;
    }
    if (points >= 4) return Rating::S;
    if (points == 3) return Rating::A;
    if (points == 2) return Rating::B;
    return Rating::C;
}

std::optional<std::int64_t> UpgradeCost(const EconomyConfig& economy, int current_level, bool uncapped) {
    if (current_level < 1) return std::nullopt;
    const auto& table = economy.upgrades.cost_to_next;
    const auto index = static_cast<std::size_t>(current_level - 1);
    if (current_level < economy.upgrades.max_level && index < table.size()) return table[index];
    if (!uncapped || table.empty()) return std::nullopt;
    // Past the table: the last step plus half of it for every level beyond.
    const std::int64_t last = table.back();
    const auto beyond = static_cast<std::int64_t>(current_level) - static_cast<std::int64_t>(table.size());
    return last + last / 2 * beyond;
}

double LevelDamageBonus(const EconomyConfig& economy, int level) {
    if (level <= 1) return 0.0;
    return static_cast<double>(level - 1) * economy.upgrades.damage_pct_per_level / 100.0;
}

}  // namespace rm
