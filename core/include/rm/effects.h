#pragma once

// The mod's own effects (2026-11-11, the user: "can we make our own effects?"): recoloured copies of
// the game's, built at run time from the player's own archives (nothing of the game's is shipped).
// The formats (research/re5-engine-map.md "Effect files"): an effect list (.efl) is "EFL\0",
// version 0x20080415, its data size and part count, then the data the game copies into memory as it
// is (rEffectList's load 0x005780B0): a table of 16-byte part entries, each part's data at its
// entry's +4 >> 8 (the low byte its kind), colours as BGRA words at fixed places of a part. A
// texture (.tex) is "TEX\0", version 0x70, its mip count, size and fourcc, its mip table at +40 and
// the mips' blocks one after another. An archive (.arc, version 7) is "ARC\0", a version, a count,
// 80-byte entries (path, type, packed size, 0x40000000 | size, offset) and zlib data from 0x8000.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rm::fx {

using Bytes = std::vector<std::uint8_t>;

// Resource types (their DTI's +0x20, as the archives name them).
inline constexpr std::uint32_t kTypeEffectList = 0x6D5AE854;  // rEffectList (.efl)
inline constexpr std::uint32_t kTypeTexture = 0x241F5DEB;     // rTexture (.tex)
inline constexpr std::uint32_t kTypeEffectAnim = 0x4E397417;  // rEffectAnim (a texture's frames)

/// The colour given the hue `hue` (degrees), keeping its saturation and value (HSV): a grey, white
/// and black stay as they are.
void Colorize(std::uint8_t& r, std::uint8_t& g, std::uint8_t& b, double hue);

/// Recolours an effect list in place: only its parts' colour fields (the BGRA words at +12, +72
/// and +76 of every part; +376 and +380 of a kind 1 part, +88 and +92 of a kind 4 one; their alpha
/// kept). The number of colours changed, or nullopt when `efl` is not an effect list this reads.
std::optional<int> RecolorEffectList(Bytes& efl, double hue);

/// Replaces every `from` with `to` (the same length) in an effect list's data: its inline texture
/// and frame paths. How many it replaced (0 when the lengths differ or it is no effect list).
int ReplaceInlinePaths(Bytes& efl, std::string_view from, std::string_view to);

/// Recolours a DXT5 texture in place: both endpoint colours of every block of every mip. The
/// number of blocks, or nullopt for anything else (another format, a mip table that does not add up).
std::optional<int> RecolorDxt5(Bytes& tex, double hue);

/// A zlib stream (RFC 1950) holding `data` in stored DEFLATE blocks (no compression).
Bytes ZlibStored(const Bytes& data);

struct ArcEntry {
    std::string path;  // "effect\efl\wp\wp21\wp2100": no extension, at most 63 characters
    std::uint32_t type = 0;
    Bytes data;        // decompressed
};

/// An archive of `entries` (version 7, the data from 0x8000, each entry a stored zlib stream).
/// Empty when an entry's path is too long or its data too large (16 MB).
Bytes WriteArchive(const std::vector<ArcEntry>& entries);

/// The decompressed entry `path` (exactly as stored) of type `type`; nullopt when absent or broken.
std::optional<Bytes> ReadArchiveEntry(const Bytes& archive, std::string_view path, std::uint32_t type);

/// The recoloured flamethrower, by element: its stream (from wp2100), the stream's companion (from
/// wp2104) and a small flame for a hit's look (from wp2103), at "effect\efl\wp\wp21\wp21<code><n>".
struct FlameLook {
    const char* element;  // rm::ElementName
    char code;            // the paths' letter
    double hue;
    bool stream;          // the stream and its companion too (with recoloured textures); else the look only
};
inline constexpr FlameLook kFlameLooks[] = {
    {"cryo", 'c', 195.0, true},
    {"toxic", 'd', 105.0, true},
    {"shock", 'e', 280.0, true},
    {"incendiary", 'f', 25.0, false},
};
inline constexpr const char* kEffectsArchive = "rmEffects";  // Image\Archive\rmEffects.arc
/// Raised when what BuildFlameEffects makes changes (a stale archive is built again).
inline constexpr int kEffectsVersion = 2;
/// Plague's death cloud (the user, 2026-11-22: "use the incendiary grenade effect for the miasma from the
/// upgraded toxic version (but green of course) on the dead enemy"): the incendiary grenade's fire
/// (uWp07's effect\cef\wp\wp07 names this list in coreEff.arc) in the toxic look's green, at
/// FlameEffectPath(kMiasmaCode, 0); every texture it names recoloured under "effect\tex\g<nnn>".
inline constexpr const char* kMiasmaSource = "effect\\efl\\core\\cr0012";
inline constexpr char kMiasmaCode = 'g';

/// "effect\efl\wp\wp21\wp21<code><part>": part 0 the stream, 4 its companion, 3 the hit's look.
std::string FlameEffectPath(char code, int part);

/// The mod's effects archive from the game's uWp21.arc and coreEff.arc (their bytes): the
/// flamethrower's three effect lists recoloured for every kFlameLooks entry, and for those with a
/// stream the coloured textures they use (tx0003, tx0006, tx0007, tx0008) recoloured under their own
/// folder ("effect\tex\c006\tx0006_BM") with their frames, the lists' paths renamed to them. One
/// line per step in `log`; nullopt when a source entry is missing or not as expected. Also Plague's
/// cloud (kMiasmaSource) when coreEff.arc has it (else left out, said in `log`).
std::optional<Bytes> BuildFlameEffects(const Bytes& wp21Arc, const Bytes& coreEffArc, std::vector<std::string>* log);

}  // namespace rm::fx
