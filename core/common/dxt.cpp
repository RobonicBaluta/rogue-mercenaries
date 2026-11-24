#include "rm/dxt.h"

#include <algorithm>
#include <cmath>

namespace rm::dxt {
namespace {

std::uint16_t Read16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t Read32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
void Write16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void Write32(std::uint8_t* p, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) p[k] = static_cast<std::uint8_t>(v >> (8 * k));
}

// 5:6:5 to 8 bits a channel by repeating the high bits (as the hardware does), and back.
Rgba Expand565(std::uint16_t c) {
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return {static_cast<std::uint8_t>((r << 3) | (r >> 2)), static_cast<std::uint8_t>((g << 2) | (g >> 4)),
            static_cast<std::uint8_t>((b << 3) | (b >> 2)), 255};
}
std::uint16_t Pack565(float r, float g, float b) {
    const auto q = [](float v, int most) { return std::clamp(static_cast<int>(std::lround(v * most / 255.0f)), 0, most); };
    return static_cast<std::uint16_t>((q(r, 31) << 11) | (q(g, 63) << 5) | q(b, 31));
}
std::uint16_t Pack565(const Rgba& c) { return Pack565(c.r, c.g, c.b); }

std::uint8_t Mix(int a, int b, int wa, int wb) { return static_cast<std::uint8_t>((a * wa + b * wb) / (wa + wb)); }

// The palette of a colour block: four colours, or (DXT1 with the first not greater) three and transparent.
std::array<Rgba, 4> Palette(std::uint16_t c0, std::uint16_t c1, bool four) {
    const Rgba p0 = Expand565(c0), p1 = Expand565(c1);
    std::array<Rgba, 4> pal{p0, p1, {}, {}};
    if (four) {
        pal[2] = {Mix(p0.r, p1.r, 2, 1), Mix(p0.g, p1.g, 2, 1), Mix(p0.b, p1.b, 2, 1), 255};
        pal[3] = {Mix(p0.r, p1.r, 1, 2), Mix(p0.g, p1.g, 1, 2), Mix(p0.b, p1.b, 1, 2), 255};
    } else {
        pal[2] = {Mix(p0.r, p1.r, 1, 1), Mix(p0.g, p1.g, 1, 1), Mix(p0.b, p1.b, 1, 1), 255};
        pal[3] = {0, 0, 0, 0};
    }
    return pal;
}

Texels DecodeColour(const std::uint8_t* colour, bool dxt1) {
    const std::uint16_t c0 = Read16(colour), c1 = Read16(colour + 2);
    const std::uint32_t indices = Read32(colour + 4);
    const auto pal = Palette(c0, c1, !dxt1 || c0 > c1);
    Texels out;
    for (int i = 0; i < 16; ++i) out[static_cast<std::size_t>(i)] = pal[(indices >> (2 * i)) & 3];
    return out;
}

}  // namespace

Texels DecodeDxt1(const std::uint8_t* block) { return DecodeColour(block, true); }

Texels DecodeDxt5(const std::uint8_t* block) {
    Texels out = DecodeColour(block + 8, false);
    const int a0 = block[0], a1 = block[1];
    int ramp[8] = {a0, a1};
    if (a0 > a1) {
        for (int k = 1; k <= 6; ++k) ramp[k + 1] = ((7 - k) * a0 + k * a1) / 7;
    } else {
        for (int k = 1; k <= 4; ++k) ramp[k + 1] = ((5 - k) * a0 + k * a1) / 5;
        ramp[6] = 0;
        ramp[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int k = 0; k < 6; ++k) bits |= static_cast<std::uint64_t>(block[2 + k]) << (8 * k);
    for (int i = 0; i < 16; ++i) out[static_cast<std::size_t>(i)].a = static_cast<std::uint8_t>(ramp[(bits >> (3 * i)) & 7]);
    return out;
}

std::array<std::uint8_t, 16> EncodeDxt5(const Texels& texels) {
    std::array<std::uint8_t, 16> block{};
    // Alpha: its highest and lowest, the eight-step ramp between (both ends exact).
    int hi = 0, lo = 255;
    for (const Rgba& t : texels) {
        hi = std::max<int>(hi, t.a);
        lo = std::min<int>(lo, t.a);
    }
    block[0] = static_cast<std::uint8_t>(hi);
    block[1] = static_cast<std::uint8_t>(lo);
    if (hi > lo) {
        int ramp[8] = {hi, lo};
        for (int k = 1; k <= 6; ++k) ramp[k + 1] = ((7 - k) * hi + k * lo) / 7;
        std::uint64_t bits = 0;
        for (int i = 0; i < 16; ++i) {
            int best = 0;
            for (int k = 1; k < 8; ++k) {
                if (std::abs(texels[static_cast<std::size_t>(i)].a - ramp[k]) < std::abs(texels[static_cast<std::size_t>(i)].a - ramp[best])) best = k;
            }
            bits |= static_cast<std::uint64_t>(best) << (3 * i);
        }
        for (int k = 0; k < 6; ++k) block[2 + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(bits >> (8 * k));
    }
    // Colour: the ends of the visible texels along their main direction (the covariance's largest
    // axis, by a few power steps); all texels when none is visible.
    float mean[3] = {0, 0, 0};
    int n = 0;
    const auto counts = [&](const Rgba& t) { return t.a > 0 || hi == 0; };
    for (const Rgba& t : texels) {
        if (!counts(t)) continue;
        mean[0] += t.r;
        mean[1] += t.g;
        mean[2] += t.b;
        ++n;
    }
    for (float& m : mean) m /= static_cast<float>(n);
    float cov[3][3] = {};
    for (const Rgba& t : texels) {
        if (!counts(t)) continue;
        const float d[3] = {t.r - mean[0], t.g - mean[1], t.b - mean[2]};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) cov[a][b] += d[a] * d[b];
    }
    float axis[3] = {0.577f, 0.577f, 0.577f};
    for (int step = 0; step < 8; ++step) {
        float next[3] = {};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) next[a] += cov[a][b] * axis[b];
        const float len = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
        if (len < 1e-6f) break;
        for (int a = 0; a < 3; ++a) axis[a] = next[a] / len;
    }
    float tmin = 0.0f, tmax = 0.0f;
    for (const Rgba& t : texels) {
        if (!counts(t)) continue;
        const float p = (t.r - mean[0]) * axis[0] + (t.g - mean[1]) * axis[1] + (t.b - mean[2]) * axis[2];
        tmin = std::min(tmin, p);
        tmax = std::max(tmax, p);
    }
    std::uint16_t c0 = Pack565(mean[0] + axis[0] * tmax, mean[1] + axis[1] * tmax, mean[2] + axis[2] * tmax);
    std::uint16_t c1 = Pack565(mean[0] + axis[0] * tmin, mean[1] + axis[1] * tmin, mean[2] + axis[2] * tmin);
    if (c0 < c1) std::swap(c0, c1);  // four colours either way (DXT5); kept in DXT1's order too
    const auto pal = Palette(c0, c1, true);
    std::uint32_t indices = 0;
    if (c0 != c1) {
        for (int i = 0; i < 16; ++i) {
            const Rgba& t = texels[static_cast<std::size_t>(i)];
            int best = 0, bestDist = 1 << 30;
            for (int k = 0; k < 4; ++k) {
                const int dr = t.r - pal[static_cast<std::size_t>(k)].r, dg = t.g - pal[static_cast<std::size_t>(k)].g,
                          db = t.b - pal[static_cast<std::size_t>(k)].b;
                const int dist = dr * dr + dg * dg + db * db;
                if (dist < bestDist) {
                    bestDist = dist;
                    best = k;
                }
            }
            indices |= static_cast<std::uint32_t>(best) << (2 * i);
        }
    }
    Write16(block.data() + 8, c0);
    Write16(block.data() + 10, c1);
    Write32(block.data() + 12, indices);
    return block;
}

Rgba ApplyTint(const Tint& tint, Rgba c) {
    const float luma = (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / 255.0f * tint.lift;
    const float target[3] = {tint.r * luma, tint.g * luma, tint.b * luma};
    const float from[3] = {static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b)};
    std::uint8_t out[3];
    for (int k = 0; k < 3; ++k) {
        out[k] = static_cast<std::uint8_t>(std::clamp(std::lround(from[k] + tint.strength * (target[k] - from[k])), 0L, 255L));
    }
    return {out[0], out[1], out[2], c.a};
}

void TintColourBlock(std::uint8_t* colour, bool dxt1, const Tint& tint) {
    const std::uint16_t c0 = Read16(colour), c1 = Read16(colour + 2);
    std::uint32_t indices = Read32(colour + 4);
    // DXT5's colour part has four colours whatever their order; it is kept with the first greater,
    // so a reader that went by the order would agree.
    const bool four = !dxt1 || c0 > c1;
    std::uint16_t n0 = Pack565(ApplyTint(tint, Expand565(c0)));
    std::uint16_t n1 = Pack565(ApplyTint(tint, Expand565(c1)));
    if (four) {
        if (n0 < n1) {
            std::swap(n0, n1);
            indices ^= 0x55555555u;  // 0 <-> 1, 2 <-> 3: the same colours from the other end
        } else if (n0 == n1) {
            indices = 0;  // one colour: every texel the first (never index 3, transparent in DXT1's other kind)
        }
    } else if (n0 > n1) {
        std::swap(n0, n1);
        std::uint32_t swapped = 0;
        for (int i = 0; i < 16; ++i) {
            std::uint32_t k = (indices >> (2 * i)) & 3;
            if (k < 2) k ^= 1;  // the two ends trade places; the middle and transparent stay
            swapped |= k << (2 * i);
        }
        indices = swapped;
    }
    Write16(colour, n0);
    Write16(colour + 2, n1);
    Write32(colour + 4, indices);
}

std::vector<std::uint8_t> RecolourDxt5(const std::vector<std::uint8_t>& blocks, int across, const Tint& tint, const Overlay* overlay) {
    std::vector<std::uint8_t> out = blocks;
    if (across <= 0) return out;
    const bool drawn = overlay != nullptr && overlay->width > 0 && overlay->height > 0 &&
                       overlay->pixels.size() == static_cast<std::size_t>(overlay->width) * static_cast<std::size_t>(overlay->height);
    const std::size_t count = out.size() / 16;
    for (std::size_t n = 0; n < count; ++n) {
        std::uint8_t* block = out.data() + n * 16;
        const int px = static_cast<int>(n % static_cast<std::size_t>(across)) * 4, py = static_cast<int>(n / static_cast<std::size_t>(across)) * 4;
        const bool under = drawn && px + 4 > overlay->x && px < overlay->x + overlay->width && py + 4 > overlay->y && py < overlay->y + overlay->height;
        if (!under) {
            TintColourBlock(block + 8, false, tint);
            continue;
        }
        Texels texels = DecodeDxt5(block);
        for (int i = 0; i < 16; ++i) {
            Rgba& t = texels[static_cast<std::size_t>(i)];
            t = ApplyTint(tint, t);
            const int ox = px + i % 4 - overlay->x, oy = py + i / 4 - overlay->y;
            if (ox < 0 || oy < 0 || ox >= overlay->width || oy >= overlay->height) continue;
            const Rgba& o = overlay->pixels[static_cast<std::size_t>(oy) * static_cast<std::size_t>(overlay->width) + static_cast<std::size_t>(ox)];
            const int a = o.a;
            const auto mix = [a](int over, int below) { return static_cast<std::uint8_t>((over * a + below * (255 - a) + 127) / 255); };
            t = {mix(o.r, t.r), mix(o.g, t.g), mix(o.b, t.b), static_cast<std::uint8_t>(a + (t.a * (255 - a) + 127) / 255)};
        }
        const auto encoded = EncodeDxt5(texels);
        std::copy(encoded.begin(), encoded.end(), block);
    }
    return out;
}

}  // namespace rm::dxt
