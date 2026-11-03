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

struct KillEvent {
    double now = 0.0;
    bool by_player = false;
    std::optional<std::string> weapon_id;
    std::optional<bool> was_melee;
    std::optional<std::string> enemy_id;
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
};

struct OutgoingResult {
    double multiplier = 1.0;  // clamped; multiply the engine's damage by this
    bool crit = false;
    OutgoingBreakdown breakdown;
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
};

/// What a chest held (all already applied: supplies credited, items in the inventory).
struct ChestReward {
    std::int64_t supplies = 0;
    std::string item_id;       // catalog item placed, if any
    std::string item_name;
    int item_count = 0;
    std::string weapon_id;     // weapon placed, if any
    std::string weapon_name;
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
    /// Marks the active run failed ("abandoned"); call EndRun afterwards to bank.
    Status AbandonRun(double now);
    /// Valid from Idle with a fresh run (first arena) or RouteSelection after ChooseRoute.
    Status BeginArena(double now);
    KillOutcome OnEnemyKilled(const KillEvent& kill);
    void OnPlayerDamaged(const DamageEvent& damage);
    /// Fails the run (no further arenas). Pending credits are banked at the death rate by EndRun.
    Status OnPlayerDied(double now);
    OutgoingResult ComputeOutgoingMultiplier(const HitContext& hit);
    IncomingResult ComputeIncoming(const IncomingContext& incoming);
    /// Sum of queued heal fractions (capped at 1.0).
    [[nodiscard]] double PendingHeal() const;
    /// Items earned by perks since the last call (item_on_kill / item_on_arena_clear).
    std::vector<ItemGrant> ConsumeItemGrants();
    std::vector<HealRequest> ConsumeHealRequests();

    [[nodiscard]] bool IsArenaComplete() const;
    [[nodiscard]] int RemainingEnemies() const;
    /// Requires ArenaActive and IsArenaComplete(). Grades the arena, awards clear supplies,
    /// accrues credits, generates the reward offer and moves to ArenaResults.
    Result<ArenaResult> CompleteArena(double now);
    /// ArenaResults -> RewardSelection.
    Status ContinueToRewards();

    [[nodiscard]] const std::vector<RewardChoice>& RewardOffer() const { return run_.reward_offer; }
    /// Spends a reroll token to regenerate the current offer (RewardSelection, before choosing).
    Status RerollReward();
    /// Exactly once per arena; a second call fails with AlreadyChosen. Moves to Shop, or to
    /// RunComplete after the final arena.
    Status ChooseReward(std::size_t index);

    [[nodiscard]] const std::vector<ShopOffer>& ShopOffers() const { return run_.shop_offers; }
    /// Validates phase, index, not-yet-bought and balance; debit and grant are all-or-nothing.
    /// A weapon the skin's build cannot use is refused (NotEligible).
    Status Purchase(std::size_t index);
    /// New shop stock for free while the skin's build has free rerolls left this visit
    /// (SkinBuild::free_shop_rerolls, reset on entering the shop); NotEligible otherwise.
    Status RerollShop();
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

    /// From RunComplete/RunFailed: banks credits into the profile exactly once per run id and
    /// saves. Calling again returns the same summary with already_banked = true.
    Result<RunSummary> EndRun();

    // --- developer commands (debug menu / tests) -----------------------------------------
    /// Grants a perk to the active run under the normal rules (character, capabilities,
    /// unlocks, stacks, exclusions); curses are allowed. Emits perk_acquired with source "dev".
    Status DevGrantPerk(std::string_view perk_id);
    /// Adds a weapon instance under the normal slot rules (see weapons.h).
    Status DevGrantWeapon(WeaponInstance weapon);
    /// Credits supplies to the active run; each call is its own transaction (reason "dev").
    Status DevGrantSupplies(std::int64_t amount);
    /// Supplies earned (> 0) or spent (< 0) outside the run's own screens (the game's
    /// Organize/Buy screen between arenas). Each call is its own transaction; a debit larger
    /// than the balance is refused.
    Status AdjustSupplies(std::int64_t delta, std::string_view reason);
    /// The adapter found another gun in the player's hands at match start: it becomes the
    /// run's starting weapon (replacing the rolled one), so upgrades and the shop follow it.
    Status SwapStartingWeapon(std::string_view base_id);
    /// A chest was opened in the arena (adapter: a time-bonus pickup). Only while an arena is
    /// active; rolls from the "loot" stream.
    Result<ChestReward> OpenChest(double now);

    // ---- Director (ROADMAP M1): the adapter tells the map, the core decides the spawns.
    /// The map (engine stage) the next and current arenas are played on.
    void SetMapStage(int stage) { map_stage_ = stage; }
    /// True while the current arena is run by the director (map known, capability present).
    [[nodiscard]] bool DirectorActive() const { return director_.has_value(); }
    /// Spawns the adapter should perform now.
    std::vector<SpawnRequest> DirectorTick(double now, const std::optional<std::array<double, 3>>& player);
    void DirectorSpawned(std::uint64_t ticket, const std::string& unit_id);
    void DirectorSpawnFailed(std::uint64_t ticket);
    [[nodiscard]] const Director* director() const { return director_ ? &*director_ : nullptr; }

    [[nodiscard]] RunSnapshot Snapshot(double now) const;
    [[nodiscard]] Phase phase() const { return run_.phase; }
    [[nodiscard]] bool HasActiveRun() const { return run_.active; }
    [[nodiscard]] const RunState& state() const { return run_; }
    [[nodiscard]] const std::optional<RunSummary>& last_summary() const { return last_summary_; }

private:
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
    const WeaponInstance* ApplicableWeapon(const std::optional<std::string>& weapon_id,
                                           const std::optional<bool>& melee) const;
    std::string NextInstanceId();
    OfferContext MakeOfferContext(double rarity_shift) const;
    PerkGateInput MakeGateInput(CursePolicy curses) const;

    /// build_mult: scale by the skin build's supplies_mult (supplies earned in an arena; rewards
    /// are scaled when they are rolled, external changes never).
    bool CreditSupplies(const std::string& tx, std::int64_t amount, std::string_view reason, bool build_mult = true);
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
