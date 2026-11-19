#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <catch_amalgamated.hpp>

#include "rm/content.h"
#include "rm/profile.h"
#include "rm/run_controller.h"
#include "rm/unlock_all.h"
#include "test_support.h"

using namespace rm;

namespace {

std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::filesystem::path FileIn(const std::filesystem::path& dir, bool unlocked) { return dir / std::string(ProfileFileName(unlocked)); }

}  // namespace

TEST_CASE("the unlocked profile opens everything the shipped game locks", "[unlock-all]") {
    ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const Content& c = *r.content;

    // A profile that earned nothing does find locks: the check below can fail.
    Profile fresh;
    bool anyLocked = false;
    for (const CharacterDef& ch : c.characters()) {
        anyLocked = anyLocked || CheckCharacter(&ch, fresh.unlocked_characters) == CharacterGate::Locked;
        for (const SkinDef& s : ch.skins) anyLocked = anyLocked || CheckSkin(s, fresh.unlocked_characters) == CharacterGate::Locked;
    }
    REQUIRE(anyLocked);
    {
        test::Harness locked(r.content, fresh);
        bool anyClosed = false;
        for (const auto& length : locked.rc.LadderLengths()) anyClosed = anyClosed || !length.open;
        REQUIRE(anyClosed);
        CHECK(locked.rc.MaxRefineRarity() == Rarity::Legendary);
    }

    Profile open;
    UnlockEverything(open, c);
    for (const CharacterDef& ch : c.characters()) {
        CHECK(CheckCharacter(&ch, open.unlocked_characters) != CharacterGate::Locked);
        for (const SkinDef& s : ch.skins) CHECK(CheckSkin(s, open.unlocked_characters) != CharacterGate::Locked);
    }
    for (const PerkDef& p : c.perks()) CHECK(open.unlocked_perks.count(p.id) == 1);
    for (const WeaponBaseDef& w : c.weapons()) CHECK(open.unlocked_weapons.count(w.id) == 1);

    test::Harness h(r.content, open);
    for (const auto& length : h.rc.LadderLengths()) {
        CHECK(length.open);
        if (length.endless) continue;  // no bosses of its own
        CHECK(length.easy_beaten);
        CHECK(length.hard_beaten);
    }
    CHECK(h.rc.MaxRefineRarity() == Rarity::Exotic);
}

TEST_CASE("unlocking everything keeps what the profile had and pays its Credits once", "[unlock-all]") {
    ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    Profile p;
    p.stats.total_kills = 5;
    (void)p.credits.Credit("earned", 7);
    UnlockEverything(p, *r.content);
    const auto once = p.credits.balance();
    CHECK(once == 7 + 99999);
    CHECK(p.stats.total_kills == 5);
    UnlockEverything(p, *r.content);
    CHECK(p.credits.balance() == once);
}

TEST_CASE("the unlocked profile is another file and leaves the normal one as it was", "[unlock-all]") {
    CHECK(ProfileFileName(false) == "profile.json");
    CHECK(ProfileFileName(true) != ProfileFileName(false));

    ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    test::TempDir dir("unlock_all");

    ProfileStore normal(FileIn(dir.path(), false));
    Profile mine;
    (void)mine.credits.Credit("earned", 7);
    REQUIRE(normal.Save(mine).ok());
    const std::string before = ReadAll(FileIn(dir.path(), false));
    REQUIRE_FALSE(before.empty());

    ProfileStore unlocked(FileIn(dir.path(), true));
    auto test = unlocked.Load().profile;
    UnlockEverything(test, *r.content);
    REQUIRE(unlocked.Save(test).ok());
    REQUIRE(unlocked.Save(test).ok());  // a second save also writes its .bak

    CHECK(ReadAll(FileIn(dir.path(), false)) == before);
    ProfileStore back(FileIn(dir.path(), false));
    const auto loaded = back.Load();
    CHECK(loaded.profile.credits.balance() == 7);
    CHECK(loaded.profile.unlocked_perks.empty());
}

TEST_CASE("Ammo Belt makes every ammo stack 20 percent larger", "[unlock-all][perks]") {
    ContentLoadResult r = Content::LoadFromDirectory(test::ConfigDir());
    REQUIRE(r.ok());
    const PerkDef* belt = r.content->FindPerk("supply_ammo");
    REQUIRE(belt != nullptr);
    CHECK(belt->effects.empty());
    REQUIRE(belt->build.has_value());
    CHECK(belt->build->ammo_stack_mult == Catch::Approx(1.2));

    Profile p;
    UnlockEverything(p, *r.content);
    test::Harness h(r.content, p);
    REQUIRE(h.rc.EquipPerk("chris", "supply_ammo").ok());
    REQUIRE(h.rc.StartRun("chris", 1).ok());
    CHECK(h.rc.state().character.build.ammo_stack_mult == Catch::Approx(1.2));
}
