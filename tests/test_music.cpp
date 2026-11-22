// The arena music (2026-11-22, the user: "cycle through the games battle themes ... the mercenaries
// theme should also be there, but not always"): the game's stream lists and each arena's track.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "rm/music.h"

using namespace rm::music;

namespace {

void PutU32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    if (b.size() < at + 4) b.resize(at + 4);
    for (int k = 0; k < 4; ++k) b[at + k] = static_cast<std::uint8_t>(v >> (8 * k));
}

// A list as the game's are laid out (st_bgm.stq: version 0x12, entries at 0x3C).
std::vector<std::uint8_t> MakeList(const std::vector<StreamEntry>& entries) {
    std::vector<std::uint8_t> b(0x3C, 0);
    std::memcpy(b.data(), "STRQ", 4);
    PutU32(b, 0x04, 0x12);
    PutU32(b, 0x08, static_cast<std::uint32_t>(entries.size()));
    const std::uint32_t first = 0x3C, end = first + static_cast<std::uint32_t>(entries.size()) * 0x18;
    PutU32(b, 0x20, first);
    PutU32(b, 0x24, end);
    b.resize(end, 0);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::size_t at = first + i * 0x18;
        PutU32(b, at, static_cast<std::uint32_t>(b.size()));
        b.insert(b.end(), entries[i].path.begin(), entries[i].path.end());
        b.push_back(0);
        PutU32(b, at + 4, entries[i].size);
        PutU32(b, at + 8, entries[i].length);
        PutU32(b, at + 12, entries[i].channels);
        PutU32(b, at + 16, entries[i].loop_start);
        PutU32(b, at + 20, entries[i].loop_end);
    }
    return b;
}

}  // namespace

TEST_CASE("music: a stream list's entries, its battle tracks", "[music]") {
    const std::vector<StreamEntry> made = {
        {"sound\\bgm\\oto\\bgm\\b_bgm\\b_bgm001", 1763750, 4210688, 2, 569088, 4113792},
        {"sound\\bgm\\oto\\bgm\\a_bgm\\a_bgm004", 188571, 432128, 2, kNoLoop, kNoLoop},
        {"sound\\bgm\\oto\\bgm\\bgm\\bgm007", 1950378, 3994624, 2, 353152, 3897856},
    };
    std::string error;
    const auto entries = ParseStreamList(MakeList(made), &error);
    INFO(error);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].path == made[0].path);
    CHECK(entries[0].Leaf() == "b_bgm001");
    CHECK(entries[0].path_offset == 0x3C + 3 * 0x18);  // the paths after the entries
    CHECK(entries[0].size == 1763750);
    CHECK(entries[0].loop_end == 4113792);
    CHECK(entries[1].Loops() == false);
    CHECK(IsBattleTrack(entries[0]));
    CHECK_FALSE(IsBattleTrack(entries[1]));
    CHECK_FALSE(IsBattleTrack(entries[2]));  // loops, but not a b_bgm

    // Not a list, or one that does not fit: nothing.
    CHECK(ParseStreamList({'R', 'I', 'F', 'F'}, &error).empty());
    CHECK_FALSE(error.empty());
    auto cut = MakeList(made);
    cut.resize(0x50);
    CHECK(ParseStreamList(cut, &error).empty());
}

TEST_CASE("music: the game's own list parses, its battle tracks with their files", "[music]") {
    // Only with the game's folder in RM_GAME_DIR (its Resident Evil 5 folder); skipped without.
    std::string dir;
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, "RM_GAME_DIR") == 0 && value != nullptr) dir = value;
    std::free(value);
#else
    if (const char* value = std::getenv("RM_GAME_DIR")) dir = value;
#endif
    if (dir.empty()) {
        SUCCEED("RM_GAME_DIR is not set");
        return;
    }
    const std::filesystem::path game = std::filesystem::path(dir) / "nativePC_MT";
    const auto list = game / "sound" / "bgm" / "st_bgm.stq";
    std::error_code ec;
    REQUIRE(std::filesystem::exists(list, ec));
    std::ifstream in(list, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto entries = ParseStreamList(bytes);
    REQUIRE(entries.size() == 61);
    int battle = 0;
    for (const StreamEntry& e : entries) {
        const auto file = game / (e.path + ".sngw");
        INFO(e.path);
        REQUIRE(std::filesystem::exists(file, ec));
        CHECK(std::filesystem::file_size(file, ec) == e.size);  // the list holds each file's size
        battle += IsBattleTrack(e) ? 1 : 0;
    }
    CHECK(battle == 11);
}

TEST_CASE("music: every track once a round, never the same twice in a row", "[music]") {
    Rotation r(42);
    CHECK(r.Next().empty());  // no pool
    r.SetPool({"theme", "b1", "b2", "b3", "b4"});
    std::string last;
    for (int round = 0; round < 40; ++round) {
        std::set<std::string> seen;
        for (int i = 0; i < 5; ++i) {
            const std::string t = r.Next();
            CHECK(t != last);
            last = t;
            seen.insert(t);
        }
        CHECK(seen.size() == 5);  // the theme too, once a round: there, but not always
    }
    // The same pool keeps the round; another starts a new one.
    Rotation a(7), b(7);
    a.SetPool({"x", "y", "z"});
    b.SetPool({"z", "y", "x"});
    CHECK(a.Next() == b.Next());
    const std::string second = a.Next();
    a.SetPool({"x", "y", "z"});
    std::set<std::string> rest = {second, a.Next()};
    CHECK(rest.size() == 2);
    // One track: that one every time.
    Rotation one;
    one.SetPool({"only"});
    CHECK(one.Next() == "only");
    CHECK(one.Next() == "only");
}
