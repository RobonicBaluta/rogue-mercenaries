// The mod's own effects (rm/effects.h): recolouring the game's effect lists and textures, and the
// archive they go into. Synthetic files in the formats the game's own have (the colour fields at
// the places every part of the game's 117 core and weapon lists has them, 2026-11-11).

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>

#include <catch_amalgamated.hpp>

#include "rm/effects.h"
#include "rm/inflate.h"

using namespace rm;
using namespace rm::fx;

namespace {

void Put32(Bytes& b, std::size_t at, std::uint32_t v) {
    if (b.size() < at + 4) b.resize(at + 4, 0);
    for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint32_t Get32(const Bytes& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 | static_cast<std::uint32_t>(b[at + 2]) << 16 |
           static_cast<std::uint32_t>(b[at + 3]) << 24;
}
void Put16(Bytes& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v & 0xFF);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
std::uint16_t Get16(const Bytes& b, std::size_t at) { return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8); }

constexpr std::uint32_t kOrange = 0xFFFF8000;  // BGRA bytes 00 80 FF FF: R 255, G 128, B 0
constexpr std::uint32_t kFloat50000 = 0x47435000;

// An effect list of two parts: kind 1 at 0x40 (0x200 bytes), kind 0 at 0x240 (0x200 bytes).
Bytes MakeEffectList() {
    const std::size_t size = 0x440;
    Bytes b(32 + size, 0);
    std::memcpy(b.data(), "EFL\0", 4);
    Put32(b, 4, 0x20080415);
    Put32(b, 8, static_cast<std::uint32_t>(size));
    Put32(b, 12, 2);
    const std::size_t data = 32;
    Put32(b, data + 4, 0x40u << 8 | 1);   // part 0: at 0x40, kind 1
    Put32(b, data + 20, 0x240u << 8 | 0); // part 1: at 0x240, kind 0
    for (const std::size_t f : {12, 72, 76, 376, 380}) Put32(b, data + 0x40 + f, kOrange);
    for (const std::size_t f : {12, 72, 76}) Put32(b, data + 0x240 + f, kOrange);
    Put32(b, data + 0x240 + 476, kFloat50000);  // a number where some parts of kind 0 have one
    Put32(b, data + 0x240 + 376, kOrange);      // kind 0 has no colour at 376: left as it is
    const char* path = "effect\\tex\\0006\\tx0006_BM";
    std::memcpy(b.data() + data + 0x240 + 0x100, path, std::strlen(path));
    return b;
}

// A DXT5 texture of 8 x 8 with 4 mips (4 + 1 + 1 + 1 blocks), every endpoint pure red.
Bytes MakeTexture() {
    const unsigned mips = 4;
    const std::size_t first = 40 + 4 * mips;
    Bytes b(first, 0);
    std::memcpy(b.data(), "TEX\0", 4);
    b[4] = 0x70;
    b[8] = static_cast<std::uint8_t>(mips);
    b[9] = 1;
    Put16(b, 12, 8);
    Put16(b, 14, 8);
    std::memcpy(b.data() + 20, "DXT5", 4);
    std::size_t at = first;
    const unsigned blocks[] = {4, 1, 1, 1};
    for (unsigned m = 0; m < mips; ++m) {
        Put32(b, 40 + 4 * m, static_cast<std::uint32_t>(at));
        at += blocks[m] * 16;
    }
    b.resize(at, 0xAA);  // alpha and index bytes
    for (std::size_t o = first; o < at; o += 16) {
        Put16(b, o + 8, 0xF800);   // red
        Put16(b, o + 10, 0xF800);
    }
    return b;
}

// Where CTest wants files for the Python readers (RM_TEST_OUTPUT_DIR), as test_telemetry.cpp.
std::optional<std::filesystem::path> OutputDir() {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, "RM_TEST_OUTPUT_DIR") != 0 || value == nullptr) return std::nullopt;
    std::filesystem::path p(value);
    std::free(value);
#else
    const char* value = std::getenv("RM_TEST_OUTPUT_DIR");
    if (value == nullptr) return std::nullopt;
    std::filesystem::path p(value);
#endif
    std::filesystem::create_directories(p);
    return p;
}

Bytes MakeAnim() {
    Bytes b(64, 0);
    std::memcpy(b.data(), "EAN\0", 4);
    return b;
}

}  // namespace

TEST_CASE("a colour takes a hue and keeps its saturation and value", "[fx]") {
    std::uint8_t r = 255, g = 0, b = 0;
    Colorize(r, g, b, 120.0);
    CHECK((r == 0 && g == 255 && b == 0));
    r = 255, g = 128, b = 0;  // orange
    Colorize(r, g, b, 200.0);
    CHECK((r == 0 && g == 170 && b == 255));
    r = 200, g = 200, b = 200;  // a grey stays
    Colorize(r, g, b, 200.0);
    CHECK((r == 200 && g == 200 && b == 200));
    r = 255, g = 161, b = 109;  // the flame's own, as a half saturated blue
    Colorize(r, g, b, 240.0);
    CHECK((r == 109 && g == 109 && b == 255));
}

TEST_CASE("an effect list's colour fields are recoloured and nothing else", "[fx]") {
    Bytes list = MakeEffectList();
    const Bytes before = list;
    const auto changed = RecolorEffectList(list, 240.0);
    REQUIRE(changed.has_value());
    CHECK(*changed == 8);  // five in the kind 1 part, three in the kind 0 one
    const std::uint32_t blue = 0xFF0000FF;  // B 255, G 0, R 0, A 255
    for (const std::size_t f : {12, 72, 76, 376, 380}) CHECK(Get32(list, 32 + 0x40 + f) == blue);
    for (const std::size_t f : {12, 72, 76}) CHECK(Get32(list, 32 + 0x240 + f) == blue);
    CHECK(Get32(list, 32 + 0x240 + 476) == kFloat50000);
    CHECK(Get32(list, 32 + 0x240 + 376) == kOrange);
    std::size_t differing = 0;
    for (std::size_t i = 0; i < list.size(); ++i) differing += list[i] != before[i] ? 1 : 0;
    CHECK(differing == 24);  // B, G and R of each of the eight colours (orange's green goes too)

    Bytes wrong = list;
    std::memcpy(wrong.data(), "TEX\0", 4);
    CHECK_FALSE(RecolorEffectList(wrong, 240.0).has_value());
    Bytes version = MakeEffectList();
    Put32(version, 4, 0x20090101);
    CHECK_FALSE(RecolorEffectList(version, 240.0).has_value());
}

TEST_CASE("an effect list's texture paths are renamed in place", "[fx]") {
    Bytes list = MakeEffectList();
    CHECK(ReplaceInlinePaths(list, "effect\\tex\\0006\\", "effect\\tex\\c006\\") == 1);
    const char* renamed = "effect\\tex\\c006\\tx0006_BM";
    CHECK(std::memcmp(list.data() + 32 + 0x240 + 0x100, renamed, std::strlen(renamed)) == 0);
    CHECK(ReplaceInlinePaths(list, "effect\\tex\\0006\\", "effect\\tex\\c0006\\") == 0);  // another length
}

TEST_CASE("a DXT5 texture's endpoints are recoloured in every mip", "[fx]") {
    Bytes tex = MakeTexture();
    const auto blocks = RecolorDxt5(tex, 240.0);
    REQUIRE(blocks.has_value());
    CHECK(*blocks == 7);
    for (std::size_t o = 56; o < tex.size(); o += 16) {
        CHECK(Get16(tex, o + 8) == 0x001F);  // blue
        CHECK(Get16(tex, o + 10) == 0x001F);
        CHECK(tex[o] == 0xAA);  // the alpha untouched
        CHECK(tex[o + 12] == 0xAA);  // the indices untouched
    }
    Bytes dxt1 = MakeTexture();
    std::memcpy(dxt1.data() + 20, "DXT1", 4);
    CHECK_FALSE(RecolorDxt5(dxt1, 240.0).has_value());
    Bytes shorter = MakeTexture();
    shorter.resize(shorter.size() - 16);
    CHECK_FALSE(RecolorDxt5(shorter, 240.0).has_value());
    Bytes table = MakeTexture();
    Put32(table, 44, Get32(table, 44) + 16);  // a mip where the blocks are not
    CHECK_FALSE(RecolorDxt5(table, 240.0).has_value());
}

TEST_CASE("a stored zlib stream inflates to its data", "[fx]") {
    for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{1000}, std::size_t{70000}, std::size_t{140000}}) {
        INFO(n);
        Bytes data(n);
        for (std::size_t i = 0; i < n; ++i) data[i] = static_cast<std::uint8_t>(i * 7 + i / 300);
        const Bytes z = ZlibStored(data);
        Bytes out;
        REQUIRE(ZlibInflate(z.data(), z.size(), out));
        CHECK(out == data);
    }
}

TEST_CASE("an archive gives back its entries", "[fx]") {
    std::vector<ArcEntry> entries = {{"effect\\tex\\c006\\tx0006_BM", kTypeTexture, MakeTexture()},
                                     {"effect\\tex\\c006\\tx0006", kTypeEffectAnim, MakeAnim()},
                                     {"effect\\efl\\wp\\wp21\\wp21c0", kTypeEffectList, MakeEffectList()}};
    const Bytes arc = WriteArchive(entries);
    REQUIRE(arc.size() > 0x8000);
    CHECK(std::memcmp(arc.data(), "ARC\0", 4) == 0);
    CHECK(arc[4] == 7);
    CHECK(arc[6] == 3);
    CHECK(Get32(arc, 8 + 64 + 12) == 0x8000);  // the first entry's data
    CHECK(Get32(arc, 8 + 64 + 8) == (0x40000000u | static_cast<std::uint32_t>(entries[0].data.size())));
    for (const ArcEntry& e : entries) {
        INFO(e.path);
        const auto back = ReadArchiveEntry(arc, e.path, e.type);
        REQUIRE(back.has_value());
        CHECK(*back == e.data);
    }
    CHECK_FALSE(ReadArchiveEntry(arc, "effect\\tex\\c006\\tx0006", kTypeTexture).has_value());  // another type
    CHECK(WriteArchive({{std::string(64, 'x'), kTypeTexture, {}}}).empty());  // a path too long

    // Left for an independent reader (tests/check_arc.py, Python's zlib).
    if (const auto dir = OutputDir()) {
        std::ofstream out(*dir / "effects.arc", std::ios::binary);
        out.write(reinterpret_cast<const char*>(arc.data()), static_cast<std::streamsize>(arc.size()));
    }
}

TEST_CASE("the flamethrower's looks are built for every element from the game's archives", "[fx]") {
    std::vector<ArcEntry> wp21, core;
    for (const char* n : {"wp2100", "wp2103", "wp2104"}) {
        wp21.push_back({std::string("effect\\efl\\wp\\wp21\\") + n, kTypeEffectList, MakeEffectList()});
    }
    for (const char* n : {"0003", "0006", "0007", "0008"}) {
        const std::string folder = std::string("effect\\tex\\") + n + "\\tx" + n;
        core.push_back({folder + "_BM", kTypeTexture, MakeTexture()});
        core.push_back({folder, kTypeEffectAnim, MakeAnim()});
    }
    std::vector<std::string> log;
    // Without the incendiary grenade's fire in coreEff.arc: the looks, Plague's cloud left out.
    const auto withoutCloud = BuildFlameEffects(WriteArchive(wp21), WriteArchive(core), &log);
    REQUIRE(withoutCloud.has_value());
    CHECK(withoutCloud->at(6) == 3 * (4 + 4 + 3) + 1);
    CHECK_FALSE(ReadArchiveEntry(*withoutCloud, FlameEffectPath(kMiasmaCode, 0), kTypeEffectList).has_value());
    // With it (2026-11-22): its green copy, its texture (0006) recoloured under g006.
    core.push_back({kMiasmaSource, kTypeEffectList, MakeEffectList()});
    log.clear();
    const auto built = BuildFlameEffects(WriteArchive(wp21), WriteArchive(core), &log);
    REQUIRE(built.has_value());
    // Cryo, Toxic and Shock: 4 textures, 4 frames, 3 lists each; Incendiary: its look only; the cloud:
    // its list, one texture and its frames.
    CHECK(built->at(6) == 3 * (4 + 4 + 3) + 1 + 3);
    const auto cloud = ReadArchiveEntry(*built, FlameEffectPath(kMiasmaCode, 0), kTypeEffectList);
    REQUIRE(cloud.has_value());
    const char* green = "effect\\tex\\g006\\tx0006_BM";
    CHECK(std::memcmp(cloud->data() + 32 + 0x240 + 0x100, green, std::strlen(green)) == 0);
    CHECK(Get32(*cloud, 32 + 0x40 + 12) != kOrange);
    CHECK(ReadArchiveEntry(*built, "effect\\tex\\g006\\tx0006_BM", kTypeTexture).has_value());
    CHECK(ReadArchiveEntry(*built, "effect\\tex\\g006\\tx0006", kTypeEffectAnim).has_value());
    const auto stream = ReadArchiveEntry(*built, FlameEffectPath('c', 0), kTypeEffectList);
    REQUIRE(stream.has_value());
    const char* renamed = "effect\\tex\\c006\\tx0006_BM";
    CHECK(std::memcmp(stream->data() + 32 + 0x240 + 0x100, renamed, std::strlen(renamed)) == 0);
    CHECK(Get32(*stream, 32 + 0x40 + 12) != kOrange);
    const auto image = ReadArchiveEntry(*built, "effect\\tex\\e008\\tx0008_BM", kTypeTexture);
    REQUIRE(image.has_value());
    CHECK(Get16(*image, 56 + 8) != 0xF800);
    const auto fire = ReadArchiveEntry(*built, FlameEffectPath('f', 3), kTypeEffectList);
    REQUIRE(fire.has_value());
    CHECK(std::memcmp(fire->data() + 32 + 0x240 + 0x100, "effect\\tex\\0006\\", 16) == 0);  // its textures as they are
    CHECK_FALSE(ReadArchiveEntry(*built, FlameEffectPath('f', 0), kTypeEffectList).has_value());

    // A source missing: nothing built, said why.
    std::vector<ArcEntry> partial(wp21.begin(), wp21.begin() + 2);
    log.clear();
    CHECK_FALSE(BuildFlameEffects(WriteArchive(partial), WriteArchive(core), &log).has_value());
    REQUIRE_FALSE(log.empty());
    CHECK(log.back().find("wp2104") != std::string::npos);
}

// The game's camera maths (rm/screen_fx.h), checked against what a right-handed camera must do.
#include "rm/screen_fx.h"

TEST_CASE("a point ahead of the game's camera shows where it should", "[fx]") {
    using namespace rm::screenfx;
    const float eye[3] = {0.0f, 150.0f, 0.0f}, target[3] = {0.0f, 150.0f, -100.0f}, up[3] = {0.0f, 1.0f, 0.0f};
    const Mat4 vp = Multiply(LookAtRH(eye, target, up), PerspectiveFovRH(60.0f, 16.0f / 9.0f, 10.0f, 10000.0f));
    const float ahead[3] = {0.0f, 150.0f, -500.0f};
    const auto c = Project(vp, ahead, 1280.0f, 720.0f);
    REQUIRE(c.has_value());
    CHECK(c->x == Catch::Approx(640.0f).margin(0.01));
    CHECK(c->y == Catch::Approx(360.0f).margin(0.01));
    CHECK(c->depth == Catch::Approx(500.0f).margin(0.01));
    // Looking down -z with y up, +x is to the right and +y up on the screen.
    const float right[3] = {100.0f, 150.0f, -500.0f}, above[3] = {0.0f, 250.0f, -500.0f};
    const auto r = Project(vp, right, 1280.0f, 720.0f), a = Project(vp, above, 1280.0f, 720.0f);
    REQUIRE((r.has_value() && a.has_value()));
    CHECK(r->x > 640.0f);
    CHECK(a->y < 360.0f);
    // At the edge of a 60 degree field of view: the top of the screen.
    const float edge[3] = {0.0f, 150.0f + 500.0f * std::tan(30.0f * 0.017453292f), -500.0f};
    const auto e = Project(vp, edge, 1280.0f, 720.0f);
    REQUIRE(e.has_value());
    CHECK(e->y == Catch::Approx(0.0f).margin(0.05));
    const float behind[3] = {0.0f, 150.0f, 500.0f};
    CHECK_FALSE(Project(vp, behind, 1280.0f, 720.0f).has_value());
}

TEST_CASE("a bolt runs from end to end within its jitter, the same for the same seed", "[fx]") {
    using namespace rm::screenfx;
    const auto bolt = BoltPath(100.0f, 100.0f, 500.0f, 100.0f, 10, 0.15f, 42);
    REQUIRE(bolt.size() == 11);
    CHECK(bolt.front().first == 100.0f);
    CHECK(bolt.front().second == 100.0f);
    CHECK(bolt.back().first == 500.0f);
    CHECK(bolt.back().second == 100.0f);
    bool bent = false;
    for (std::size_t i = 0; i < bolt.size(); ++i) {
        CHECK(std::fabs(bolt[i].second - 100.0f) <= 0.15f * 400.0f + 0.01f);
        CHECK(bolt[i].first == Catch::Approx(100.0f + 40.0f * static_cast<float>(i)).margin(0.01));
        bent = bent || std::fabs(bolt[i].second - 100.0f) > 1.0f;
    }
    CHECK(bent);
    CHECK(BoltPath(100.0f, 100.0f, 500.0f, 100.0f, 10, 0.15f, 42) == bolt);
    CHECK(BoltPath(100.0f, 100.0f, 500.0f, 100.0f, 10, 0.15f, 43) != bolt);
}
