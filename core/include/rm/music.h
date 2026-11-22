#pragma once

// The arena music (2026-11-22, the user: "Would it be possible to cycle through the games battle
// themes? Hearing the same Mercenaries Theme gets tiring" ... "the mercenaries theme should also be
// there, but not always"). The game's music is Ogg streams listed in stream lists
// (nativePC_MT\sound\bgm\st_bgm.stq, bgm_dc\st_bgm_dc.stq: "STRQ"), each entry the stream's path,
// file size, length and loop points (samples, 48 kHz). The adapter has the Mercenaries theme's entry
// play another track each arena; this reads the lists and picks the tracks.

#include <cstdint>
#include <string>
#include <vector>

namespace rm::music {

constexpr std::uint32_t kNoLoop = 0xFFFFFFFFu;

struct StreamEntry {
    std::string path;  // "sound\bgm\oto\bgm\b_bgm\b_bgm001" (no extension)
    std::uint32_t size = 0;      // the .sngw file's bytes
    std::uint32_t length = 0;    // samples
    std::uint32_t channels = 0;
    std::uint32_t loop_start = kNoLoop, loop_end = kNoLoop;
    [[nodiscard]] bool Loops() const { return loop_start != kNoLoop && loop_end != kNoLoop && loop_end > loop_start; }
    /// The file's name without its folders ("b_bgm001").
    [[nodiscard]] std::string Leaf() const;
};

/// A stream list's entries (on disk: "STRQ", the count at +8, the entries' offset at +0x20 and their
/// end at +0x24, 0x18 bytes each: the path's offset, size, length, channels, loop start, loop end).
/// Empty, with `error` set, when the bytes are not one.
std::vector<StreamEntry> ParseStreamList(const std::vector<std::uint8_t>& bytes, std::string* error = nullptr);

/// A battle track: one that loops, in a "b_bgm" folder.
bool IsBattleTrack(const StreamEntry& e);

/// Each arena's track: every one of the pool once, in a shuffled order, before any again, and never
/// the same twice in a row (across two rounds either). The same seed gives the same order.
class Rotation {
public:
    explicit Rotation(std::uint64_t seed = 0x9E3779B97F4A7C15ull);
    /// The pool (a change starts a new round; the same pool keeps the round going).
    void SetPool(std::vector<std::string> tracks);
    [[nodiscard]] const std::vector<std::string>& Pool() const { return pool_; }
    /// The next arena's track (empty with no pool).
    std::string Next();

private:
    std::uint64_t NextRandom();
    std::uint64_t state_;
    std::vector<std::string> pool_;
    std::vector<std::string> bag_;  // this round's tracks still to play, the next one last
    std::string last_;
};

}  // namespace rm::music
