#pragma once

// Block-compressed textures (DXT1 and DXT5), for recolouring the game's own: an element gun's
// inventory picture and its model's colour map (the user's item 4, 2026-11-24: "modifying the items
// either textures or adding some effect and changing the item image to reflect the element").
//
// A DXT block is 4x4 texels. DXT1: 8 bytes, two 5:6:5 colours and a 2-bit index each; the first
// colour greater than the second means four colours (the two, then 2/3 and 1/3 of the way), else
// three and transparent. DXT5: 8 bytes of alpha (two values, 3-bit indices into a ramp of eight),
// then a DXT1-style colour block that always has four colours.

#include <array>
#include <cstdint>
#include <vector>

namespace rm::dxt {

struct Rgba {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
};
using Texels = std::array<Rgba, 16>;  // a block's, row by row

Texels DecodeDxt1(const std::uint8_t* block);  // 8 bytes
Texels DecodeDxt5(const std::uint8_t* block);  // 16 bytes
/// 16 bytes: the alpha's own lowest and highest, the colours' ends along their main direction.
std::array<std::uint8_t, 16> EncodeDxt5(const Texels& texels);

/// A colour cast toward `r g b` (an element's): each colour moves `strength` of the way to that
/// colour at its own brightness (times `lift`, so a dark gun still shows it), clamped to 0..255.
/// Linear in the colour, so applying it to a block's two colours recolours its whole palette.
struct Tint {
    float strength = 0.0f;
    float r = 255.0f, g = 255.0f, b = 255.0f;
    float lift = 1.25f;
};
Rgba ApplyTint(const Tint& tint, Rgba c);
/// The tint on a block's colour part (8 bytes: a DXT1 block, or the second half of a DXT5 one),
/// through its two colours. A DXT1 block keeps its kind (four colours, or three and transparent):
/// its colours and indices swap when the tint would reverse their order.
void TintColourBlock(std::uint8_t* colour, bool dxt1, const Tint& tint);

/// A picture over a DXT5 region: `width` x `height` texels, straight alpha, row by row.
struct Overlay {
    std::vector<Rgba> pixels;
    int width = 0, height = 0;
    int x = 0, y = 0;  // its top left in the region, in texels (multiples of 4)
};
/// A DXT5 region's blocks (`across` blocks a row, row by row) with every colour tinted and, when
/// given, `overlay` drawn over the tinted colours (only the blocks under it are encoded again).
std::vector<std::uint8_t> RecolourDxt5(const std::vector<std::uint8_t>& blocks, int across, const Tint& tint, const Overlay* overlay);

}  // namespace rm::dxt
