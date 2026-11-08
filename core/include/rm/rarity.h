#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace rm {

/// Shared rarity ladder for perks, weapons and reward bundles (ordered low -> high). Exotic is a
/// weapon's tier above Legendary, reached only by refining one (rm::RefineWeapon): its weights and
/// prices in the economy tables default to 0, so nothing rolls it.
enum class Rarity { Common = 0, Uncommon = 1, Rare = 2, Epic = 3, Legendary = 4, Exotic = 5 };

inline constexpr std::size_t kRarityCount = 6;
inline constexpr std::array<Rarity, kRarityCount> kAllRarities = {
    Rarity::Common, Rarity::Uncommon, Rarity::Rare, Rarity::Epic, Rarity::Legendary, Rarity::Exotic,
};

/// Per-rarity weights indexed by RarityIndex().
using RarityWeights = std::array<double, kRarityCount>;

constexpr std::size_t RarityIndex(Rarity r) { return static_cast<std::size_t>(r); }
std::string_view RarityName(Rarity r);
std::optional<Rarity> ParseRarity(std::string_view name);

/// Shifts weights toward rarer tiers: w[i] * max(0.1, 1 + shift)^i.
/// shift > 0 favours rare results (route rarity_bonus, good rating); shift < 0 favours common.
RarityWeights ShiftRarityWeights(const RarityWeights& base, double shift);

}  // namespace rm
