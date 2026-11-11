#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rm/capabilities.h"
#include "rm/content.h"
#include "rm/events.h"
#include "rm/profile.h"
#include "rm/rng.h"
#include "rm/run_state.h"
#include "rm/status.h"

namespace rm {

// ---------------------------------------------------------------------------------------
// Adapter contract
//
//  * Threading: RunController is single-threaded. The adapter must serialise all calls
//    (e.g. queue engine callbacks and drain them on one thread).
//  * Time: every `now` is monotonic seconds supplied by the adapter (game or QPC clock).
//    The core never reads a clock. Kill-streak and after-melee windows compare these values.
//  * Capabilities: call SetCapabilities() with only runtime-verified features. Perks, weapon
//    affixes and shop offers whose capabilities are missing are never offered.
//  * Weapon ids: with "weapon_id", pass the weapon base id from weapons.json (e.g. "m92f")
//    in HitContext/KillEvent; instance modifiers apply only to matching hits/kills. Without
//    it the run has one sidearm instance that applies to every non-melee player hit/kill.
//  * Melee: is_melee/was_melee == true marks melee; weapon modifiers never apply to melee.
//    nullopt means "unknown" (treated as not melee).
//  * Heals: the core never heals directly. heal_on_kill and shop heals queue HealRequests
//    (fraction of max hp). Poll PendingHeal() / ConsumeHealRequests() and apply them in game;
//    consumed requests are gone, so apply exactly once.
//  * Damage hooks: call ComputeOutgoingMultiplier once per player hit (it may roll a crit) and
//    ComputeIncoming once per incoming hit before applying it (it may consume a cheat-death
//    charge). Then report the actual result with OnPlayerDamaged. Outside ArenaActive both
//    return a neutral 1.0 and never roll.
//  * Kills: only by_player kills count toward the enemy budget, perks and rewards unless
//    economy run.non_player_kills_count_toward_budget is true. Repeated reports of the same
//    enemy_id within one arena are ignored.
// ---------------------------------------------------------------------------------------

/// What a gun's element or legendary effect asks the adapter to do in the world (the rarity
/// remake): statuses on the target, blasts and arcs around it. The core decides; the adapter
/// plays it with the game's own reactions where the enemy class has them.
enum class ProcKind {
    Burn,          // the target burns: dps for seconds (a new burn keeps the higher dps, the later end)
    Poison,        // the target is poisoned: dps for seconds; it takes ElementDef(Toxic).taken_mult
    Freeze,        // the target is frozen for seconds; it takes ElementDef(Cryo).taken_mult
    Shock,         // the target is stunned; `damage` arcs to `targets` other enemies within `radius`
    IgniteAround,  // every enemy within `radius` of the target burns (dps, seconds)
    Arc,           // `damage` arcs to `targets` other enemies within `radius` (no stun on the target)
    Execute,       // the target dies now (never a boss)
    Explode,       // the victim blows up: `damage` to every enemy within `radius`, thrown
    Shatter,       // a frozen victim shatters: `damage` to every enemy within `radius`
    Overkill,      // `damage` to the nearest other enemy within `radius`
    Refill,        // the gun's magazine is full again
    PoisonCloud,   // the adapter's: a Plague-poisoned enemy died, every enemy within `radius` is poisoned (dps, seconds)
};
std::string_view ProcKindName(ProcKind k);

struct WeaponProc {
    ProcKind kind = ProcKind::Burn;
    std::string source;      // the element's name or the legendary effect's id
    double damage = 0.0;     // Shock, Arc, Explode, Shatter, Overkill: per enemy reached
    double dps = 0.0;        // Burn, Poison, IgniteAround
    double seconds = 0.0;    // Burn, Poison, Freeze, IgniteAround
    double radius = 0.0;     // engine units around the target or the victim
    int targets = 0;         // Shock, Arc: enemies reached (0: every one in the radius)
    std::string element;     // the element ("incendiary"; empty: a legendary effect's), for its look
    bool evolved = false;    // its evolved form (a Legendary gun's)
    int chain = 0;           // Explode: the blast's kills blow up too, this many times over
    double freeze_seconds = 0.0;  // Shatter: those it hits freeze this long
    /// Poison from Plague (evolved Toxic): when the poisoned enemy dies, however, a cloud of
    /// this fraction of its maximum health a second poisons those within cloud_radius.
    double cloud_dps_fraction = 0.0;
    double cloud_radius = 0.0;
};

/// A gun refined to Epic waiting for its element (RunController::ChooseElement).
struct ElementChoice {
    std::string instance_id;
    std::string base_id;
    Rarity rarity = Rarity::Epic;
    bool evolved = false;          // already Legendary: the element comes evolved
    std::vector<Element> offer;    // the two offered (option 0 and 1); option -1 is a random one
};

struct KillEvent {
    double now = 0.0;
    bool by_player = false;
    std::optional<std::string> weapon_id;
    std::optional<bool> was_melee;
    std::optional<std::string> enemy_id;
    std::optional<std::string> enemy_class;  // the unit's class ("uEm10"): key drops
    // The killing hit, for the gun's kill effects (the rarity remake).
    double overkill = 0.0;       // damage past zero health
    double target_hp_max = 0.0;
    bool target_frozen = false;  // frozen by a Cryo gun when it died: it shatters
    bool target_is_boss = false;
};

struct KillOutcome {
    bool counted = false;    // reduced the enemy budget
    bool duplicate = false;  // enemy_id already reported this arena
    bool ignored = false;    // no arena active
    std::int64_t supplies_awarded = 0;
    double heal_fraction_queued = 0.0;
    int remaining = 0;
    bool arena_complete = false;
    std::vector<std::string> triggered;  // perk ids / unique affix ids that fired
    bool key_dropped = false;            // a chest key (economy keys)
    std::string stat_gained;             // a build's melee-kill stat ("damage", ...), empty if none
    double stat_amount = 0.0;
    std::vector<WeaponProc> procs;       // the killing gun's kill effects (Explode, Shatter, Overkill, Refill)
};

/// A healing item's heal as the game applies it (the adapter's hook on the player's health add).
struct HealContext {
    double now = 0.0;
    double amount = 0.0;  // the game's heal amount
    double hp = 0.0;      // health before
    double hp_max = 0.0;
};

struct HealOutcome {
    double amount = 0.0;    // the heal to apply (the build's heal_mult)
    bool high_tier = false; // at least heal_buff_fraction of max health: the build's buffs started
    double invulnerable_seconds = 0.0;
    double damage_bonus = 0.0;
    double damage_seconds = 0.0;
};

/// The build's special for the HUD.
struct SpecialState {
    std::string kind;   // empty: the build has none
    std::string name;
    std::string icon;
    bool ready = false;          // usable now (an arena is running, cooled down, its arena allowed)
    double cooldown_left = 0.0;  // seconds
    double cooldown_total = 0.0;
    int arenas_left = 0;         // every_arenas: arenas until it can be used again
};

struct DamageEvent {
    double now = 0.0;
    double amount = 0.0;  // damage actually taken (after ComputeIncoming)
    double hp_before = 0.0;
    double hp_after = 0.0;
    double hp_max = 0.0;
};

struct HitContext {
    double now = 0.0;
    double base_damage = 0.0;
    std::optional<double> target_hp;      // needs "enemy_hp"
    std::optional<double> target_hp_max;
    std::optional<std::string> weapon_id;  // needs "weapon_id"
    std::optional<bool> is_melee;          // needs "melee_detect"
    std::optional<double> player_hp;       // optional; else last value seen in damage events
    std::optional<double> player_hp_max;
    // The rarity remake: the target's statuses (the adapter keeps them), whether it is the
    // arena's boss, and the rounds left in the gun's magazine after this shot (Last Word).
    bool target_frozen = false;
    bool target_poisoned = false;
    bool target_is_boss = false;
    std::optional<int> magazine_left;
};

/// Telemetry breakdown: final = clamp(raw, outgoing_min, outgoing_max) where
/// raw = route_mult * max(0, 1 + perk_bonus + kill_streak_bonus + build_bonus) * weapon_base_mult
///       * max(0, 1 + weapon_bonus) * (crit ? crit_multiplier : 1).
struct OutgoingBreakdown {
    double route_mult = 1.0;
    double perk_bonus = 0.0;
    double kill_streak_bonus = 0.0;
    double build_bonus = 0.0;  // the skin's build: damage by weapon category
    // (the build's uncapped levels count in weapon_bonus like any level)
    double weapon_base_mult = 1.0;
    double weapon_bonus = 0.0;
    double crit_chance = 0.0;
    double crit_multiplier = 1.0;
    bool crit = false;
    double raw = 1.0;
    double clamped = 1.0;
    std::string weapon_instance_id;  // empty when no instance applied
    double status_mult = 1.0;        // the target's statuses (frozen, poisoned): times raw
};

struct OutgoingResult {
    double multiplier = 1.0;  // clamped; multiply the engine's damage by this
    bool crit = false;
    OutgoingBreakdown breakdown;
    std::vector<WeaponProc> procs;  // the gun's elements and legendary effects on this hit
};

struct IncomingContext {
    double now = 0.0;
    double amount = 0.0;  // engine damage before modification
    double hp = 0.0;      // player hp before the hit
    double hp_max = 0.0;
};

struct IncomingResult {
    double multiplier = 1.0;  // clamped [incoming_min, incoming_max]
    double raw_multiplier = 1.0;
    /// The modified hit would kill but a cheat_death charge was consumed: set hp to leave_hp
    /// instead of applying lethal damage.
    bool prevent_death = false;
    double leave_hp = 0.0;
};

enum class UnlockKind { Character, Perk, Weapon };
std::string_view UnlockKindName(UnlockKind k);

struct UnlockReceipt {
    UnlockKind kind = UnlockKind::Character;
    std::string id;
    std::int64_t cost = 0;
    std::int64_t balance_after = 0;
};

/// "M92F, Hand Grenade x2, Green Herb" for the HUD and telemetry.
std::string FormatLoadout(const Loadout& loadout, const Content& content);

struct SkinAvailability {
    std::string id;
    std::string name;
    std::int64_t costume_index = -1;
    CharacterGate gate = CharacterGate::Selectable;
    std::string unlock_text;
    ChallengeProgress challenge;
};

struct CharacterAvailability {
    std::string id;
    std::string name;
    CharacterGate gate = CharacterGate::Unknown;
    std::int64_t unlock_cost_credits = 0;
    /// Challenge text and progress while locked (target 0 = no challenge).
    std::string unlock_text;
    ChallengeProgress challenge;
    std::int64_t costume_index = -1;  // the default skin's
    std::vector<SkinAvailability> skins;
};

struct RunControllerOptions {
    /// Verified engine features. Default: none (safe for an adapter that has verified nothing).
    Capabilities capabilities;
    IEventSink* sink = nullptr;
    /// Optional persistence; when null the profile lives only in memory (simulator/tests).
    ProfileStore* store = nullptr;
    /// Emit a damage_modified event for every Nth ComputeOutgoingMultiplier call (and always
    /// when the clamp changed the value). 0 disables sampling except clamped hits.
    int damage_sample_every = 50;
};

/// Single facade the game adapter (and the simulator) drives.
/// The adapter's inventory, when it can place items at once (in a match). Rewards and shop
/// purchases then succeed only when the item fits; without a port, grants queue up in
/// RunController::ConsumeItemGrants() for the adapter to apply later.
struct InventoryPort {
    virtual ~InventoryPort() = default;
    /// Returns the units placed (0 = nothing fit).
    virtual int Give(std::int64_t game_item, int count, std::string_view source) = 0;
    /// Whether the player has this item now, in the inventory or the run's storage; nullopt when it
    /// cannot tell (no player, the Organize screen open). A gun it says is gone leaves the run.
    virtual std::optional<bool> Holds(std::int64_t game_item) { (void)game_item; return std::nullopt; }
};

/// What a chest held (all already applied: supplies credited, items in the inventory).
struct ChestReward {
    bool locked = false;       // keys: no key held, nothing granted (the chest stays shut)
    bool key_used = false;
    std::int64_t supplies = 0;
    std::string item_id;       // catalog item placed, if any
    std::string item_name;
    int item_count = 0;
    std::string weapon_id;     // weapon placed, if any
    std::string weapon_name;
    std::string refined_instance_id;  // a gun refined one tier up (the rarity remake), if any
    std::string refined_name;
    std::string refined_rarity;       // the tier it reached
    int number = 0;            // chests opened this run, this one included
};

class RunController {
public:
    RunController(std::shared_ptr<const Content> content, Profile profile,
                  RunControllerOptions options = {});

    // --- configuration ---------------------------------------------------------------
    void SetCapabilities(Capabilities caps);
    void SetInventory(InventoryPort* port) { inventory_ = port; }
    /// Weapons the adapter can place in the current match (base ids); rewards and the shop
    /// offer only those. Clear with std::nullopt.
    void SetPlaceableWeapons(std::optional<StringSet> bases) { placeable_weapons_ = std::move(bases); }
    /// The mod skins whose mod files the adapter found (SkinDef::mod); the others are NotInstalled.
    void SetInstalledModSkins(StringSet skin_ids) { installed_mod_skins_ = std::move(skin_ids); }
    /// A skin's gate: its challenge, then (a mod skin) whether its mod is installed.
    [[nodiscard]] CharacterGate SkinGate(const SkinDef& skin) const;
    [[nodiscard]] const Capabilities& capabilities() const { return caps_; }
    void SetEventSink(IEventSink* sink) { sink_ = sink; }
    [[nodiscard]] const Content& content() const { return *content_; }

    // --- meta progression --------------------------------------------------------------
    [[nodiscard]] const Profile& profile() const { return profile_; }
    [[nodiscard]] std::vector<CharacterAvailability> Characters() const;
    /// Menu selection, one perk per category (style, supply, support) per character. The perk
    /// must be enabled, unlocked (or free), usable by the character and not a skin trait or
    /// curse; it replaces the perk held in its category. Saved to the profile.
    Status EquipPerk(std::string_view character_id, std::string_view perk_id);
    Status UnequipPerk(std::string_view character_id, PerkCategory category);
    /// The perks a run of this character starts with: the equipped ones, with the character's
    /// own starting trait standing in for an empty style slot.
    [[nodiscard]] std::vector<std::string> EquippedPerks(std::string_view character_id) const;
    [[nodiscard]] const PerkDef* EquippedPerk(std::string_view character_id, PerkCategory category) const;
    /// Unlocks every character whose challenge the profile stats now meet (character_unlocked
    /// events) and returns their ids. Runs at construction and after each run ends.
    std::vector<std::string> EvaluateCharacterUnlocks();
    /// Spends credits on a character/perk/weapon unlock (tx "unlock:<kind>:<id>") and saves.
    /// Not allowed while a run is active. Rolled back if saving fails.
    Result<UnlockReceipt> PurchaseUnlock(UnlockKind kind, std::string_view id);
    /// A crash-interrupted run left in the profile (see ActiveRunCheckpoint).
    [[nodiscard]] const std::optional<ActiveRunCheckpoint>& interrupted_run() const;
    /// Banks an interrupted run's pending credits at the death rate (idempotent by run id).
    Result<RunSummary> RecoverInterruptedRun();

    // --- run lifecycle -----------------------------------------------------------------
    /// Fails for unknown, disabled or locked characters or while a run is active. Settles
    /// any interrupted run, resets all temporary state, increments runs_started and saves.
    Status StartRun(std::string_view character_id, std::uint64_t seed);
    /// With a skin (empty id = the character's default skin). Locked skins are rejected.
    Status StartRun(std::string_view character_id, std::uint64_t seed, std::string_view skin_id);
    /// With a ladder length (EconomyConfig::Ladder; empty = the first): locked lengths are refused.
    Status StartRun(std::string_view character_id, std::uint64_t seed, std::string_view skin_id, std::string_view length_id);

    /// The ladder for the menu: every length in order, with what the profile has opened.
    struct LadderLengthView {
        std::string id;
        std::string name;
        int arenas = 0;           // without the finale's
        bool open = false;        // can be played
        bool hard_open = false;   // its hard boss is offered (its easy one beaten)
        bool finale_possible = false;  // the finale can follow its boss (in a run beating the needs_hard bosses)
        bool easy_beaten = false;
        bool hard_beaten = false;
    };
    [[nodiscard]] std::vector<LadderLengthView> LadderLengths() const;
    /// Marks the active run failed ("abandoned"); call EndRun afterwards to bank.
    Status AbandonRun(double now);

    // --- going on with a run (2026-11-07: long runs must survive a crash or a quit) ----------
    /// Between arenas (route chosen, before the next arena begins): the run is written into the
    /// profile's active run as the point it goes on from, with `adapter_state` (JSON the adapter
    /// reads back: what it carries into the next arena). Saves the profile.
    Status SaveResumePoint(std::string adapter_state);
    /// No run active and the profile holds one with a resume point.
    [[nodiscard]] bool CanResumeRun() const;
    /// The profile's run again, before the arena it was saved before (route selection with its
    /// route chosen; the first arena from Idle): the character rebuilt from content (skin and
    /// perk builds), the random streams drawn anew for that arena, cooldowns ready. Returns the
    /// adapter's state saved with it. Fails when the content no longer has its character or skin.
    Result<std::string> ResumeRun();
    /// The active run's match was left (a quit, a crash's aftermath): the run stops without being
    /// settled and goes on later from its resume point (the arena it was in is played again).
    /// Fails without a resume point (the caller abandons it instead).
    Status SuspendRun(double now);
    /// Valid from Idle with a fresh run (first arena) or RouteSelection after ChooseRoute.
    Status BeginArena(double now);
    KillOutcome OnEnemyKilled(const KillEvent& kill);
    void OnPlayerDamaged(const DamageEvent& damage);
    /// Fails the run (no further arenas). Pending credits are banked at the death rate by EndRun.
    Status OnPlayerDied(double now);
    OutgoingResult ComputeOutgoingMultiplier(const HitContext& hit);
    IncomingResult ComputeIncoming(const IncomingContext& incoming);
    /// A healing item's heal: scaled by the build's heal_mult; one of at least heal_buff_fraction
    /// of max health starts the build's invulnerability and damage buff. Needs an active run.
    HealOutcome OnPlayerHealed(const HealContext& heal);
    /// The build's special (cooldown, arena limit) for the HUD.
    [[nodiscard]] SpecialState Special(double now, int slot = 0) const;
    /// Uses the build's special during an arena: NotEligible without one, while it cools down or
    /// before its arena limit allows it; a dash also starts its invulnerability. The adapter
    /// performs the move itself; `cooldown_scale` (> 0) lengthens or shortens this use's cooldown
    /// (a dash build's stronger moves recover longer). `slot` 1 is the build's special_2, with
    /// its own cooldown and arena limit.
    Status UseSpecial(double now, double cooldown_scale = 1.0, int slot = 0);
    [[nodiscard]] bool Invulnerable(double now) const { return run_.active && now < run_.invulnerable_until; }
    /// The current arena's enemy threat level (economy enemy_threat; 0 without a director or levels).
    [[nodiscard]] int ArenaThreatLevel() const { return director_ ? director_->threat_level() : 0; }
    /// Sum of queued heal fractions (capped at 1.0).
    [[nodiscard]] double PendingHeal() const;
    /// Items earned by perks since the last call (item_on_kill / item_on_arena_clear).
    std::vector<ItemGrant> ConsumeItemGrants();
    std::vector<HealRequest> ConsumeHealRequests();

    [[nodiscard]] bool IsArenaComplete() const;
    [[nodiscard]] int RemainingEnemies() const;
    /// Requires ArenaActive and IsArenaComplete(). Grades the arena, awards clear supplies,
    /// accrues credits, generates the reward offer (none after the final arena) and moves to ArenaResults.
    Result<ArenaResult> CompleteArena(double now);
    /// ArenaResults -> RewardSelection, or RunComplete after the final arena.
    Status ContinueToRewards();

    [[nodiscard]] const std::vector<RewardChoice>& RewardOffer() const { return run_.reward_offer; }
    /// Spends a reroll token to regenerate the current offer (RewardSelection, before choosing).
    Status RerollReward();
    /// Exactly once per arena; a second call fails with AlreadyChosen. Moves to Shop.
    Status ChooseReward(std::size_t index);

    [[nodiscard]] const std::vector<ShopOffer>& ShopOffers() const { return run_.shop_offers; }
    /// Validates phase, index, not-yet-bought and balance; debit and grant are all-or-nothing.
    /// A weapon the skin's build cannot use is refused (NotEligible).
    Status Purchase(std::size_t index);
    /// New shop stock for free while the skin's build has free rerolls left this visit
    /// (SkinBuild::free_shop_rerolls, reset on entering the shop); NotEligible otherwise.
    Status RerollShop();
    /// What the shop's next NEW STOCK costs: 0 while the build's free ones last, -1 when there is
    /// none (outside the shop, or no paid restock configured).
    [[nodiscard]] std::int64_t ShopRestockPrice() const;
    /// Shop -> RouteSelection (route choices are generated here).
    Status LeaveShop();

    [[nodiscard]] const std::vector<RouteChoice>& RouteChoices() const { return run_.route_choices; }
    /// Once per RouteSelection; grants the route's curse if it carries one and moves the run
    /// map to the chosen node.
    Status ChooseRoute(std::size_t index);

    /// The arenas (map keys) the next StartRun builds its run map from, and the one it starts
    /// on (-1: drawn from them). Without a pool every node's map key is -1.
    void SetArenaPool(std::vector<int> arenas, int first) {
        arena_pool_ = std::move(arenas);
        first_arena_ = first;
    }
    /// The map key of the run map node being played, or of the next one once a route is
    /// chosen (-1: none or unknown).
    [[nodiscard]] int PlannedMapKey() const;
    /// That node's guests (ROADMAP M3.3): enemy classes of other maps its arena adds to its
    /// map's own; the arena fields them when it is played on PlannedMapKey(). Empty without a plan.
    [[nodiscard]] std::vector<std::string> PlannedGuests() const;
    /// The map an arena on `stage` is fought on: its maps.json entry with the planned guests when
    /// `stage` is PlannedMapKey(), as it is otherwise (a dev pick); empty for an unknown stage.
    [[nodiscard]] std::optional<MapDef> ArenaMap(int stage) const;

    /// From RunComplete/RunFailed: banks credits into the profile exactly once per run id and
    /// saves. Calling again returns the same summary with already_banked = true.
    Result<RunSummary> EndRun();

    // --- developer commands (debug menu / tests) -----------------------------------------
    /// Grants a perk to the active run under the normal rules (character, capabilities,
    /// unlocks, stacks, exclusions); curses are allowed. Emits perk_acquired with source "dev".
    Status DevGrantPerk(std::string_view perk_id);
    /// Adds a weapon instance under the normal slot rules (see weapons.h).
    Status DevGrantWeapon(WeaponInstance weapon);
    /// Refines the run's gun of `base` (else RefineTarget's pick) one tier, as a reward would.
    Status DevRefineWeapon(const std::optional<std::string>& base);
    /// Credits supplies to the active run; each call is its own transaction (reason "dev").
    Status DevGrantSupplies(std::int64_t amount);
    /// Supplies earned (> 0) or spent (< 0) outside the run's own screens (the game's
    /// Organize/Buy screen between arenas). Each call is its own transaction; a debit larger
    /// than the balance is refused.
    Status AdjustSupplies(std::int64_t delta, std::string_view reason);
    /// The adapter found another gun in the player's hands at match start: it becomes the
    /// run's starting weapon (replacing the rolled one), so upgrades and the shop follow it.
    Status SwapStartingWeapon(std::string_view base_id);
    /// The run's guns that come back every arena (WeaponBaseDef::restocks), as game items: the
    /// adapter hands out one of each the player does not hold.
    [[nodiscard]] std::vector<std::int64_t> RestockWeaponItems() const;
    /// A gun the player gave up (sold): it leaves the run, so it is not upgraded or restocked.
    Status DropWeapon(std::int64_t game_item, std::string_view reason);
    /// Guns refined to Epic waiting for their element (the user, 2026-11-11: two offered and a
    /// random one); the adapter asks between arenas, a chest's refine at the next screen.
    [[nodiscard]] std::vector<ElementChoice> PendingElementChoices() const;
    /// Gives the gun its element: `option` 0 or 1 the offered one, -1 a random one (any element but
    /// the build's, each as likely, so the rare ones too).
    Status ChooseElement(std::string_view instance_id, int option);
    /// The elements of the run's gun `base_id` (as its hits carry them, the build's too), each with
    /// whether it plays evolved; empty when the run holds no such gun. The adapter's flamethrower
    /// stream plays them on the enemies in its flame.
    [[nodiscard]] std::vector<std::pair<Element, bool>> ElementsOf(std::string_view base_id) const;
    /// What selling `count` of `game_item` is worth in supplies: half its shop price (a gun of the
    /// run by its rarity, any other gun as a common one; an item by its catalog price per unit),
    /// 0 for anything the shop does not price. The game's Organize screen pays this, never its gold.
    [[nodiscard]] std::int64_t SaleValue(std::int64_t game_item, std::int64_t count) const;
    /// A chest was opened in the arena (adapter: a time-bonus pickup). Only while an arena is
    /// active; rolls from the "loot" stream.
    /// `held_weapon`: the base id of the gun in the player's hands (a chest's refine goes to it).
    Result<ChestReward> OpenChest(double now, std::optional<std::string> held_weapon = std::nullopt);
    /// The highest tier a gun can be refined to: Exotic once economy refine.exotic_unlocked_by is
    /// beaten (and the content has an exotic tier), else Legendary.
    [[nodiscard]] Rarity MaxRefineRarity() const;
    /// The gun a refine goes to: `preferred` (a base id) if it can go up, else the highest tier,
    /// then level, then the newest; nullptr when none can.
    [[nodiscard]] const WeaponInstance* RefineTarget(const std::optional<std::string>& preferred) const;

    // ---- Director (ROADMAP M1): the adapter tells the map, the core decides the spawns.
    /// The map (engine stage) the next and current arenas are played on.
    void SetMapStage(int stage) { map_stage_ = stage; }
    /// True while the current arena is run by the director (map known, capability present).
    [[nodiscard]] bool DirectorActive() const { return director_.has_value(); }
    /// Spawns the adapter should perform now, from points `prefer` takes when there are any
    /// (Director::Tick).
    std::vector<SpawnRequest> DirectorTick(double now, const std::optional<std::array<double, 3>>& player,
                                           const Director::PointFilter& prefer = {});
    void DirectorSpawned(std::uint64_t ticket, const std::string& unit_id);
    void DirectorSpawnFailed(std::uint64_t ticket);
    /// The adapter cannot create this class on the stage (its archive is not loaded): the
    /// director stops picking it this arena (Director::Unavailable).
    void DirectorClassUnavailable(const std::string& unit_class);
    /// A director unit cannot reach the player: where to move it, a point `accept` takes when
    /// given (Director::Relocate).
    std::optional<std::array<double, 3>> DirectorRelocate(const std::string& unit_id,
                                                          const std::optional<std::array<double, 3>>& player,
                                                          const Director::PointFilter& accept = {}, bool nearest = false);
    [[nodiscard]] const Director* director() const { return director_ ? &*director_ : nullptr; }

    [[nodiscard]] RunSnapshot Snapshot(double now) const;
    [[nodiscard]] Phase phase() const { return run_.phase; }
    [[nodiscard]] bool HasActiveRun() const { return run_.active; }
    [[nodiscard]] const RunState& state() const { return run_; }
    [[nodiscard]] const std::optional<RunSummary>& last_summary() const { return last_summary_; }

private:
    // The ladder (EconomyConfig::Ladder): what the profile has beaten and opened.
    [[nodiscard]] bool Beaten(std::string_view key) const;
    // The rarity remake's refines: a refine reward added to the offer; one for the shop.
    void AddRefineReward(bool sure);
    void AddRefineOffer();
    Status ApplyRefine(const std::string& instance_id, const WeaponInstance& refined, std::string_view source);
    [[nodiscard]] bool LengthOpen(const EconomyConfig::Ladder::Length& length) const;
    /// The finale's arena can follow this length's boss (a run of it may earn it).
    [[nodiscard]] bool FinalePossible(const EconomyConfig::Ladder::Length& length) const;
    /// This run has earned the finale: it beat a hard boss of every length in finale.needs_hard.
    [[nodiscard]] bool FinaleEarned() const;
    /// A length's boss arena choices: its easy boss, then its hard ones once the easy one is beaten.
    [[nodiscard]] std::vector<int> BossChoices(const EconomyConfig::Ladder::Length& length) const;
    /// What these first-time ladder keys opened (the run's results).
    [[nodiscard]] std::vector<LadderUnlock> LadderUnlocksFor(const std::vector<std::string>& fresh_keys) const;
    [[nodiscard]] double BossHpMult(int map_key) const;
    /// A boss arena cleared: its ladder key, and its class, into the profile's bosses beaten.
    void RecordBossBeaten(int map_key);
    struct Streams {
        Rng reward;
        Rng route;
        Rng loot;
        Rng shop;
        Rng combat;
        Rng loadout;
        Rng director;
    };

    Event MakeEvent(EventType type, EventLevel level = EventLevel::Info) const;
    void Emit(Event event) const;
    void EmitError(std::string_view operation, const Status& status) const;
    Status Fail(std::string_view operation, ErrorCode code, std::string message) const;

    Status SaveProfile(std::string_view reason);
    const CharacterDef* Character() const;
    const RouteDef* Route() const;
    ArenaMetrics* CurrentArena();
    const ArenaMetrics* CurrentArena() const;
    bool WeaponIdMode() const;
    /// The guns the inventory says are gone (InventoryPort::Holds) leave the run, but restocking
    /// ones; done before rewards and shop stock are drawn, so none is offered for them.
    void PruneLostWeapons();
    // The rarity remake (combat.cpp): a hit's element and effect procs (`dealt`: the hit's damage
    // after the multiplier); a kill's (blasts, overkill, refill, Bloodthirst's heal).
    void AddHitProcs(const WeaponInstance& weapon, double dealt, bool execute, std::vector<WeaponProc>& procs);
    void AddKillProcs(const KillEvent& kill, const WeaponInstance* weapon, KillOutcome& out);
    /// A gun's elements with the build's (gun_elements, the arena's element), each once.
    /// The gun's elements, each with whether it plays evolved: its own (evolved on a Legendary
    /// gun), then the build's (gun_elements, the arena's element), each once.
    [[nodiscard]] std::vector<std::pair<Element, bool>> GunElements(const WeaponInstance& weapon) const;
    /// The build's own elements (gun_elements): never a gun's own.
    [[nodiscard]] std::vector<Element> BuildElements() const;
    const WeaponInstance* ApplicableWeapon(const std::optional<std::string>& weapon_id,
                                           const std::optional<bool>& melee) const;
    std::string NextInstanceId();
    OfferContext MakeOfferContext(double rarity_shift) const;
    PerkGateInput MakeGateInput(CursePolicy curses) const;

    /// build_mult: scale by the skin build's supplies_mult (supplies earned in an arena; rewards
    /// are scaled when they are rolled, external changes never).
    bool CreditSupplies(const std::string& tx, std::int64_t amount, std::string_view reason, bool build_mult = true);
    /// A player kill's build effects: a melee-kill stat, a key drop.
    void ApplyBuildKill(const KillEvent& kill, KillOutcome& out);
    TxResult DebitSupplies(const std::string& tx, std::int64_t amount, std::string_view reason);
    Status GrantPerk(std::string_view perk_id, std::string_view source, CursePolicy curses,
                     bool ignore_unlock);
    /// Queues an inventory item (loadouts.json equipment) for the adapter; needs the inventory capability.
    Status GrantItem(std::string_view equipment_id, int count, std::string_view source);
    Status PlaceWeaponItem(const WeaponInstance& weapon, std::string_view source);
    void GrantWeapon(WeaponInstance weapon, std::string_view source);

    int ActiveKills(double now, double window) const;
    double KillStreakBonus(double now, int* kills_in_longest_window) const;
    int CheatDeathCharges() const;
    void UpdateCheckpoint();
    Result<RunSummary> SettleCheckpoint(const ActiveRunCheckpoint& checkpoint,
                                        std::string_view reason);
    void EnterShop();
    void EnterRouteSelection();
    void CompleteRun();
    void FailRun(double now, std::string_view reason);

    std::shared_ptr<const Content> content_;
    Profile profile_;
    Capabilities caps_;
    InventoryPort* inventory_ = nullptr;
    int map_stage_ = -1;
    std::vector<int> arena_pool_;
    int first_arena_ = -1;
    std::optional<Director> director_;
    std::optional<StringSet> placeable_weapons_;
    StringSet installed_mod_skins_;
    int dev_supply_grants_ = 0;
    int external_supply_changes_ = 0;
    IEventSink* sink_ = nullptr;
    ProfileStore* store_ = nullptr;
    int damage_sample_every_ = 50;

    RunState run_;
    std::optional<Streams> streams_;
    std::optional<RunSummary> last_summary_;
};

}  // namespace rm
