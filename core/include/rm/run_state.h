#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rm/characters.h"
#include "rm/common.h"
#include "rm/economy.h"
#include "rm/rewards.h"
#include "rm/routes.h"
#include "rm/shop.h"
#include "rm/weapons.h"

namespace rm {

/// Run phase state machine:
///   Idle --StartRun--> Idle(run active) --BeginArena--> ArenaActive --CompleteArena-->
///   ArenaResults --ContinueToRewards--> RewardSelection --ChooseReward-->
///   Shop --LeaveShop--> RouteSelection --ChooseRoute, BeginArena--> ArenaActive ...
///   The final arena offers no reward: its ContinueToRewards goes to RunComplete. OnPlayerDied/AbandonRun: RunFailed.
///   EndRun (from RunComplete/RunFailed) banks credits and returns to Idle (no run active).
enum class Phase {
    Idle,
    ArenaActive,
    ArenaResults,
    RewardSelection,
    Shop,
    RouteSelection,
    RunComplete,
    RunFailed,
};
std::string_view PhaseName(Phase p);

struct KillRecord {
    double time = 0.0;
    double extra_seconds = 0.0;  // bloodhound-style window extensions
};

/// Heal the adapter should apply, as a fraction of the player's max hp.
struct HealRequest {
    double fraction_of_max = 0.0;
    std::string source;  // perk id or "shop"
};

/// What a run starts with, rolled from the loadout point budget (see LoadoutConfig).
struct LoadoutItem {
    std::string id;
    std::string name;
    int count = 1;
    std::int64_t game_item = -1;  // the adapter's inventory item id, -1 if none
};
struct Loadout {
    std::string weapon_id;
    std::vector<LoadoutItem> items;
    std::int64_t budget = 0;
    std::int64_t spent = 0;
};

/// An item a perk earned; the adapter puts it in the game inventory (ConsumeItemGrants).
struct ItemGrant {
    std::int64_t game_item = -1;
    int count = 1;
    std::string source;  // perk id
};

struct ArenaMetrics {
    int arena = 0;  // 1-based
    std::string route_id;
    int budget = 0;
    double start_time = 0.0;
    double end_time = 0.0;
    int player_kills = 0;
    int budget_kills = 0;  // kills that reduced the budget (player kills, plus others if configured)
    int non_player_kills = 0;
    int duplicate_kills = 0;
    int damage_events = 0;
    double damage_taken = 0.0;
    double hp_max_seen = 0.0;
    int deaths = 0;
    std::int64_t supplies_awarded = 0;
    std::int64_t credits_accrued = 0;
    int cheat_death_used = 0;
    bool completed = false;
};

struct ArenaResult {
    int arena = 0;
    std::string route_id;
    double time_seconds = 0.0;
    int kills = 0;
    double damage_taken = 0.0;
    bool no_damage = true;
    int deaths = 0;
    Rating rating = Rating::C;
    std::int64_t supplies_awarded = 0;
    std::int64_t credits_accrued = 0;
    std::string arena_name;  // the maps.json name of the map it was played on, empty when unknown
    bool boss = false;       // a boss arena
};

/// What a run's first win over a ladder boss opened (EconomyConfig::Ladder), for its results.
struct LadderUnlock {
    enum class Kind {
        HardBoss,  // the length's hard boss is now a choice at its arena
        Length,    // a length opened
        Finale,    // the length that opened can go on to the finale (its run beating the needs_hard bosses)
        Exotic,    // guns can be refined to Exotic now (economy refine.exotic_unlocked_by beaten)
    };
    Kind kind = Kind::HardBoss;
    std::string length_id;  // the length it concerns
    int arenas = 0;         // that length's arenas (without the finale)
    int boss = -1;          // the boss map it brings: the hard boss, the length's easy boss, the finale's
};

struct RunSummary {
    std::string run_id;
    std::string character_id;
    std::uint64_t seed = 0;
    bool completed = false;
    std::string end_reason;  // "complete", "died", "abandoned", "interrupted"
    int arenas_cleared = 0;
    int arena_count = 0;
    std::int64_t total_kills = 0;
    std::int64_t supplies_earned = 0;
    std::int64_t supplies_spent = 0;
    std::int64_t credits_pending = 0;
    std::int64_t credits_banked = 0;
    bool already_banked = false;  // EndRun called again / ledger already had this run
    std::vector<ArenaResult> arenas;
    StringMap<int> perks;
    // For the run's results screen.
    std::string skin_id;
    double time_seconds = 0.0;    // time spent in arenas, the one a failed run ended in included
    int previous_best_arena = 0;  // the profile's record before this run was banked
    std::vector<std::string> unlocked;  // characters and skins whose challenge this run met
    std::vector<LadderUnlock> ladder_unlocks;  // what the run's boss wins opened, in ladder order
    std::string final_arena;  // the map the run ended on (where the player died), empty when unknown
    bool endless = false;     // the endless length's run (no last arena: arena_count is its map's)
};

/// Stats a run gains as it goes (a build's melee_kill_stats): additive fractions.
struct RunStats {
    double damage = 0.0;      // outgoing damage
    double defense = 0.0;     // less incoming damage
    double max_health = 0.0;  // maximum health (the adapter scales it)
    double crit = 0.0;        // crit chance
    double move_speed = 0.0;  // movement speed (the adapter moves her that much further)
    double heal = 0.0;        // healing items heal more
    double supplies = 0.0;    // supplies gained
    int gains = 0;
    [[nodiscard]] double Get(std::string_view stat) const;
    double* Field(std::string_view stat);
};

/// The build's specials (slot 0 the special, 1 special_2): when each is usable again, the
/// cooldown its last use started (scaled per move), the arena of its last use (0 = never).
struct SpecialUse {
    double ready_at = 0.0;
    double cooldown = 0.0;
    int used_arena = 0;
    int uses = 0;
};

/// One player's own part of a run (M10, local co-op, 2026-11-17). RunState's fields of these names
/// hold the active seat's (RunController::ActivateSeat); the other seats wait in RunState::seats.
/// What is not here (the seed, the run map, the route, supplies, keys, chests, the arenas'
/// metrics) is the players' together. SwapSeat below exchanges every field listed here.
struct PlayerSeat {
    std::string character_id;
    std::string skin_id;
    CharacterDef character;
    Loadout loadout;
    std::vector<ItemGrant> item_grants;
    StringMap<int> perk_kill_counts;
    RunStats stats;
    double invulnerable_until = -1.0;
    double damage_buff = 0.0;
    double damage_buff_until = -1.0;
    std::array<SpecialUse, 2> specials{};
    StringMap<int> perks;
    std::vector<WeaponInstance> weapons;
    int reroll_tokens = 0;
    int cheat_death_used_total = 0;
    std::vector<RewardChoice> reward_offer;
    double reward_rarity_shift = 0.0;
    bool reward_refine_sure = false;
    bool reward_chosen = false;
    std::vector<ShopOffer> shop_offers;
    int free_shop_rerolls = 0;
    int shop_restocks = 0;
    int shop_generation = 0;
    std::vector<KillRecord> kill_records;
    StringMap<int> weapon_hits;
    int arena_element = -1;
    std::optional<double> last_melee_time;
    std::optional<double> player_hp;
    std::optional<double> player_hp_max;
    std::vector<HealRequest> heal_requests;
};

/// Complete temporary state of one run. Rebuilt from scratch by StartRun.
struct RunState {
    bool active = false;
    std::string run_id;
    std::uint64_t seed = 0;
    std::string character_id;
    std::string skin_id;
    std::string length_id;   // the ladder length (EconomyConfig::Ladder; empty without one)
    std::vector<std::string> bosses_beaten_new;  // ladder keys this run beat for the first time
    std::vector<std::string> ladder_beaten;      // ladder keys this run beat ("short:hard"): the finale's needs_hard
    CharacterDef character;  // the character with the skin applied
    Loadout loadout;
    std::vector<ItemGrant> item_grants;  // earned, not yet handed to the adapter
    StringMap<int> perk_kill_counts;     // counted kills per perk, for every_kills
    int chests_opened = 0;
    int keys = 0;               // chest keys held (economy keys)
    RunStats stats;
    double invulnerable_until = -1.0;  // a build's heal or dash: no damage taken until then
    double damage_buff = 0.0;          // a build's heal: extra outgoing damage until damage_buff_until
    double damage_buff_until = -1.0;
    using SpecialUse = rm::SpecialUse;
    std::array<SpecialUse, 2> specials{};
    Phase phase = Phase::Idle;
    int arena_index = 0;  // arenas begun so far; the current arena number while active
    int arena_count = 3;
    std::string route_id;
    bool route_chosen = false;
    RunMap run_map;
    int map_layer = 0;  // the run map node of the current arena, or of the next once chosen
    int map_node = 0;

    Ledger supplies;
    std::int64_t supplies_earned = 0;
    std::int64_t supplies_spent = 0;
    std::int64_t credits_pending = 0;

    StringMap<int> perks;  // perk id -> stacks
    std::vector<WeaponInstance> weapons;
    int next_weapon_serial = 1;
    int reroll_tokens = 0;
    int cheat_death_used_total = 0;

    std::vector<ArenaMetrics> arenas;
    std::vector<ArenaResult> results;

    std::vector<RewardChoice> reward_offer;
    double reward_rarity_shift = 0.0;
    bool reward_refine_sure = false;  // the arena's rating guarantees a refine among the rewards (rerolls too)
    bool reward_chosen = false;
    std::vector<ShopOffer> shop_offers;
    int free_shop_rerolls = 0;  // left in this shop visit (the skin build's)
    int shop_restocks = 0;      // NEW STOCK bought in this shop visit
    int shop_generation = 0;    // stock rerolled this visit (keeps purchase transaction ids unique)
    std::vector<RouteChoice> route_choices;

    // Combat bookkeeping (reset per arena).
    std::vector<KillRecord> kill_records;
    StringMap<int> weapon_hits;  // hits per weapon instance this run (Chain Lightning's every N-th)
    int arena_element = -1;      // the build's random element this arena (rm::Element; -1 none)
    std::optional<double> last_melee_time;
    std::optional<double> player_hp;
    std::optional<double> player_hp_max;
    StringSet seen_enemy_ids;
    std::int64_t hits_computed = 0;
    bool clamp_logged = false;  // first clamped hit per arena is always logged

    std::vector<HealRequest> heal_requests;
    std::string end_reason;

    // M10 local co-op. One player: `seats` is empty. Two or more: seats[i] holds seat i's own
    // state (PlayerSeat) while another seat is active; the active seat's entry is empty, its state
    // being in the fields above. seat_status is every seat's, whichever is active.
    struct SeatStatus {
        bool down = false;       // at 0 health this arena (back in the next one)
        bool revive = false;     // came back at an arena's start: the adapter sets its health
        bool shop_done = false;  // left this shop visit
    };
    std::vector<PlayerSeat> seats;
    std::vector<SeatStatus> seat_status;
    int active_seat = 0;
};

/// Exchanges a seat's own state (PlayerSeat's fields) between the run's fields and `seat`.
inline void SwapSeat(RunState& run, PlayerSeat& seat) {
    using std::swap;
    swap(run.character_id, seat.character_id);
    swap(run.skin_id, seat.skin_id);
    swap(run.character, seat.character);
    swap(run.loadout, seat.loadout);
    swap(run.item_grants, seat.item_grants);
    swap(run.perk_kill_counts, seat.perk_kill_counts);
    swap(run.stats, seat.stats);
    swap(run.invulnerable_until, seat.invulnerable_until);
    swap(run.damage_buff, seat.damage_buff);
    swap(run.damage_buff_until, seat.damage_buff_until);
    swap(run.specials, seat.specials);
    swap(run.perks, seat.perks);
    swap(run.weapons, seat.weapons);
    swap(run.reroll_tokens, seat.reroll_tokens);
    swap(run.cheat_death_used_total, seat.cheat_death_used_total);
    swap(run.reward_offer, seat.reward_offer);
    swap(run.reward_rarity_shift, seat.reward_rarity_shift);
    swap(run.reward_refine_sure, seat.reward_refine_sure);
    swap(run.reward_chosen, seat.reward_chosen);
    swap(run.shop_offers, seat.shop_offers);
    swap(run.free_shop_rerolls, seat.free_shop_rerolls);
    swap(run.shop_restocks, seat.shop_restocks);
    swap(run.shop_generation, seat.shop_generation);
    swap(run.kill_records, seat.kill_records);
    swap(run.weapon_hits, seat.weapon_hits);
    swap(run.arena_element, seat.arena_element);
    swap(run.last_melee_time, seat.last_melee_time);
    swap(run.player_hp, seat.player_hp);
    swap(run.player_hp_max, seat.player_hp_max);
    swap(run.heal_requests, seat.heal_requests);
}

struct PerkView {
    std::string id;
    std::string name;
    std::string category;
    std::string rarity;
    int stacks = 0;
};

struct WeaponView {
    std::string instance_id;
    std::string base_id;
    std::string name;
    std::string rarity;
    int level = 1;
    std::vector<std::string> affixes;  // DescribeAffix lines
    double level_bonus = 0.0;          // damage its level adds (0.3 = +30 percent)
    double base_mult = 1.0;            // the gun's own damage factor (weapons.json base_damage_mult)
};

struct OfferView {
    std::string kind;    // "perk" | "weapon" | "supplies" | "heal" | "reroll" | "upgrade" | "route"
    std::string id;      // perk id / weapon base id / route id / instance id
    std::string title;
    std::string detail;
    std::string rarity;
    std::int64_t price = 0;
    bool purchased = false;
    int tier = 0;       // routes: difficulty tier
    bool boss = false;  // routes: a boss arena (titled with the boss)
    bool restricted = false;  // shop weapon the skin's build cannot use (not for sale)
};

struct RunMapNodeView {
    int map_key = -1;
    std::string arena_name;  // empty when the map is unknown
    std::string route_id;
    std::string route_name;
    int tier = 0;
    std::string boss_name;  // a boss arena's boss, empty otherwise
    std::vector<int> next;
    bool current = false;  // the node of the current arena (or of the next once chosen)
    std::vector<std::string> guests;  // enemy classes of other maps it adds (ROADMAP M3.3)
    std::string guests_from;          // their home maps: "Ship Deck and Village"
};

/// Plain data for UI rendering; never references internal state.
struct RunSnapshot {
    bool run_active = false;
    Phase phase = Phase::Idle;
    std::string phase_name;
    std::string run_id;
    std::string character_id;
    std::string skin_id;
    std::string loadout;  // "M92F, Hand Grenade x2, Green Herb"
    std::string route_id;
    std::string route_name;
    std::string arena_name;  // the run map node's arena (maps.json name), empty when unknown
    std::string arena_element;  // the build's random element this arena (its content name; empty: none)
    std::vector<std::vector<RunMapNodeView>> run_map;
    int arena = 0;  // current arena number (1-based), 0 before the first
    int arena_count = 0;
    bool endless = false;  // the endless length's run: arena_count is only how far its map reaches
    int kills = 0;
    int budget = 0;
    int remaining = 0;  // enemies still needed to clear the arena
    // The arena under way (the info panel, 2026-11-22): its time so far, its route's par time, the damage
    // taken in it, and the rating it would get if it were cleared now (none outside an arena).
    double arena_seconds = 0.0;
    double par_seconds = 0.0;
    double damage_taken = 0.0;
    std::optional<Rating> rating_now;
    std::int64_t supplies = 0;
    std::int64_t credits = 0;          // profile balance
    std::int64_t credits_pending = 0;  // earned this run, banked at EndRun
    int reroll_tokens = 0;
    int cheat_death_remaining = 0;
    double kill_streak_bonus = 0.0;  // current kill-streak damage bonus (fraction)
    int kill_streak_kills = 0;       // kills inside the longest owned streak window
    double pending_heal = 0.0;
    std::vector<PerkView> perks;
    std::vector<WeaponView> weapons;
    std::optional<ArenaResult> last_result;
    bool reward_chosen = false;
    std::vector<OfferView> reward_offers;
    std::vector<OfferView> shop_offers;
    int free_shop_rerolls = 0;  // the skin build's, left in this shop visit
    std::int64_t restock_price = 0;  // the next NEW STOCK (0 free, -1 none; in the shop)
    std::vector<OfferView> route_offers;
    // Builds (M8): keys, gained stats, buffs (seconds left).
    bool keys_enabled = false;
    bool keys_needed = false;  // this run's chests need a key (not a key-exempt build)
    int keys = 0;
    RunStats stats;
    double invulnerable_left = 0.0;
    double damage_buff_left = 0.0;
    double damage_buff = 0.0;
    // M10 local co-op: the seat this snapshot is of (the active one) and the run's seats.
    int seat = 0;
    int seat_count = 1;
    bool seat_down = false;
    bool shop_done = false;  // this seat left the shop; the others are still in it
};

}  // namespace rm
