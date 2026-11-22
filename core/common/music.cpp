#include "rm/music.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace rm::music {
namespace {

bool ReadU32(const std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t& out) {
    if (at + 4 > b.size()) return false;
    out = static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 | static_cast<std::uint32_t>(b[at + 2]) << 16 |
          static_cast<std::uint32_t>(b[at + 3]) << 24;
    return true;
}

}  // namespace

std::string StreamEntry::Leaf() const {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::vector<StreamEntry> ParseStreamList(const std::vector<std::uint8_t>& bytes, std::string* error) {
    std::vector<StreamEntry> out;
    const auto fail = [&](const char* why) {
        if (error != nullptr) *error = why;
        return std::vector<StreamEntry>{};
    };
    if (bytes.size() < 0x28 || std::memcmp(bytes.data(), "STRQ", 4) != 0) return fail("not a stream list (no STRQ)");
    std::uint32_t count = 0, first = 0, end = 0;
    ReadU32(bytes, 0x08, count);
    ReadU32(bytes, 0x20, first);
    ReadU32(bytes, 0x24, end);
    constexpr std::uint32_t kEntry = 0x18;
    if (count == 0 || count > 4096 || first < 0x28 || end != first + count * kEntry || end > bytes.size()) {
        return fail("its entry table does not fit");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = first + static_cast<std::size_t>(i) * kEntry;
        std::uint32_t name = 0;
        StreamEntry e;
        ReadU32(bytes, at, name);
        ReadU32(bytes, at + 4, e.size);
        ReadU32(bytes, at + 8, e.length);
        ReadU32(bytes, at + 12, e.channels);
        ReadU32(bytes, at + 16, e.loop_start);
        ReadU32(bytes, at + 20, e.loop_end);
        if (name >= bytes.size()) return fail("an entry's path is outside the file");
        e.path_offset = name;
        const auto zero = std::find(bytes.begin() + name, bytes.end(), std::uint8_t{0});
        if (zero == bytes.end()) return fail("an entry's path has no end");
        e.path.assign(bytes.begin() + name, zero);
        out.push_back(std::move(e));
    }
    if (error != nullptr) error->clear();
    return out;
}

bool IsBattleTrack(const StreamEntry& e) {
    std::string lower = e.path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e.Loops() && lower.find("\\b_bgm\\") != std::string::npos;
}

Rotation::Rotation(std::uint64_t seed) : state_(seed != 0 ? seed : 0x9E3779B97F4A7C15ull) {}

std::uint64_t Rotation::NextRandom() {
    // xorshift64*
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1Dull;
}

void Rotation::SetPool(std::vector<std::string> tracks) {
    std::sort(tracks.begin(), tracks.end());
    tracks.erase(std::unique(tracks.begin(), tracks.end()), tracks.end());
    if (tracks == pool_) return;
    pool_ = std::move(tracks);
    bag_.clear();
}

std::string Rotation::Next() {
    if (pool_.empty()) return {};
    if (bag_.empty()) {
        bag_ = pool_;
        for (std::size_t i = bag_.size(); i > 1; --i) std::swap(bag_[i - 1], bag_[NextRandom() % i]);
        // A new round never opens with the track the last one closed on.
        if (bag_.size() > 1 && bag_.back() == last_) std::swap(bag_.back(), bag_.front());
    }
    last_ = bag_.back();
    bag_.pop_back();
    return last_;
}

}  // namespace rm::music
