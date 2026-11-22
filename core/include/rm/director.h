#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rm/common.h"
#include "rm/rng.h"

namespace rm {

/// Named spawn settings the adapter writes into the enemy's descriptor (weapon, plaga type,
/// mask, ...). Names and meaning belong to the adapter; the core only picks one.
using SpawnFields = std::vector<std::pair<std::string, double>>;

/// One way the game itself spawned this enemy type (surveyed), picked by weight.
struct EnemyProfile {
    double weight = 1.0;
    SpawnFields fields;
};

/// One enemy type a map can host (its model is loaded there). `profiles` are the variants
/// the game used there; without them, `arms` are the weapon ids seen (empty = the default).
struct MapEnemy {
    std::string unit_class;          // engine class, e.g. "uEm10"
    std::vector<std::int64_t> arms;  // descriptor weapon ids
    std::vector<EnemyProfile> profiles;
    int min_tier = 1;                // first route tier that may use it
    double weight = 1.0;
    /// The stage's special enemy (the big ones the game itself spawns one at a time): never a
    /// normal pick; once `MapDef::special_after` of the budget is dead, kept at `max_alive`
    /// (default 1) until the budget runs out. Its kills count like any other.
    bool special = false;
    int max_alive = 0;
    /// A guest (ROADMAP M3.3, rm::WithGuests): the map key of the map whose roster entry this is;
    /// -1 for the map's own enemies.
    int home = -1;
};

/// Enemies a boss calls in (MapBoss phases): `count` of `unit_class`.
struct BossAdd {
    std::string unit_class;
    int count = 1;
};
/// A boss's second wind (the user, 2026-11-22, Midnight Wesker: "I want him to kneel and that it looks
/// like he's dead, then he should get back up and play some voiceline and call in some enemies"): the
/// first death goes to phases[0] instead, the next to phases[1], and the one after the last is real.
/// The boss stays down `down_seconds` (it cannot be hurt), then gets up with `hp_fraction` of its
/// health and its adds come. The actions are the adapter's (engine action names; empty: none).
struct BossPhase {
    double down_seconds = 6.0;
    double hp_fraction = 1.0;
    std::string down_action;  // played as it goes down ("looks dead")
    std::string rise_action;  // played as it gets up (a taunt with its voice line)
    std::vector<BossAdd> adds;
    /// It takes its second form while down (the user, 2026-11-22, Uroboros Wesker: "when he dies we bump
    /// it to a second phase where he enters that natural second phase"): the down lasts while its down
    /// action plays, at least down_seconds; then the adapter gives the form (Uroboros Wesker: his second
    /// form's moves on his own body; the story's StageChange left him unhittable).
    bool transform = false;
};
/// A boss (ROADMAP M6): on its map the director spawns only the boss, and the arena clears when
/// it dies. Runs end on a boss arena when one is in their pool.
struct MapBoss {
    std::string unit_class;  // engine class, e.g. "uEm40"
    std::string name;        // shown in the route menu and on the HUD
    int hp = 0;              // > 0: its health once spawned (the game's own suits story weapons)
    std::string article = "the";  // before its name in a sentence ("the Uroboros"; "" for a person)
    /// Where the story sets it (its set's position): it appears there, not at a map point
    /// (2026-11-08, the user: "spawn ... where they are supposed to be").
    std::optional<std::array<double, 3>> spawn;
    std::optional<double> facing;  // its set's turn about Y (radians), with a spawn
    /// With a spawn: it appears once the player comes within `trigger` of it (0: at once), or
    /// `trigger_wait` seconds after the director's first tick, whichever comes first (0: no limit).
    double trigger = 0.0;
    double trigger_wait = 0.0;
    /// With a spawn, instead of `trigger`: it appears once the player is inside this box (min x, y,
    /// z, max x, y, z): the story's own trigger for it (the Uroboros' s103 "uro shtuugen").
    std::optional<std::array<double, 6>> trigger_box;
    std::vector<BossPhase> phases;  // its deaths before the real one (BossPhase)
    /// How the adapter makes it harder (the user's picks, 2026-11-22, the boss tuning board): numbers
    /// by name, each checked against kBossTuningKeys. Uroboros Wesker: second_form_at (his health
    /// share when his second form starts), body_damage (the share of a body hit he takes), rage_every
    /// and rage_seconds, specials_every. Jill: attack_chance (6 chances, far to near), fire_cooldown_max
    /// (seconds), melee_every and melee_range, wide_fire_every, flinch_only_explosives, dodge_chance
    /// and dodge_aim_seconds.
    StringMap<std::vector<double>> tuning;
    /// A tuning value (its first number), or `fallback` when not set.
    [[nodiscard]] double Tuning(std::string_view key, double fallback = 0.0) const {
        const auto it = tuning.find(std::string(key));
        return it != tuning.end() && !it->second.empty() ? it->second.front() : fallback;
    }
    /// Its name in a sentence: "the Uroboros", "Wesker".
    [[nodiscard]] std::string Phrase() const { return article.empty() ? name : article + " " + name; }
};

/// A map the director can run an arena on (configs/maps.json).
struct MapDef {
    std::string id;
    std::string name;
    /// Mercenaries maps: the stage index 0..7; campaign areas: their stage number (100 and up),
    /// which the adapter's host loads in place of a Mercenaries stage.
    int stage = -1;
    bool rotation = true;  // runs may pick it (false keeps it for tests)
    /// Where the player starts (campaign areas: the game picks one of the area's entrances at
    /// random, so the adapter moves the player here when an arena begins far from it).
    std::optional<std::array<double, 3>> start;
    /// Where player 2 starts in a co-op run (the map editor's; none: at `start`, by player 1).
    std::optional<std::array<double, 3>> start2;
    std::vector<std::array<double, 3>> points;  // spawn positions known to be on walkable ground
    /// Points (of `points`, within half a metre) only the small enemies use: a big one (a special,
    /// a boss's add that is a special or has its own health) is never spawned or moved there, as
    /// long as another point will do (the user, 2026-11-22, the Engine Room's lower floor: "there's
    /// a stair, minibosses can't jump it or use the stairs so they get blocked there").
    std::vector<std::array<double, 3>> small_only_points;
    std::vector<std::array<double, 3>> chests;  // chest positions (where the game puts its time bonuses)
    std::vector<MapEnemy> roster;
    /// Archives (Image\Archive\<name>) the adapter loads with the arena's enemies: everything the
    /// roster's own stage loads for them (weapons they carry, plagas, effects), for maps whose
    /// stage does not load them itself (campaign areas).
    std::vector<std::string> preload;
    /// A campaign area's own object layouts holding its ladders (`s500_IDf01_Ins`): a Mercenaries
    /// match skips them, so the adapter files them and builds their ladders.
    std::vector<std::string> layouts;
    /// Objects of the area not built (set record names or their start, any case: the Engine
    /// Room's `om0955_ido00`): a door its campaign opens with a lever that a run never pulls.
    std::vector<std::string> hide_models;
    int max_alive = 8;
    double min_player_distance = 800.0;  // engine units; points closer than this are avoided
    double max_player_distance = 0.0;    // > 0: points farther than this are avoided (large areas)
    double special_after = 0.25;         // share of the budget dead before specials join
    int max_specials = 1;                // specials alive at once on tier 1 (+1 per tier above)
    std::optional<MapBoss> boss;         // a boss arena (the roster is not used)
};

/// How dangerous an enemy is (ROADMAP M3.4): 1 a Majini with a blade or bare hands, 2 big blades,
/// fire, bows and handguns, 3 explosives, stun rods, shotguns and the big specials, 4 machine
/// guns, rockets, gatlings and the chainsaw. A spawn's threat is the higher of its class's and
/// its weapon's (economy.json enemy_threat, written by scripts/write_threats.py).
struct ThreatTable {
    int none = 1;      // no weapon (no arm, or the arm 0x70100)
    int fallback = 2;  // a class or weapon not listed
    std::unordered_map<int, int> weapons;  // by weapon class: the arm 0x701NN's NN
    StringMap<int> classes;

    [[nodiscard]] int Weapon(std::int64_t arm) const;
    [[nodiscard]] int Class(const std::string& unit_class) const;
    [[nodiscard]] int Of(const MapEnemy& enemy, std::int64_t arm) const;
    /// Each variant of an entry with its weight: its profiles, else its arms, else the class alone.
    [[nodiscard]] std::vector<std::pair<int, double>> Variants(const MapEnemy& enemy) const;
    [[nodiscard]] int Lowest(const MapEnemy& enemy) const;
    /// The mildest enemy a map's normal (not special) roster can field; 0 for a boss map.
    [[nodiscard]] int Lowest(const MapDef& map) const;
};

/// The weapon a profile gives (its "arm" field), -1 without one.
std::int64_t ProfileArm(const EnemyProfile& profile);

/// What an arena may field (the run builds it from economy enemy_threat).
struct ArenaThreat {
    int level = 0;          // 0: no limit (every enemy, the map's max_alive, specials by tier)
    int max_alive = 0;      // > 0: at most this many alive at once (within the map's own)
    int specials_from = 1;  // the stage's specials join from this level on
    ThreatTable table;
    double boss_hp_mult = 1.0;  // a boss's health times this (its ladder length's, EconomyConfig::Ladder)
};

/// What the adapter must create now. `ticket` identifies the request until it is answered.
struct SpawnRequest {
    std::uint64_t ticket = 0;
    std::string unit_class;
    std::int64_t arm = -1;
    std::array<double, 3> pos{};
    SpawnFields fields;  // from the picked profile (empty = adapter defaults)
    int hp = 0;          // > 0: the unit's health once spawned (bosses, a boss's add that is a boss itself)
    bool boss = false;
    bool add = false;    // a boss's add (Director::AddWave): not part of the budget
    std::optional<double> facing;  // a boss's story turn (MapBoss::facing); else the adapter's choice
    int seat = -1;       // the player it was placed for (PlayerSpot::seat); -1: no player known
};

/// A standing player the spawns are shared between (co-op: each standing one; solo: the one).
struct PlayerSpot {
    int seat = 0;
    std::array<double, 3> pos{};
};

/// Runs one arena: `budget` enemies in total, at most `max_alive` at once, refilled as they
/// die. Only units it spawned count. Deterministic for a given seed. On a boss map the boss is
/// the only unit (budget 1).
class Director {
public:
    /// A spawn point the adapter takes (e.g. out of the player's sight, open to him).
    using PointFilter = std::function<bool(const std::array<double, 3>&)>;

    /// `threat` (level > 0) holds the arena to enemies at or under its level: entries and
    /// variants above it are left out (a map with nothing that mild fields its mildest), at most
    /// threat.max_alive are alive at once, and specials join only from threat.specials_from.
    Director(const MapDef& map, int budget, int tier, std::uint64_t seed, ArenaThreat threat = {});

    /// Requests to spawn now. `player` (when known) steers spawns away from the player; points
    /// `prefer` takes come first when given (any point when it takes none).
    std::vector<SpawnRequest> Tick(double now, const std::optional<std::array<double, 3>>& player,
                                   const PointFilter& prefer = {});
    /// The same with every standing player: each spawn goes to the player with the fewest of our
    /// units (alive or asked for), the other's turn when even, at a point picked for that player and
    /// not too close to any other (the user, 2026-11-22, co-op: "they have a tendency for rushing 1
    /// player and letting the other with nothing, they should spawn evenly"). A waiting boss
    /// appears once any player comes near.
    std::vector<SpawnRequest> Tick(double now, const std::vector<PlayerSpot>& players, const PointFilter& prefer = {});
    /// Our units (alive or asked for, adds too) placed for that seat.
    [[nodiscard]] int ForSeat(int seat) const;
    /// The adapter created the request's unit; `unit_id` is how its death will be reported.
    void Spawned(std::uint64_t ticket, const std::string& unit_id);
    /// The adapter could not create it; the slot goes back to the budget (the point is
    /// avoided after repeated failures).
    void SpawnFailed(std::uint64_t ticket);
    /// The adapter cannot create this class at all here (its archive is not loaded): it is not
    /// picked again this arena. Call SpawnFailed for the request as well.
    void Unavailable(const std::string& unit_class);
    /// A unit died or vanished. True (once) when it was one of ours (an add's death too: it is
    /// ours, but it does not count toward the budget; ask IsAdd first).
    bool Died(const std::string& unit_id);
    /// One enemy a boss calls in: its roster entry (its home map's) and, > 0, its health before the
    /// arena's boss_hp_mult (another map's boss: that boss's health).
    struct AddSpec {
        MapEnemy enemy;
        int hp = 0;
    };
    /// The boss calls in `adds` from `at` on (a BossPhase): they come from map points like any spawn
    /// (outside the alive cap), but are not part of the budget, so the arena still clears on the boss.
    void AddWave(std::vector<AddSpec> adds, double at);
    /// True while `unit_id` is a live add.
    [[nodiscard]] bool IsAdd(const std::string& unit_id) const { return adds_alive_.count(unit_id) > 0; }
    [[nodiscard]] int adds_alive() const { return static_cast<int>(adds_alive_.size()); }
    [[nodiscard]] int adds_waiting() const { return static_cast<int>(adds_queued_.size() + adds_pending_.size()); }
    /// The boss's health in this arena (its MapBoss hp times the arena's boss_hp_mult; 0 without one).
    [[nodiscard]] int BossHp() const;
    /// One of our units cannot reach the player (behind a door, on a ledge): it gets another
    /// point, within reach of the player when one is known and one `accept` takes (when given),
    /// and its own point is not used again this arena. Empty: no such point (nothing changes),
    /// or not our unit. The adapter moves it there. `nearest`: the point closest to the player that
    /// is not too close, and the unit's own point stays usable (a pull, not a bad point).
    std::optional<std::array<double, 3>> Relocate(const std::string& unit_id,
                                                  const std::optional<std::array<double, 3>>& player,
                                                  const PointFilter& accept = {}, bool nearest = false);
    [[nodiscard]] int relocations() const { return relocations_; }
    /// The map editor changed this map's points during the arena: the spawns from now on use
    /// `points` (empty: unchanged). Units already out keep where they are; the points' failures
    /// and the points units came from are forgotten.
    void ReplacePoints(std::vector<std::array<double, 3>> points);
    /// Picks `count` distinct chest positions of the map for this arena (all of them when the
    /// map has fewer); they stay available as chests().
    const std::vector<std::array<double, 3>>& PickChests(int count);
    [[nodiscard]] const std::vector<std::array<double, 3>>& chests() const { return chests_; }

    [[nodiscard]] int budget() const { return budget_; }
    [[nodiscard]] int dead() const { return dead_; }
    [[nodiscard]] int alive() const { return static_cast<int>(alive_.size()); }
    [[nodiscard]] int pending() const { return static_cast<int>(pending_.size()); }
    /// A boss with a story spot that waits for the player to come near (as of the last Tick).
    [[nodiscard]] bool WaitingForPlayer() const { return boss_waiting_; }
    [[nodiscard]] int remaining() const { return budget_ - dead_; }
    [[nodiscard]] bool complete() const { return dead_ >= budget_; }
    [[nodiscard]] const MapDef& map() const { return map_; }
    /// The arena's threat level (0: none), and the level spawns are held to (the map's mildest
    /// when that is above the arena's).
    [[nodiscard]] int threat_level() const { return threat_.level; }
    [[nodiscard]] int pick_level() const { return pick_level_; }

private:
    const MapEnemy* PickEnemy();
    const MapEnemy* PickSpecial();
    [[nodiscard]] double Share(const MapEnemy& enemy) const;  // the part of an entry this arena may field
    [[nodiscard]] bool Fits(int threat) const { return pick_level_ <= 0 || threat <= pick_level_; }
    [[nodiscard]] int Active(const MapEnemy* enemy) const;  // alive + pending of that roster entry
    int PickPoint(const std::optional<std::array<double, 3>>& player, const PointFilter& accept = {}, bool nearest = false);
    [[nodiscard]] std::size_t PickSeat(const std::vector<PlayerSpot>& players) const;  // an index into players
    // `big`: a special or a boss's big add, kept off the map's small_only_points while another will do.
    int PickPointFor(const std::vector<PlayerSpot>& players, std::size_t who, const PointFilter& prefer, bool big = false);
    [[nodiscard]] bool SmallOnly(const std::array<double, 3>& p) const;
    [[nodiscard]] bool BossWaits(double now, const std::optional<std::array<double, 3>>& player) const;

    MapDef map_;
    int budget_ = 0;
    int tier_ = 1;
    ArenaThreat threat_;
    int pick_level_ = 0;
    Rng rng_;
    std::uint64_t next_ticket_ = 1;
    int requested_ = 0;  // spawns asked for and not failed (alive + pending + dead)
    int dead_ = 0;
    double next_spawn_time_ = 0.0;
    int last_point_ = -1;
    double last_now_ = 0.0;
    double first_tick_ = -1.0;  // the first Tick's time (a boss's trigger_wait counts from it)
    bool boss_waiting_ = false;
    StringMap<std::uint64_t> alive_;                 // unit id -> ticket
    std::vector<std::pair<std::uint64_t, int>> pending_;  // ticket -> point index
    std::vector<int> point_failures_;
    std::unordered_map<std::uint64_t, const MapEnemy*> ticket_enemy_;  // pending and alive tickets
    std::unordered_map<std::uint64_t, int> ticket_point_;               // alive tickets: their point
    std::unordered_map<std::uint64_t, int> ticket_seat_;                // pending and alive tickets (adds too): their player
    int last_seat_ = -1;                                                // the seat the last spawn went to
    int relocations_ = 0;
    std::vector<std::array<double, 3>> chests_;
    StringSet unavailable_;  // classes the adapter cannot create on this stage
    struct QueuedAdd {
        AddSpec spec;
        double at = 0.0;
        int tries = 0;
    };
    std::vector<QueuedAdd> adds_queued_;                           // not asked for yet
    std::unordered_map<std::uint64_t, QueuedAdd> adds_pending_;   // ticket -> the add asked for
    StringMap<std::uint64_t> adds_alive_;                          // unit id -> ticket
};

}  // namespace rm
