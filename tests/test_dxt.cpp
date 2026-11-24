// The DXT block maths behind the element guns' recoloured pictures and models (rm/dxt.h).

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "rm/dxt.h"

using rm::dxt::Rgba;
using rm::dxt::Texels;
using rm::dxt::Tint;

namespace {

std::array<std::uint8_t, 8> ColourBlock(std::uint16_t c0, std::uint16_t c1, std::uint32_t indices) {
    std::array<std::uint8_t, 8> b{};
    b[0] = static_cast<std::uint8_t>(c0);
    b[1] = static_cast<std::uint8_t>(c0 >> 8);
    b[2] = static_cast<std::uint8_t>(c1);
    b[3] = static_cast<std::uint8_t>(c1 >> 8);
    for (int k = 0; k < 4; ++k) b[4 + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(indices >> (8 * k));
    return b;
}

std::uint16_t First(const std::uint8_t* b) { return static_cast<std::uint16_t>(b[0] | (b[1] << 8)); }
std::uint16_t Second(const std::uint8_t* b) { return static_cast<std::uint16_t>(b[2] | (b[3] << 8)); }

// Within what 5:6:5 and the palette's thirds allow.
bool Near(const Rgba& a, const Rgba& b, int slack = 10) {
    return std::abs(a.r - b.r) <= slack && std::abs(a.g - b.g) <= slack && std::abs(a.b - b.b) <= slack && a.a == b.a;
}

const Tint kFire{0.5f, 255.0f, 140.0f, 60.0f, 1.25f};

}  // namespace

TEST_CASE("DXT1 and DXT5 blocks decode to their palettes", "[dxt]") {
    // Red and blue, the first greater: four colours; index 2 two thirds red.
    auto block = ColourBlock(0xF800, 0x001F, 0b10'01'00u);
    Texels t = rm::dxt::DecodeDxt1(block.data());
    CHECK((t[0].r == 255 && t[0].g == 0 && t[0].b == 0 && t[0].a == 255));
    CHECK((t[1].r == 0 && t[1].b == 255));
    CHECK((t[2].r == 170 && t[2].b == 85));
    // The first not greater: three colours and transparent (index 3).
    block = ColourBlock(0x001F, 0xF800, 0b11'10u);
    t = rm::dxt::DecodeDxt1(block.data());
    CHECK((t[0].r == 127 && t[0].b == 127 && t[0].a == 255));
    CHECK(t[1].a == 0);
    // DXT5: alpha 255..0 in eight steps; its colour part has four colours whatever their order.
    std::array<std::uint8_t, 16> five{};
    five[0] = 255;
    five[1] = 0;
    five[2] = 0b001'000;  // texel 0: 255, texel 1: 0
    const auto colour = ColourBlock(0x001F, 0xF800, 0b11'00u);
    std::copy(colour.begin(), colour.end(), five.begin() + 8);
    t = rm::dxt::DecodeDxt5(five.data());
    CHECK((t[0].a == 255 && t[0].b == 255));
    CHECK((t[1].a == 0 && t[1].r == 170));  // index 3 is a colour here, not transparent
}

TEST_CASE("Every 5:6:5 colour survives the trip to 8 bits and back", "[dxt]") {
    // The tint's zero strength must leave a block's bytes as they were.
    int changed = 0;
    for (std::uint32_t c = 0; c < 0x10000; ++c) {
        auto block = ColourBlock(static_cast<std::uint16_t>(c), 0, 0x1B1B1B1Bu);
        const auto before = block;
        rm::dxt::TintColourBlock(block.data(), false, Tint{0.0f});
        if (block != before && !(c == 0)) ++changed;
    }
    CHECK(changed == 0);
}

TEST_CASE("DXT5 encoding keeps a block close to its texels", "[dxt]") {
    Texels in;
    for (int i = 0; i < 16; ++i) {
        in[static_cast<std::size_t>(i)] = {static_cast<std::uint8_t>(40 + i * 4), static_cast<std::uint8_t>(30 + i * 3),
                                           static_cast<std::uint8_t>(200 - i * 3), static_cast<std::uint8_t>(i < 4 ? 0 : 255)};
    }
    const auto block = rm::dxt::EncodeDxt5(in);
    const Texels out = rm::dxt::DecodeDxt5(block.data());
    for (int i = 0; i < 16; ++i) {
        INFO("texel " << i);
        CHECK(out[static_cast<std::size_t>(i)].a == in[static_cast<std::size_t>(i)].a);
        if (in[static_cast<std::size_t>(i)].a > 0) CHECK(Near(out[static_cast<std::size_t>(i)], in[static_cast<std::size_t>(i)], 16));
    }
}

TEST_CASE("A recoloured picture: every block tinted, the badge drawn over its corner", "[dxt]") {
    // A 16x8 region (4 blocks across, 2 down) of one grey.
    Texels grey;
    for (int i = 0; i < 16; ++i) grey[static_cast<std::size_t>(i)] = {128, 128, 128, 255};
    std::vector<std::uint8_t> region;
    for (int n = 0; n < 8; ++n) {
        const auto block = rm::dxt::EncodeDxt5(grey);
        region.insert(region.end(), block.begin(), block.end());
    }
    rm::dxt::Overlay badge;
    badge.width = 8;
    badge.height = 4;
    badge.x = 8;  // over blocks 2 and 3 of the first row
    badge.y = 0;
    for (int i = 0; i < 32; ++i) badge.pixels.push_back(i % 8 < 4 ? Rgba{0, 0, 255, 255} : Rgba{0, 0, 0, 0});
    const auto out = rm::dxt::RecolourDxt5(region, 4, kFire, &badge);
    REQUIRE(out.size() == region.size());
    const Rgba tinted = rm::dxt::ApplyTint(kFire, {128, 128, 128, 255});
    for (int n = 0; n < 8; ++n) {
        const Texels t = rm::dxt::DecodeDxt5(out.data() + n * 16);
        INFO("block " << n);
        if (n == 2) {
            CHECK(Near(t[0], {0, 0, 255, 255}));  // under the badge's opaque half
        } else {
            CHECK(Near(t[0], tinted));  // the badge's clear half (block 3) shows the tinted grey too
        }
    }
    // Without a badge, only the colours move: the alpha bytes stay as they were.
    const auto plain = rm::dxt::RecolourDxt5(region, 4, kFire, nullptr);
    for (int n = 0; n < 8; ++n) CHECK(std::equal(plain.begin() + n * 16, plain.begin() + n * 16 + 8, region.begin() + n * 16));
}

TEST_CASE("A tint through a block's two colours recolours every texel", "[dxt]") {
    SECTION("DXT1, four colours") {
        auto block = ColourBlock(0xC618, 0x2104, 0xE4E4E4E4u);  // light and dark grey, all four indices
        const Texels before = rm::dxt::DecodeDxt1(block.data());
        rm::dxt::TintColourBlock(block.data(), true, kFire);
        CHECK(First(block.data()) > Second(block.data()));  // still four colours
        const Texels after = rm::dxt::DecodeDxt1(block.data());
        for (int i = 0; i < 16; ++i) {
            INFO("texel " << i);
            CHECK(Near(after[static_cast<std::size_t>(i)], rm::dxt::ApplyTint(kFire, before[static_cast<std::size_t>(i)])));
        }
        CHECK(after[0].r > after[0].b);  // warmer than grey
    }
    SECTION("DXT1, three colours and transparent: the transparent texels stay transparent") {
        auto block = ColourBlock(0x2104, 0xC618, 0b11'10'01'00u);
        const Texels before = rm::dxt::DecodeDxt1(block.data());
        rm::dxt::TintColourBlock(block.data(), true, kFire);
        CHECK(First(block.data()) <= Second(block.data()));
        const Texels after = rm::dxt::DecodeDxt1(block.data());
        CHECK(after[3].a == 0);
        for (int i = 0; i < 3; ++i) CHECK(Near(after[static_cast<std::size_t>(i)], rm::dxt::ApplyTint(kFire, before[static_cast<std::size_t>(i)])));
    }
    SECTION("DXT1: a tint that reverses the colours' order swaps them and their indices") {
        // Dark red is the greater number but grows darker than green under a full grey cast.
        auto block = ColourBlock(0x8000, 0x07E0, 0b11'10'01'00u);
        const Texels before = rm::dxt::DecodeDxt1(block.data());
        const Tint grey{1.0f, 255.0f, 255.0f, 255.0f, 1.0f};
        rm::dxt::TintColourBlock(block.data(), true, grey);
        CHECK(First(block.data()) > Second(block.data()));
        const Texels after = rm::dxt::DecodeDxt1(block.data());
        for (int i = 0; i < 4; ++i) {
            INFO("texel " << i);
            CHECK(Near(after[static_cast<std::size_t>(i)], rm::dxt::ApplyTint(grey, before[static_cast<std::size_t>(i)])));
        }
    }
    SECTION("DXT5: the alpha half is not the tint's") {
        std::array<std::uint8_t, 16> five{};
        five[0] = 200;
        five[1] = 10;
        for (int k = 2; k < 8; ++k) five[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(0x5A + k);
        const auto colour = ColourBlock(0xC618, 0x2104, 0x1B1B1B1Bu);
        std::copy(colour.begin(), colour.end(), five.begin() + 8);
        const Texels before = rm::dxt::DecodeDxt5(five.data());
        rm::dxt::TintColourBlock(five.data() + 8, false, kFire);
        const Texels after = rm::dxt::DecodeDxt5(five.data());
        for (int i = 0; i < 16; ++i) {
            INFO("texel " << i);
            CHECK(after[static_cast<std::size_t>(i)].a == before[static_cast<std::size_t>(i)].a);
            CHECK(Near(after[static_cast<std::size_t>(i)], rm::dxt::ApplyTint(kFire, before[static_cast<std::size_t>(i)])));
        }
    }
}
