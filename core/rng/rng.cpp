#include "rm/rng.h"

#include <limits>

namespace rm {
namespace {

constexpr std::uint64_t Rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

}  // namespace

std::uint64_t SplitMix64(std::uint64_t& state) {
    state += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::uint64_t Fnv1a64(std::string_view text) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001B3ull;
    }
    return h;
}

Rng::Rng(std::uint64_t seed) {
    std::uint64_t sm = seed;
    for (std::uint64_t& s : s_) s = SplitMix64(sm);
    if ((s_[0] | s_[1] | s_[2] | s_[3]) == 0) s_[0] = 1;  // xoshiro must not be all-zero
}

Rng Rng::Derive(std::uint64_t seed, std::string_view stream) {
    std::uint64_t sm = seed;
    return Rng(SplitMix64(sm) ^ Fnv1a64(stream));
}

std::uint64_t Rng::Next() {
    const std::uint64_t result = Rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = Rotl(s_[3], 45);
    ++draws_;
    return result;
}

double Rng::NextDouble() { return static_cast<double>(Next() >> 11) * 0x1.0p-53; }

std::int64_t Rng::UniformInt(std::int64_t lo, std::int64_t hi) {
    if (hi <= lo) return lo;
    const std::uint64_t span = static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo);
    if (span == std::numeric_limits<std::uint64_t>::max()) {
        return static_cast<std::int64_t>(Next());
    }
    const std::uint64_t range = span + 1;
    // Reject the low (2^64 mod range) values so every residue is equally likely.
    const std::uint64_t threshold = (~range + 1) % range;
    for (;;) {
        const std::uint64_t r = Next();
        if (r >= threshold) {
            return static_cast<std::int64_t>(static_cast<std::uint64_t>(lo) + r % range);
        }
    }
}

std::size_t Rng::Index(std::size_t n) {
    if (n <= 1) return 0;
    return static_cast<std::size_t>(UniformInt(0, static_cast<std::int64_t>(n - 1)));
}

double Rng::UniformReal(double lo, double hi) { return lo + (hi - lo) * NextDouble(); }

bool Rng::Chance(double p) { return NextDouble() < p; }

std::size_t Rng::WeightedIndex(std::span<const double> weights) {
    double total = 0.0;
    for (const double w : weights) {
        if (w > 0.0) total += w;
    }
    if (!(total > 0.0)) return weights.size();
    const double target = NextDouble() * total;
    double acc = 0.0;
    std::size_t last = weights.size();
    for (std::size_t i = 0; i < weights.size(); ++i) {
        if (!(weights[i] > 0.0)) continue;
        acc += weights[i];
        last = i;
        if (target < acc) return i;
    }
    return last;
}

}  // namespace rm
