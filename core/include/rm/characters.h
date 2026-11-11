#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rm/common.h"

namespace rm {

struct ProfileStats;

/// What a themed unlock challenge counts (configs/characters.json "unlock_kind").
enum class ChallengeKind {
    None,
    RunsCompleted,    // completed runs with any character
    RunsCompletedAs,  // completed runs as "unlock_character"
    TotalKills,
    BestArena,        // most arenas cleared in one run
    CreditsEarned,    // lifetime banked credits
    NoDamageArenas,   // arenas cleared without taking damage
    SRankArenas,      // arenas rated S
    BossBeaten,       // "unlock_boss" in the profile's bosses beaten (a ladder key or boss:<class>)
};
std::optional<ChallengeKind> ParseChallengeKind(std::string_view name);
std::string_view ChallengeKindName(ChallengeKind k);

/// A themed unlock condition shared by characters and skins (None = unlocked from the start).
struct Challenge {
    ChallengeKind kind = ChallengeKind::None;
    std::int64_t count = 0;  // target for the counted kind
    std::string character;   // for RunsCompletedAs
    std::string boss;        // for BossBeaten (ProfileStats::bosses_beaten)
    std::string text;        // shown to the player while locked
};

/// Weapons named by category ("shotgun", see weapons.h) or base id. Empty = every weapon.
struct WeaponFilter {
    std::vector<std::string> categories;
    std::vector<std::string> ids;
    [[nodiscard]] bool Empty() const { return categories.empty() && ids.empty(); }
    /// True for a weapon of a listed category or id; true for every weapon when empty.
    [[nodiscard]] bool Allows(std::string_view base_id, std::string_view category) const;
};

/// A build's special move, used with a button combo during an arena (the adapter reads the
/// input and performs it; the core keeps its cooldown and availability).
struct SkinSpecial {
    std::string kind;              // "dash", "dual_smg", "backup", "burst"; empty = none
    std::string name;              // HUD label
    std::string icon;              // icons/<icon>.png
    double cooldown_seconds = 0.0;
    int every_arenas = 0;          // > 0: once per this many arenas (Josh: 2)
    double power = 0.0;            // dash: metres; dual_smg, burst: damage per enemy (per pulse)
    double duration_seconds = 0.0; // dash, burst: invulnerability; dual_smg: burst length
    [[nodiscard]] bool Any() const { return !kind.empty(); }
};

/// A skin's build (configs/characters.json skin "build", ROADMAP M8): rules a run of that skin
/// plays by. Defaults change nothing.
struct SkinBuild {
    std::string name;              // the build's name in the menus
    std::string icon;              // icons/<icon>.png next to it
    std::string summary;           // one line for the SKINS screen
    WeaponFilter offered_weapons;  // rewards, the shop and chests offer only these
    WeaponFilter usable_weapons;   // others: the shop shows them but cannot sell them, rewards skip them
    StringMap<double> category_damage;  // additive damage fraction per weapon category
    double supplies_mult = 1.0;    // supplies earned in arenas and from rewards
    int free_shop_rerolls = 0;     // shop rerolls without cost, per shop visit
    double rarity_shift = 0.0;     // added to the rarity of rewards and shop offers
    WeaponFilter uncapped_upgrades;  // weapons that level past economy upgrades.max_level (empty = none)
    std::string starting_weapon;     // base id handed over when the run starts
    std::string starting_weapon_rarity;  // its rarity name (empty = common)
    double max_health_mult = 1.0;    // the adapter scales the player's maximum health
    /// Game item id -> the stack size the adapter allows for it during the run (JSON keys may be
    /// hex, "0x010A").
    std::vector<std::pair<std::int64_t, int>> stack_max;
    double ammo_stack_mult = 1.0;    // the adapter multiplies every ammo stack's size
    std::vector<std::string> extra_weapons;  // build-only weapons (weapons.json build_only) this build is offered
    double melee_damage = 0.0;       // additive damage fraction on melee hits (needs melee_detect)
    /// Each melee kill adds one stat for the rest of the run, picked at random: "damage",
    /// "defense", "max_health", "crit" -> the amount it adds (fractions).
    StringMap<double> melee_kill_stats;
    StringMap<double> melee_kill_caps;  // a stat at its cap is not rolled again (none: no cap)
    /// A run length's own pace (run length id -> stat -> gains): in a run of that length a stat
    /// reaches its cap in that many gains (each adds cap / gains), in place of melee_kill_stats.
    StringMap<StringMap<int>> melee_kill_gains_by_length;
    double heal_mult = 1.0;          // healing items heal this much
    /// A heal of at least this fraction of max health (spray, mixed herbs; 0 = never) also makes
    /// the player invulnerable and stronger for a while.
    double heal_buff_fraction = 0.0;
    double heal_invulnerable_seconds = 0.0;
    double heal_damage_bonus = 0.0;
    double heal_damage_seconds = 0.0;
    std::int64_t drops_as = -1;      // enemy drops and chest items become this game item (-1 = no)
    bool chest_keys_exempt = false;  // opens chests without keys
    int extra_chests = 0;            // chests placed in each arena besides economy chests.per_arena
    /// Game item id -> its stack size times this (after stack_max; JSON keys may be hex).
    std::vector<std::pair<std::int64_t, double>> stack_mult;
    WeaponFilter never_offered;      // rewards, the shop and chests never offer these
    bool drops_as_held_ammo = false; // ammo and recovery items picked up become ammo for the gun in hand
    /// Game item picked up -> the item given instead (JSON keys and values may be hex).
    std::vector<std::pair<std::int64_t, std::int64_t>> pickup_as;
    bool break_grabs = false;        // a grab is broken at once: the grabber is thrown off (Majini)
    /// Base id the shop always offers, with no unlock, until the run holds it (Heavy Metal's
    /// Gatling), at shop_weapon_price supplies (0: the common weapon price).
    std::string shop_weapon;
    std::int64_t shop_weapon_price = 0;
    // The rarity remake's elements on the build's guns (element names, rm::ParseElement): every gun's
    // hits carry gun_elements, and with arena_element one more random element, drawn each arena.
    std::vector<std::string> gun_elements;
    bool arena_element = false;
    bool refine_every_shop = false;  // every shop offers a refine (economy refine)
    double refine_price_mult = 1.0;  // refines in the shop cost this much
    double fuel_regen_mult = 1.0;    // the flamethrower's fuel comes back this much faster
    SkinSpecial special;
    SkinSpecial special_2;  // a second special on its own button (Wesker's burst)
    [[nodiscard]] const SkinSpecial& Special(int slot) const { return slot == 1 ? special_2 : special; }
    [[nodiscard]] bool Uncapped(std::string_view base_id, std::string_view category) const {
        return !uncapped_upgrades.Empty() && uncapped_upgrades.Allows(base_id, category);
    }
};

/// A character skin (costume). Overrides change the archetype a little: the weapons a run
/// can start with, the starting perk and extra character skills. Empty fields keep the
/// character's own values.
/// A third-party mod a skin wears (the supported mods, 2026-11-08): the player gets it from its
/// creator's page and unzips it into RogueMercenaries\mods; the adapter reads the listed files from
/// there instead of the game's (nothing in the game folder is written). Without it the skin is
/// listed but not selectable (CharacterGate::NotInstalled), with where to get it.
struct SkinModFile {
    std::string name;       // the archive's file name ("uPl05ExcellaNormal.arc", under nativePC_MT\Image\Archive)
    std::int64_t size = 0;  // its size in the supported version (0: any)
    std::vector<std::int64_t> sizes;  // or one of these (a mod's variants of the file)
    [[nodiscard]] bool Accepts(std::int64_t bytes) const {
        if (!sizes.empty()) return std::find(sizes.begin(), sizes.end(), bytes) != sizes.end();
        return size <= 0 || bytes == size;
    }
};

/// The mod's own picture for the SKINS screen, read from the player's copy of the mod (never
/// shipped): a texture entry of one of its archives, or a region of an image it comes with.
struct SkinModPortrait {
    std::string file;   // its name in the mod's folder ("Merce2Resource.arc", "A.jpg"), or the end of its path ("Leon dsc\\kreedpic.jpg")
    std::string entry;  // an archive's texture entry ("id\jpn\merce_dc\versus_22_360_NOMIP_BM"); empty: an image
    std::vector<int> crop;  // an image's x, y, width, height (empty: all of it)
};

struct SkinMod {
    std::string name;    // the mod's title on its page
    std::string author;  // its creator (empty: unknown)
    std::string url;     // its page (https://www.nexusmods.com/...)
    std::vector<SkinModFile> files;
    /// Folders of new files the costume asks for (its voice lines), under nativePC_MT
    /// ("sound\\bgm\\oto\\emo_voice\\mod\\leon"): read from the mod's copy, and required (a file the
    /// game cannot find ends it).
    std::vector<std::string> folders;
    std::optional<SkinModPortrait> portrait;
};

struct SkinDef {
    std::string id;
    std::string name;
    /// The adapter's costume/character slot for this skin (game-specific; -1 = unknown).
    std::int64_t costume_index = -1;
    std::vector<std::string> starting_weapons;
    std::string starting_passive;
    std::vector<std::string> skill_pool;
    Challenge unlock;
    SkinBuild build;
    std::optional<SkinMod> mod;  // a third-party mod's look (costume_index: the costume it stands in for)
};

/// Playable character definition (configs/characters.json).
struct CharacterDef {
    std::string id;
    std::string name;
    /// false = reserved/future entry; never selectable or purchasable until the adapter supports it.
    bool enabled = true;
    /// Weapon base ids a run may start with (the loadout roll picks one). Without the
    /// "weapon_id" capability the pick is used as the single sidearm slot.
    std::vector<std::string> starting_weapons;
    /// Perk id granted at run start (empty = none).
    std::string starting_passive;
    /// Character-specific perk ids allowed in addition to the general pool.
    std::vector<std::string> skill_pool;
    /// Free-form mechanic id implemented by the adapter (empty = none).
    std::string exclusive_mechanic;
    /// 0 = not purchasable with credits.
    std::int64_t unlock_cost_credits = 0;
    /// Challenge that unlocks the character. A character with neither a cost nor a
    /// challenge is unlocked from the start.
    Challenge unlock;
    /// The adapter's costume/character slot for this character (game-specific; -1 = unknown).
    std::int64_t costume_index = -1;
    /// Skins; the loader guarantees at least one (the first is the default).
    std::vector<SkinDef> skins;
    /// The build of the skin a run plays (set by ApplySkin; a character has none of its own).
    SkinBuild build;
};

/// NotInstalled: a mod skin whose mod is not in RogueMercenaries\mods (RunController::SetInstalledModSkins).
enum class CharacterGate { Selectable, Unknown, Disabled, Locked, NotInstalled };
std::string_view CharacterGateName(CharacterGate g);

/// Selectability for StartRun: unknown/disabled/locked characters are rejected.
CharacterGate CheckCharacter(const CharacterDef* def, const StringSet& unlocked_characters);
/// Locked while the skin's challenge is unmet (skin ids share the unlocked_characters set).
CharacterGate CheckSkin(const SkinDef& skin, const StringSet& unlocked_characters);

/// Progress toward a challenge, clamped to the target. met() is false without a challenge.
struct ChallengeProgress {
    std::int64_t current = 0;
    std::int64_t target = 0;
    [[nodiscard]] bool met() const { return target > 0 && current >= target; }
};
ChallengeProgress CheckChallenge(const Challenge& challenge, const ProfileStats& stats);
ChallengeProgress CheckChallenge(const CharacterDef& def, const ProfileStats& stats);

/// The named skin, or the default (first) one for an empty id; nullptr if unknown.
const SkinDef* FindSkin(const CharacterDef& def, std::string_view skin_id);
/// The character with the skin's overrides applied (what a run plays).
CharacterDef ApplySkin(const CharacterDef& def, const SkinDef& skin);
/// A perk's build changes on top of the run's (a style perk with a cost: Boulder Puncher, Superior
/// Being): category damage, melee damage and rarity shift add; health, heals, supplies and stack
/// sizes multiply; filters, conversions and flags join. Name, icon, specials and weapons stay.
void ApplyPerkBuild(SkinBuild& build, const SkinBuild& perk);

}  // namespace rm
