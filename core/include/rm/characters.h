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
    // "dash", "dual_smg", "backup", "burst", "element_pick" (the tester), "flip"; a charm's transformation's
    // "transform" and "uroboros_strike" (CharmTransform); empty = none
    std::string kind;
    std::string name;              // HUD label
    std::string icon;              // icons/<icon>.png
    double cooldown_seconds = 0.0;
    int every_arenas = 0;          // > 0: once per this many arenas (Josh: 2)
    double power = 0.0;            // dash: metres; dual_smg, burst: damage per enemy (per pulse)
    double duration_seconds = 0.0; // dash, burst: invulnerability; dual_smg: burst length
    [[nodiscard]] bool Any() const { return !kind.empty(); }
};

/// A charm's transformation (the user, 2026-11-24: "at 30 if you have midnight wesker the burst button combo
/// should transform you into uroboros wesker ... for 12 seconds you are invulnerable (don't even flinch) and the
/// other button combo lets you do some uroboros wesker attacks, with a high cooldown on the transformation (but
/// lets you do burst in the meantime)"). While the charm is carried with `stacks` kill stacks (0: its
/// kill_stack_max) on the skin `skin`, the special button `slot` transforms in place of its own special; for
/// special.duration_seconds the player takes no damage and no hit's reaction and breaks every grab, and the other
/// button does `attack`. special.cooldown_seconds runs from the transformation's end; meanwhile the button does
/// its own special again.
struct CharmTransform {
    std::string skin;
    int stacks = 0;
    int slot = 1;
    SkinSpecial special;             // kind "transform"; power: the blast as it starts
    double cinematic_seconds = 0.0;  // its start's scene: the enemies held, the camera on the player
    SkinSpecial attack;              // kind "uroboros_strike": the other button's while transformed
    [[nodiscard]] bool Any() const { return special.Any(); }
};

/// A charm (the user's Perk Workshop picks, 2026-11-24): an item that takes a cell of the grid and
/// acts while the player carries it (the adapter reports what is carried, RunController::SetCarriedCharms).
/// The game holds it as `game_item`, a treasure no run ever hands out; the adapter draws `picture` over
/// that treasure's picture and gives it `name`. A skin's build or a perk's brings it; it is handed over
/// when the run starts.
struct CharmDef {
    std::string id;                    // "fidelity_card"
    std::string name;                  // "Fidelity Card"
    std::string description;
    std::int64_t game_item = -1;       // a treasure (category 4)
    std::string picture;               // configs\charms\<file>.png, 102x102
    int chest_weapon_every = 0;        // every n-th chest opened while carried holds a weapon
    std::string chest_weapon_rarity;   // ... of at least this rarity (empty: any)
    /// Each of the player's kills while carried adds a stack (up to kill_stack_max); each stack adds
    /// these run stats ("damage", "defense", "crit", "move_speed").
    StringMap<double> kill_stack_stats;
    int kill_stack_max = 0;
    bool stacks_end_on_hit = false;    // a hit the player takes clears the stacks
    bool breaks_grabs = false;         // while carried, a grab is broken at once (as SkinBuild::break_grabs)
    SkinSpecial special;               // usable while carried, on the special's button when the build has none
    CharmTransform transform;          // the kill stacks' last step (Midnight Wesker's Uroboros)
    /// The stacks its transformation needs.
    [[nodiscard]] int TransformStacks() const { return transform.stacks > 0 ? transform.stacks : kill_stack_max; }
};

/// Rounds that come by themselves when every gun the player carries is empty and none of their
/// rounds is in the grid (Scavenger): for the gun in hand's category, after `seconds`.
struct AmmoRegen {
    double seconds = 0.0;
    int rounds = 0;
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
    /// More base ids handed over at common rarity when the run starts, after starting_weapon (the
    /// tester's Gatling and rocket launcher).
    std::vector<std::string> more_starting_weapons;
    /// The run starts with the build's own weapons only (the tester, 2026-11-22: "make my tester
    /// character have only the flamethrower"): the hands take its first one, and the pistol and its
    /// rounds go (the adapter).
    bool only_build_weapons = false;
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
    /// Game item -> the item the run gets in its place wherever it comes from: picked up, a chest, a reward,
    /// the shop (which lists the item given) and the perks' items (Safari Chris, 2026-11-23: "should pick only
    /// change mines from grenades and stuff, also when bought from shop or given by chest, but should get ammo").
    std::vector<std::pair<std::int64_t, std::int64_t>> items_as;
    /// Proximity bombs (the adapter tells their hits by the bomb's unit): this much more damage (a fraction),
    /// and a wider blast: those within mine_blast_m metres of an enemy the blast hit take mine_blast_fraction
    /// of its biggest hit.
    double mine_damage = 0.0;
    double mine_blast_m = 0.0;
    double mine_blast_fraction = 0.0;
    /// An item every so many of the player's kills (Safari Chris: a bomb every 8; the user, 2026-11-23:
    /// "some way to generate mines"); -1 = none.
    std::int64_t kill_item = -1;
    int kill_item_every = 0;
    int kill_item_amount = 1;
    /// The damage explosions do to the player (the adapter: a hit by no character, a bomb's, a grenade's,
    /// a barrel's, a rocket's), times this (Safari Chris: 0.5; "he should also get less damage from explosions").
    double explosion_damage_mult = 1.0;
    /// Hand grenades (the adapter tells their hits by the grenade's unit, uWp06; the Perk Workshop's
    /// Grenade Pouch and Squad Tactics): this much more damage, a wider blast as the bombs' (mine_blast_*),
    /// and cluster_bomblets bomblets that go off around it a moment later, each cluster_damage_fraction of
    /// its biggest hit to the enemies within cluster_radius_m (never to a player).
    double grenade_damage = 0.0;
    double grenade_blast_m = 0.0;
    double grenade_blast_fraction = 0.0;
    int cluster_bomblets = 0;
    double cluster_damage_fraction = 0.0;
    double cluster_radius_m = 0.0;
    bool incendiary_burns = false;     // an incendiary grenade's hits burn as the Incendiary element (Fire Pouch)
    bool flash_masked = false;         // a flash grenade stuns the masked Majini too (Flash Pouch)
    double flash_extra_seconds = 0.0;  // ... and every stun it makes lasts this much longer
    /// Weapon category -> the rounds that come when everything is empty (Scavenger; the adapter).
    StringMap<AmmoRegen> empty_ammo_regen;
    std::vector<CharmDef> charms;      // handed over when the run starts
    bool break_grabs = false;        // a grab is broken at once: the grabber's own grab is ended (the adapter's families)
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
    // The run starts with a handgun in the hands: a costume whose loadout holds another gun (Fairy
    // Tale Sheva's M3, under Hound Wolf Sheva) takes its own handgun instead (the adapter).
    bool pistol_in_hand = false;
    // Weapons whose rounds never run out (the tester): the adapter marks them the way the game's own
    // Infinite Ammo bonus does, so no shot uses a round and a one-use gun is never thrown away.
    WeaponFilter infinite_ammo;
    SkinSpecial special;
    SkinSpecial special_2;  // a second special on its own button (Wesker's burst)
    std::string special_charm;  // the charm whose special `special` is (ApplyCharmSpecial): usable while it is carried
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
    /// Listed and playable only on an install that asks for it (the adapter's "test_character",
    /// RunController::SetTesterCharacters): a character for testing, never shown to players.
    bool tester_only = false;
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
/// A build without a special takes the first of its charms' (special_charm names that charm).
void ApplyCharmSpecial(SkinBuild& build);

}  // namespace rm
