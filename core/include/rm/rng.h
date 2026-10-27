#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace rm {

/// Bump when the generator, seeding or Derive() mixing changes; golden tests pin version 1.
inline constexpr std::uint32_t kRngVersion = 1;

/// Named streams derived from a run seed. Each consumer owns one so that drawing from one
/// stream (e.g. a crit roll) never shifts another (e.g. the next reward offer).
namespace streams {
inline constexpr std::string_view kReward = "reward";
inline constexpr std::string_view kRoute = "route";
inline constexpr std::string_view kLoot = "loot";
inline constexpr std::string_view kShop = "shop";
inline constexpr std::string_view kCombat = "combat";
}  // namespace streams

std::uint64_t SplitMix64(std::uint64_t& state);
std::uint64_t Fnv1a64(std::string_view text);

/// xoshiro256** seeded through splitmix64. Integer-only state transitions and exact
/// 53-bit doubles keep sequences identical on x86 and x64 (requires SSE2 doubles, the
/// MSVC default; never build the core with /arch:IA32).
class Rng {
public:
    explicit Rng(std::uint64_t seed);

    /// Independent stream for (seed, name). Different names give uncorrelated sequences.
    static Rng Derive(std::uint64_t seed, std::string_view stream);

    std::uint64_t Next();
    /// Uniform in [0, 1) with 53 bits of precision.
    double NextDouble();
    /// Uniform in [lo, hi] (inclusive), unbiased via rejection. Returns lo when hi <= lo.
    std::int64_t UniformInt(std::int64_t lo, std::int64_t hi);
    /// Uniform index in [0, n). n must be > 0.
    std::size_t Index(std::size_t n);
    /// Uniform in [lo, hi).
    double UniformReal(double lo, double hi);
    /// True with probability p. Always consumes exactly one draw.
    bool Chance(double p);
    /// Index chosen proportionally to positive weights; returns weights.size() (without
    /// consuming a draw) when no weight is positive.
    std::size_t WeightedIndex(std::span<const double> weights);

    template <class T>
    void Shuffle(std::vector<T>& items) {
        for (std::size_t i = items.size(); i > 1; --i) {
            const std::size_t j = Index(i);
            std::swap(items[i - 1], items[j]);
        }
    }

    /// Number of 64-bit draws consumed so far (diagnostics/tests).
    [[nodiscard]] std::uint64_t draws() const { return draws_; }

private:
    std::uint64_t s_[4] = {};
    std::uint64_t draws_ = 0;
};

}  // namespace rm
