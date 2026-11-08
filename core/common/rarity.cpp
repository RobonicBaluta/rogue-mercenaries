#include "rm/rarity.h"

#include <algorithm>

#include "rm/economy.h"

namespace rm {

std::string_view RarityName(Rarity r) {
    switch (r) {
        case Rarity::Common: return "common";
        case Rarity::Uncommon: return "uncommon";
        case Rarity::Rare: return "rare";
        case Rarity::Epic: return "epic";
        case Rarity::Legendary: return "legendary";
        case Rarity::Exotic: return "exotic";
    }
    return "common";
}

std::optional<Rarity> ParseRarity(std::string_view name) {
    for (Rarity r : kAllRarities) {
        if (RarityName(r) == name) return r;
    }
    return std::nullopt;
}

RarityWeights ShiftRarityWeights(const RarityWeights& base, double shift) {
    const double factor = std::max(0.1, 1.0 + shift);
    RarityWeights out{};
    double scale = 1.0;
    for (std::size_t i = 0; i < kRarityCount; ++i) {
        out[i] = std::max(0.0, base[i]) * scale;
        scale *= factor;
    }
    return out;
}

std::string_view RatingName(Rating r) {
    switch (r) {
        case Rating::S: return "S";
        case Rating::A: return "A";
        case Rating::B: return "B";
        case Rating::C: return "C";
    }
    return "C";
}

std::optional<Rating> ParseRating(std::string_view name) {
    for (Rating r : {Rating::S, Rating::A, Rating::B, Rating::C}) {
        if (RatingName(r) == name) return r;
    }
    return std::nullopt;
}

}  // namespace rm
