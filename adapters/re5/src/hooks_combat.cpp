#include <windows.h>

#include <MinHook.h>
#include <intrin.h>
#include <cstring>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

#include "engine.h"
#include "hooks.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5 {

namespace detail {
HookSink* g_sink = nullptr;
HookStatus g_status;
}  // namespace detail

using detail::g_sink;
using detail::g_status;

void SetHookSink(HookSink* sink) { g_sink = sink; }
void MarkHooksInactive() {
    g_status.combat = false;
    g_status.input = false;
    g_status.xinput = false;
    g_status.d3d9 = false;
}
const HookStatus& Status() { return g_status; }

namespace {

// The enemy hit currently being processed on this thread. OnHit -> ApplyDamage
// -> SubHp is a synchronous chain, so a thread-local stack ties the base
// SubHp call back to the hit (and attacker) that caused it.
struct HitScope {
    std::uintptr_t enemy = 0;
    std::uintptr_t attacker = 0;
    bool attackerIsPlayer = false;
    bool attackerIsLocal = false;
    int rawDamage = 0;
    int appliedDamage = 0;
    bool bodyHit = false;  // Uroboros Wesker: his own check refused it (not his open core), the mod let it through
    HitScope* previous = nullptr;
};
thread_local HitScope* t_hit = nullptr;

using EnemyOnHitFn = void(__thiscall*)(std::uintptr_t self, std::uintptr_t hit);
using SubHpFn = void(__thiscall*)(std::uintptr_t self, std::int16_t amount);

EnemyOnHitFn g_origEnemyOnHit = nullptr;

// Developer probe of the enemy hit check (dev "hitprobe <class>").
using HitCheckFn = std::uint32_t(__thiscall*)(std::uintptr_t self, std::uintptr_t part);
HitCheckFn g_origHitCheck = nullptr;
std::atomic<std::uintptr_t> g_hitProbe{0};

// Uroboros Wesker as a run's boss: his own check lets only his open core be hurt (the story opens
// it with the partner's help); in a run every hit of his body hurts too. His own check still runs
// first, so his gauges drain and he staggers and opens his core as he would.
using MayHurtFn = std::uint32_t(__thiscall*)(std::uintptr_t self, std::uint32_t region, std::uint32_t amount);
MayHurtFn g_origEm82MayHurt = nullptr;
std::atomic<std::uintptr_t> g_gunfireBoss{0};
std::atomic<int> g_gunfireLogged{0};
// The share of a body hit Uroboros Wesker takes (maps.json boss tuning "body_damage"; 1: all of it).
std::atomic<float> g_bossBodyShare{1.0f};
// A boss's adds that are bosses themselves (Uroboros Mkono called in by Wesker): their own refills
// are lifted as the run's boss's are (SetBossAddUnits).
constexpr int kBossAddSlots = 8;
std::atomic<std::uintptr_t> g_bossAdds[kBossAddSlots] = {};
// One of the boss's adds that is a boss itself (not the arena's boss): a hit never takes its last
// health, so the game never starts its own death (the Uroboros' is a cinematic with the story's calls).
bool IsBossAddUnit(std::uintptr_t self) {
    if (self == 0 || self == g_gunfireBoss.load(std::memory_order_relaxed)) return false;
    for (const auto& a : g_bossAdds) {
        if (a.load(std::memory_order_relaxed) == self) return true;
    }
    return false;
}
// The run's boss while he has a phase left (SetBossKeepsLastHealth; 0: none): his health never
// reaches 0. At 0 the game's own enemy death (0x00963660) runs and marks him dead, +0x18DC bit 0x80,
// which the enemy hit check (0x009E0700) turns every later hit away by: Uroboros Wesker's second form
// took nothing, the user's fights 2026-11-22 ("takes no damage, tried rockers, tried ak in normal,
// chest core, back core, nothing"; dev watch: flags 22190000 to 22190080 at the killing hit). At 1
// the mod puts him down (mod.cpp DetectEnemyDeaths: a boss counts dead at 1).
std::atomic<std::uintptr_t> g_lastHealthBoss{0};
bool KeepsLastHealth(std::uintptr_t self) {
    return self != 0 && (self == g_lastHealthBoss.load(std::memory_order_relaxed) || IsBossAddUnit(self));
}
bool IsRunBoss(std::uintptr_t self) {
    if (self == 0) return false;
    if (self == g_gunfireBoss.load(std::memory_order_relaxed)) return true;
    for (const auto& a : g_bossAdds) {
        if (a.load(std::memory_order_relaxed) == self) return true;
    }
    return false;
}

std::uint32_t __fastcall Em82MayHurtDetour(std::uintptr_t self, void*, std::uint32_t region, std::uint32_t amount) {
    const std::uint32_t own = g_origEm82MayHurt(self, region, amount);
    if ((own & 0xFFu) != 0 || self == 0 || self != g_gunfireBoss.load(std::memory_order_relaxed)) return own;
    if (g_gunfireLogged.fetch_add(1) < 3) {
        RM_LOGI("boss: %s takes a hit its own check refused (region %u)", engine::ClassName(self).c_str(), region & 0xFFFFu);
    }
    if (t_hit != nullptr && t_hit->enemy == self) t_hit->bodyHit = true;  // its damage is scaled where it is taken
    return 1;
}

// Uroboros Wesker's form switch (re5_layout.h addr::kFnEm82SetPose: thiscall (pose, apply), ret 8): with
// apply 1 and pose 1 it takes the story's giant lava model and turns off his hit shapes 3 to 5 (the
// finale the story's rockets end). His own actions call it so (0x00922060, 0x00922A50 StageChange's
// step, 0x00929B4A). For a run's boss it only ever sets his pose byte: the second form's moves on his
// own body. (Nothing hurting him in that form, 2026-11-22, was the dead mark: g_lastHealthBoss.)
using Em82SetPoseFn = void(__thiscall*)(std::uintptr_t self, int pose, int apply);
Em82SetPoseFn g_origEm82SetPose = nullptr;
std::atomic<int> g_em82PoseLogged{0};

// Uroboros Mkono (uEm41) as a run's boss: its update (0x008B6040) runs the Uroboros' (0x008AE2B0)
// and then sets its health back to its maximum (vtable +0x10C, the same call its grab kills the
// player with), so gunfire never wears it down (2026-11-08: 15000 -> 14920, then 15000 again; its
// end in the story is the flamethrower room's). The run's boss runs the update without that.
using UpdateFn = void(__fastcall*)(std::uintptr_t self, void* edx);
UpdateFn g_origEm41Update = nullptr;
std::atomic<int> g_em41Logged{0};

// The Uroboros goes under the floor and comes up nearer the players when they are out of its sight
// in other rooms (its brain's own way past doors, the user's "maybe make that he can open doors"). The
// check wants both players in those rooms; a run has one, and the second's room stays -1, so it could
// never pass. For the run's boss the second player counts as standing with the first.
using Em40CanDiveFn = std::uint32_t(__fastcall*)(std::uintptr_t self, void* edx);
Em40CanDiveFn g_origEm40CanDive = nullptr;
std::uint32_t __fastcall Em40CanDiveDetour(std::uintptr_t self, void* edx) {
    if (self == 0 || self != g_gunfireBoss.load(std::memory_order_relaxed)) return g_origEm40CanDive(self, edx);
    auto* second = reinterpret_cast<std::int32_t*>(self + re5::off::kEm40RoomPlayer2);
    const std::int32_t own = *second;
    if (own < 0) *second = *reinterpret_cast<std::int32_t*>(self + re5::off::kEm40RoomPlayer1);
    const std::uint32_t answer = g_origEm40CanDive(self, edx);
    *second = own;
    return answer;
}

// A player's execution (0x0076D9F0) starts the death camera that ends in the game over. In the campaign
// it filters by player; in Mercenaries it runs for anyone, so a chainsaw on a computer partner ended
// the run with the player unhurt (the user, 2026-11-09). Health at zero by itself never came this way:
// she just lies there and the match goes on (E22). -1 is the game's own pick of the first one down.
using DeathEventFn = void(__fastcall*)(std::uintptr_t self, void* edx, int player);
DeathEventFn g_origDeathEvent = nullptr;
std::atomic<std::uintptr_t> g_partnerUnit{0};
std::atomic<int> g_partnerDeathLogged{0};
std::atomic<bool> g_coopDeathHold{false};  // SetCoopDeathHold
std::atomic<bool> g_coopEndWanted{false};  // SetCoopMatchEndWanted
std::atomic<bool> g_deathEventPass{false};  // SetDeathEventPass (developer)

bool MarkedDead(std::uintptr_t player, int hp);

// Human players with health left (M10: a co-op match goes on while one stands).
int StandingHumans() {
    const auto snap = engine::ReadPlayers();
    int standing = 0;
    for (const auto& p : snap.players) {
        if (p.object && p.control == 0 && !MarkedDead(p.object, p.hp.hp)) ++standing;
    }
    return standing;
}

// The application's request (thiscall (flags), re5::addr::kFnAppRequest): every way the game ends a
// match on a player's death asks for the game over with kAppGameOver (twelve callers, 2026-11-17: the
// death camera is only one of them). In a co-op run it is dropped while a human player stands.
using AppRequestFn = void(__fastcall*)(std::uintptr_t app, void* edx, std::uint32_t flags);
AppRequestFn g_origAppRequest = nullptr;
constexpr std::uint32_t kAppGameOver = 0x08000000;

// The match's game-over check (re5::addr::kFnGameOverCheck) reads the game over flag each frame; in
// a co-op run it finds it cleared while a human player stands, whoever set it.
//
// It also freezes a split-screen match (game work +0x58 not 1) the moment any player is dead: it
// loops over sPlayer's four players (0x007E6220, valid 0x007E6340, dead 0x00B6B380) and asks the
// application for 0x20000000 (0x00726120) and for 0x400000 in its request word 1 (0x00726170:
// +0x1043C + 4) - every player's input stops and the HUD goes (the sBioPad lock bytes +0x600,
// +0x678, +0x6E0 follow it). It lifts them (0x0031A020, 0x0031A062) only once no player is still in
// the death sequence (0x00B6B330); a co-op run keeps the fallen one there, so the other player was
// frozen for good (2026-11-21, the user: "if any of the playes dire, be it first or seconds player,
// the other player's hud weapons and movement are disabled, basically is a softlock"; found live
// with a virtual player 2: app flags 0x00020000 -> 0x20020000 and +0x1043C/+0x10440 0x400000 at
// the fall, player 1 still at the same spot with W held). While one player is down, one stands and
// the mod is not ending the match itself, the check is not run and the freeze is taken back.
using GameOverCheckFn = std::uint32_t(__fastcall*)(std::uintptr_t self, void* edx);
GameOverCheckFn g_origGameOverCheck = nullptr;
using LockFn = void(__stdcall*)(void* cs);
constexpr std::uint32_t kAppCoopFreeze = 0x20000000;   // app flags: the split screen's fall freeze
constexpr std::size_t kAppRequestWord1 = 0x10440;      // the request words (+0x1043C + 4 x index)
constexpr std::uint32_t kAppInputLock = 0x00400000;    // request word 1: no player input

bool HumanDown();

// The freeze bits off, under the application's lock as the game writes them.
void ReleaseCoopFreeze() {
    const auto app = ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0);
    if (!app) return;
    const std::uint32_t flags = ReadOr<std::uint32_t>(app + re5::off::kAppFlags, 0);
    const std::uint32_t word = ReadOr<std::uint32_t>(app + kAppRequestWord1, 0);
    if ((flags & (kAppCoopFreeze | kAppGameOver)) == 0 && (word & kAppInputLock) == 0) return;
    const bool threaded = ReadOr<std::uint8_t>(app + re5::off::kAppThreadedFlag, 0) != 0 ||
                          ReadOr<std::uint8_t>(re5::addr::kAppForceLock, 0) != 0;
    const auto enter = ReadOr<std::uintptr_t>(re5::addr::kImportEnterCriticalSection, 0);
    const auto leave = ReadOr<std::uintptr_t>(re5::addr::kImportLeaveCriticalSection, 0);
    void* cs = reinterpret_cast<void*>(app + re5::off::kAppLock);
    if (threaded && enter && leave) reinterpret_cast<LockFn>(enter)(cs);
    SafeStore<std::uint32_t>(app + re5::off::kAppFlags, ReadOr<std::uint32_t>(app + re5::off::kAppFlags, 0) & ~(kAppCoopFreeze | kAppGameOver));
    SafeStore<std::uint32_t>(app + kAppRequestWord1, ReadOr<std::uint32_t>(app + kAppRequestWord1, 0) & ~kAppInputLock);
    if (threaded && enter && leave) reinterpret_cast<LockFn>(leave)(cs);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) {
        RM_LOGI("co-op: the game's freeze on a fall taken back (flags %08X, request word %08X): the other player fights on", flags, word);
    }
}

// uPlayer's "dead or frozen" and "in the death sequence" checks (re5::addr::kFnPlayerDeadOrFrozen,
// kFnPlayerInDeath). In a split-screen match the game freezes everyone as soon as one player is dead
// by them (the game-over check, the UI's 0x009E16A0, and more). While a co-op player who fell has a
// partner standing (as the game tick last saw it: SetCoopStanding) and the mod is not ending the
// match, the fallen one is neither to them: the other fights on. The fallen one still lies there (its
// execution event stays blocked) until the next arena; once the last one falls the checks answer
// as the game's own, and its game over ends the run.
using PlayerCheckFn = bool(__fastcall*)(std::uintptr_t self, void* edx);
PlayerCheckFn g_origDeadOrFrozen = nullptr;
PlayerCheckFn g_origInDeath = nullptr;
std::atomic<std::uintptr_t> g_standing[2];

bool HiddenFall(std::uintptr_t self) {
    if (!g_coopDeathHold.load(std::memory_order_relaxed) || g_coopEndWanted.load(std::memory_order_relaxed) || !self) return false;
    const std::uintptr_t a = g_standing[0].load(std::memory_order_relaxed);
    const std::uintptr_t b = g_standing[1].load(std::memory_order_relaxed);
    const bool partner = (a != 0 && a != self) || (b != 0 && b != self);
    if (!partner || ReadOr<std::int32_t>(self + re5::off::kPlayerControl, -1) != 0) return false;
    const auto hp = engine::ReadHp(self);
    if (!hp || !MarkedDead(self, hp->hp)) return false;
    // A partner standing as of the last tick must still stand now (both may fall in one frame).
    const std::uintptr_t other = a != 0 && a != self ? a : b;
    const auto otherHp = engine::ReadHp(other);
    return otherHp && !MarkedDead(other, otherHp->hp);
}

bool __fastcall DeadOrFrozenDetour(std::uintptr_t self, void* edx) {
    if (HiddenFall(self)) return false;
    return g_origDeadOrFrozen(self, edx);
}

bool __fastcall InDeathDetour(std::uintptr_t self, void* edx) {
    if (HiddenFall(self)) return false;
    return g_origInDeath(self, edx);
}

// The cockpit's weapon rings check (re5::addr::kFnCockpitRingsCheck). In a split-screen match it
// also wants the cockpit's partner alive, so a co-op fall took the weapon rings from the player
// still standing too (2026-11-21, the user: "you need to fix the ring and inventory"). While that
// partner is a hidden fall and the cockpit's own player stands, the check runs without the partner:
// its field is null for the call (the game's own helpers take a null partner as none) and put back
// right after, so the partner's ring still shows, dimmed.
using CockpitCheckFn = void(__fastcall*)(std::uintptr_t cockpit, void* edx);
CockpitCheckFn g_origCockpitRings = nullptr;

void __fastcall CockpitRingsDetour(std::uintptr_t cockpit, void* edx) {
    const std::uintptr_t partner = ReadOr<std::uintptr_t>(cockpit + re5::addr::kCockpitPartner, 0);
    const std::uintptr_t player = ReadOr<std::uintptr_t>(cockpit + re5::addr::kCockpitPlayer, 0);
    if (partner == 0 || player == 0 || partner == player || !HiddenFall(partner) || HiddenFall(player)) {
        g_origCockpitRings(cockpit, edx);
        return;
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 2) {
        RM_LOGI("co-op: cockpit %08X keeps its rings; its partner %08X is down", static_cast<unsigned>(cockpit), static_cast<unsigned>(partner));
    }
    SafeStore<std::uintptr_t>(cockpit + re5::addr::kCockpitPartner, 0);
    g_origCockpitRings(cockpit, edx);
    SafeStore<std::uintptr_t>(cockpit + re5::addr::kCockpitPartner, partner);
}

// uPlayer's own game over (re5::addr::kFnPlayerGameOver). A fallen co-op player sets the game over
// flag itself every frame once its death has played out; the game-over check took it back once a
// frame, but in between the other player's inventory grid saw it and stayed shut. For a hidden fall
// only its first part runs (virtual +0x1B4 with bit 4 of the action flags), not the flag.
using PlayerGameOverFn = void(__fastcall*)(std::uintptr_t self, void* edx);
PlayerGameOverFn g_origPlayerGameOver = nullptr;

void __fastcall PlayerGameOverDetour(std::uintptr_t self, void* edx) {
    if (!HiddenFall(self)) {
        g_origPlayerGameOver(self, edx);
        return;
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 2) RM_LOGI("co-op: %08X fell; its game over is not set while the other player stands", static_cast<unsigned>(self));
    if ((ReadOr<std::uint32_t>(self + re5::off::kPlayerActionFlags, 0) & 0x10u) != 0) {
        const auto vtable = ReadOr<std::uintptr_t>(self, 0);
        const auto fn = vtable ? ReadOr<std::uintptr_t>(vtable + 0x1B4, 0) : 0;
        if (fn) reinterpret_cast<void(__fastcall*)(std::uintptr_t, void*)>(fn)(self, nullptr);
    }
}

// The inventory grid's visibility (re5::addr::kFnWpSlotUiVisible). In a split-screen match the
// grid shows only while the partner lives, so after a co-op fall the player still standing opened
// the inventory and saw nothing. While that partner is a hidden fall and the grid's own player
// stands, the check runs without the partner (null for the call; the grid's move sets it again
// right after).
using WpSlotUiVisibleFn = bool(__fastcall*)(std::uintptr_t self, void* edx);
WpSlotUiVisibleFn g_origWpSlotUiVisible = nullptr;

bool __fastcall WpSlotUiVisibleDetour(std::uintptr_t self, void* edx) {
    const std::uintptr_t partner = ReadOr<std::uintptr_t>(self + re5::addr::kWpSlotUiPartner, 0);
    const std::uintptr_t player = ReadOr<std::uintptr_t>(self + re5::addr::kWpSlotUiPlayer, 0);
    if (partner == 0 || player == 0 || partner == player || !HiddenFall(partner) || HiddenFall(player)) {
        return g_origWpSlotUiVisible(self, edx);
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 2) RM_LOGI("co-op: inventory grid of %08X shown with its partner down", static_cast<unsigned>(player));
    SafeStore<std::uintptr_t>(self + re5::addr::kWpSlotUiPartner, 0);
    const bool shown = g_origWpSlotUiVisible(self, edx);
    SafeStore<std::uintptr_t>(self + re5::addr::kWpSlotUiPartner, partner);
    return shown;
}

// The may-take-item check's partner (re5::addr::kPickupPartnerCall). Its one call of the
// partner lookup goes here instead: while the player is the only one standing in a co-op match
// (the partner a fall) it has no partner, so a pickup is decided by the player's own inventory;
// otherwise the game's own lookup answers. Only this call: every other partner lookup is the game's.
using PlayerPartnerFn = std::uintptr_t(__fastcall*)(std::uintptr_t self, void* edx, int which);

bool OnlyOneStanding(std::uintptr_t self);

std::uintptr_t __fastcall PickupPartner(std::uintptr_t self, void* edx, int which) {
    const std::uintptr_t partner = reinterpret_cast<PlayerPartnerFn>(re5::addr::kFnPlayerPartner)(self, edx, which);
    if (partner == 0 || !OnlyOneStanding(self) || !HiddenFall(partner)) return partner;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 2) RM_LOGI("co-op: %08X picks up with its partner down (its own inventory decides)", static_cast<unsigned>(self));
    return 0;
}

// uPlayer's may-act check (re5::addr::kFnPlayerMayAct). In a split-screen match it says no while
// the partner is dead, so after a co-op fall the player still standing could not open the
// inventory (the grid asks it first). While that player is the only one standing and the mod is
// not ending the match, its own part of the check decides.
using PlayerMayActFn = bool(__fastcall*)(std::uintptr_t self, void* edx);
PlayerMayActFn g_origPlayerMayAct = nullptr;

bool OnlyOneStanding(std::uintptr_t self) {
    if (!self || !g_coopDeathHold.load(std::memory_order_relaxed) || g_coopEndWanted.load(std::memory_order_relaxed)) return false;
    const std::uintptr_t a = g_standing[0].load(std::memory_order_relaxed);
    const std::uintptr_t b = g_standing[1].load(std::memory_order_relaxed);
    return (a == self && b == 0) || (b == self && a == 0);  // the tick fills g_standing in a co-op match only
}

bool __fastcall PlayerMayActDetour(std::uintptr_t self, void* edx) {
    if (g_origPlayerMayAct(self, edx)) return true;
    if (!OnlyOneStanding(self)) return false;
    const auto hp = engine::ReadHp(self);
    if (!hp || MarkedDead(self, hp->hp) || ReadOr<std::int16_t>(self + 0x1364, 0) == 1 || ReadOr<std::int8_t>(self + 0x1BCC, 0) > 0) return false;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 2) RM_LOGI("co-op: %08X may act (inventory and moves) with its partner down", static_cast<unsigned>(self));
    return true;
}

// The request words (re5::addr::kFnAppRequestWord). While one co-op player is down and the other
// stands, word 1's 0x400000 (no player input) is dropped, whoever asks for it; each such request is
// logged with its caller (a few), and so are the others made meanwhile.
using AppRequestWordFn = void(__fastcall*)(std::uintptr_t app, void* edx, std::uint32_t flags, int index);
AppRequestWordFn g_origAppRequestWord = nullptr;

void __fastcall AppRequestWordDetour(std::uintptr_t app, void* edx, std::uint32_t flags, int index) {
    if (g_coopDeathHold.load(std::memory_order_relaxed) && !g_coopEndWanted.load(std::memory_order_relaxed) && StandingHumans() > 0 &&
        HumanDown()) {
        static std::atomic<int> logged{0};
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const bool lock = index == 1 && (flags & kAppInputLock) != 0;
        if (logged.fetch_add(1) < 16) {
            RM_LOGI("co-op: request word %d %08X from %08X while a player is down%s", index, flags, static_cast<unsigned>(caller),
                    lock ? ": the input lock dropped" : "");
        }
        if (lock) {
            flags &= ~kAppInputLock;
            if (flags == 0) return;
        }
    }
    g_origAppRequestWord(app, edx, flags, index);
}

std::uint32_t __fastcall GameOverCheckDetour(std::uintptr_t self, void* edx) {
    if (g_coopDeathHold.load(std::memory_order_relaxed) && !g_coopEndWanted.load(std::memory_order_relaxed) &&
        StandingHumans() > 0 && HumanDown()) {
        ReleaseCoopFreeze();
        return 0;
    }
    if (g_coopDeathHold.load(std::memory_order_relaxed)) {
        const auto app = ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0);
        const auto flags = app ? ReadOr<std::uint32_t>(app + re5::off::kAppFlags, 0) : 0;
        if ((flags & kAppGameOver) != 0) {
            const int standing = StandingHumans();
            if (standing > 0 && SafeStore<std::uint32_t>(app + re5::off::kAppFlags, flags & ~kAppGameOver)) {
                static std::atomic<int> logged{0};
                if (logged.fetch_add(1) < 8) RM_LOGI("co-op: the game over flag cleared (%d player(s) standing)", standing);
            }
        }
    }
    return g_origGameOverCheck(self, edx);
}

// A run's match is on (mod.cpp, as autopilot::SetRunMatch): the boss Jill's blow-away death asks for
// no game over then.
std::atomic<bool> g_runMatchHooks{false};

void __fastcall AppRequestDetour(std::uintptr_t app, void* edx, std::uint32_t flags) {
    // The boss Jill's blow-away death (re5_layout.h kJillDieBlowGameOverReturn): the story's "partner
    // died" game over, in a run only her fall (the arena clears on her death).
    if ((flags & kAppGameOver) != 0 && g_runMatchHooks.load(std::memory_order_relaxed) && engine::Caps().em84DieBlow &&
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == re5::addr::kJillDieBlowGameOverReturn) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 4) RM_LOGI("boss: Jill's blow-away death asked for the game over; not in a run");
        flags &= ~kAppGameOver;
        if (flags == 0) return;
    }
    if ((flags & kAppGameOver) != 0 && g_coopDeathHold.load(std::memory_order_relaxed)) {
        const int standing = StandingHumans();
        if (standing > 0) {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 8) RM_LOGI("co-op: the game over is held back (%d player(s) standing)", standing);
            flags &= ~kAppGameOver;
            if (flags == 0) return;
        }
    }
    g_origAppRequest(app, edx, flags);
}

// A human player at zero health (a co-op fall).
// A player the game counts as dead: no health, or the death mark (bit 1 of the action flags, which
// the die actions carry; 0x00B6B380 reads it).
bool MarkedDead(std::uintptr_t player, int hp) {
    return hp <= 0 || (ReadOr<std::uint32_t>(player + re5::off::kPlayerActionFlags, 0) & 0x2u) != 0;
}

bool HumanDown() {
    const auto snap = engine::ReadPlayers();
    for (const auto& p : snap.players) {
        if (p.object && p.control == 0 && MarkedDead(p.object, p.hp.hp)) return true;
    }
    return false;
}

// The match's end message (re5::addr::kFnMercEndEvent). In a co-op run one player's fall must
// not end the match for the other; the mod's own ends (time-up for a stage change, the run over)
// pass. Every call is logged (a few), with what the message holds.
using MercEndFn = void(__fastcall*)(std::uintptr_t self, void* edx, int sender, std::uintptr_t message);
MercEndFn g_origMercEnd = nullptr;

void __fastcall MercEndDetour(std::uintptr_t self, void* edx, int sender, std::uintptr_t message) {
    const bool hold = g_coopDeathHold.load(std::memory_order_relaxed) && !g_coopEndWanted.load(std::memory_order_relaxed);
    const int standing = hold ? StandingHumans() : 0;
    const bool down = hold && HumanDown();
    const bool block = hold && standing > 0 && down;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 24) {
        const auto app = ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0);
        const auto work = app ? ReadOr<std::uintptr_t>(app + re5::off::kGameWork, 0) : 0;
        RM_LOGI("co-op: the match's end message (sender %d, +4 %d, +8 %.2f, +C %d; mode %d, Mercenaries phase %d; hold %d, "
                "standing %d, a player down %d): %s",
                sender, message ? ReadOr<std::int32_t>(message + 4, -1) : -1, message ? ReadOr<float>(message + 8, 0.0f) : 0.0f,
                message ? ReadOr<std::int32_t>(message + 0xC, -1) : -1, work ? ReadOr<std::int32_t>(work + 0x58, -1) : -1,
                work ? ReadOr<std::int32_t>(work + re5::off::kGameWorkMercPhase, -1) : -1, hold ? 1 : 0, standing, down ? 1 : 0,
                block ? "held back (the other player fights on)" : "passed");
    }
    if (block) return;
    g_origMercEnd(self, edx, sender, message);
}

void __fastcall DeathEventDetour(std::uintptr_t self, void* edx, int player) {
    // M10: a co-op run goes on while a human player stands; the fallen one lies where they fell
    // until the next arena (the game would end the match for both).
    if (g_coopDeathHold.load(std::memory_order_relaxed) && !g_deathEventPass.load(std::memory_order_relaxed)) {
        const int standing = StandingHumans();
        if (standing > 0) {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 8) RM_LOGI("co-op: player %d is down; the match goes on (%d standing)", player, standing);
            return;
        }
    }
    if (const std::uintptr_t partner = g_partnerUnit.load(std::memory_order_relaxed)) {
        const int number = ReadOr<std::uint8_t>(partner + re5::off::kPlayerNumber, 0xFF);
        const auto local = engine::LocalPlayer();
        const auto localHp = local ? engine::ReadHp(local) : std::nullopt;
        const auto partnerHp = engine::ReadHp(partner);
        if (player == number || (player == -1 && localHp && localHp->hp > 0 && partnerHp && partnerHp->hp <= 0)) {
            if (g_partnerDeathLogged.fetch_add(1) == 0) RM_LOGI("partner: executed; the match goes on (player %d)", player);
            return;
        }
    }
    g_origDeathEvent(self, edx, player);
}

void __fastcall Em82SetPoseDetour(std::uintptr_t self, void*, int pose, int apply) {
    if ((apply & 0xFF) == 1 && (pose & 0xFF) == 1 && IsRunBoss(self)) {
        if (g_em82PoseLogged.fetch_add(1) < 4) RM_LOGI("boss: uEm82's giant form asked for: his pose only (he stays hittable)");
        apply = 0;
    }
    g_origEm82SetPose(self, pose, apply);
}

void __fastcall Em41UpdateDetour(std::uintptr_t self, void* edx) {
    if (!IsRunBoss(self)) {
        g_origEm41Update(self, edx);
        return;
    }
    *reinterpret_cast<std::uint8_t*>(self + re5::off::kEm41UpdateFlag) = 0;
    reinterpret_cast<UpdateFn>(re5::addr::kFnEm40Update)(self, nullptr);
    if (g_em41Logged.fetch_add(1) == 0) RM_LOGI("boss: uEm41 updates without its full-health refill");
}
std::atomic<int> g_hitProbeLines{0};

std::uint32_t __fastcall HitCheckDetour(std::uintptr_t self, void*, std::uintptr_t part) {
    const std::uintptr_t probe = g_hitProbe.load(std::memory_order_relaxed);
    if (probe == 0 || self != probe) return g_origHitCheck(self, part);
    const std::uintptr_t unit = part ? part : self;
    const auto record = ReadOr<std::uintptr_t>(unit + re5::off::kEnemyHitRecord, 0);
    const auto pending = record ? ReadOr<std::uint8_t>(record + re5::off::kHitRecordPending, 0) : 0;
    const auto block = ReadOr<std::uint8_t>(self + re5::off::kEnemyHitBlock, 0);
    const auto flags = ReadOr<std::uint32_t>(self + re5::off::kEnemyHitFlags, 0);
    float cool[5];
    for (int k = 0; k < 5; ++k) cool[k] = ReadOr<float>(self + re5::off::kEnemyHitCooldowns + k * 4, -1.0f);
    const std::uint32_t result = g_origHitCheck(self, part);
    if (pending && g_hitProbeLines.fetch_add(1) < 400) {
        RM_LOGW("hitprobe: %s part %08X (%s) record %08X pending %u; block %u flags80 %u cooldowns %.2f %.2f %.2f %.2f %.2f -> %u",
                engine::ClassName(self).c_str(), static_cast<unsigned>(part), part ? engine::ClassName(part).c_str() : "body",
                static_cast<unsigned>(record), pending, block, (flags & 0x80u) ? 1u : 0u, cool[0], cool[1], cool[2], cool[3], cool[4],
                result & 0xFFu);
    }
    return result;
}
SubHpFn g_origCharacterSubHp = nullptr;
SubHpFn g_origPlayerSubHp = nullptr;

int ClampDamage(double value) {
    if (value < 1.0) {
        return 1;
    }
    if (value > 32767.0) {
        return 32767;
    }
    return static_cast<int>(std::lround(value));
}

void __fastcall EnemyOnHitDetour(std::uintptr_t self, void*, std::uintptr_t hit) {
    ++g_status.enemyHits;
    HitScope scope;
    scope.enemy = self;
    scope.previous = t_hit;
    const auto before = engine::ReadHp(self);
    if (hit) {
        scope.attacker = engine::HitAttacker(hit);
        scope.attackerIsPlayer = scope.attacker && engine::IsKindOf(scope.attacker, re5::addr::kDtiUPlayer);
        scope.attackerIsLocal = scope.attackerIsPlayer && engine::HumanPlayerSlot(scope.attacker) >= 0;
    }
    t_hit = &scope;
    g_origEnemyOnHit(self, hit);
    t_hit = scope.previous;

    const auto after = engine::ReadHp(self);
    if (!g_sink || !before || !after || (before->hp == after->hp && scope.rawDamage == 0)) {
        return;
    }
    EnemyHitResult result;
    result.enemy = self;
    result.attacker = scope.attacker;
    result.attackerIsPlayer = scope.attackerIsPlayer;
    result.attackerIsLocal = scope.attackerIsLocal;
    result.hpBefore = before->hp;
    result.hpAfter = after->hp;
    result.maxHp = after->max;
    result.rawDamage = scope.rawDamage;
    result.appliedDamage = scope.appliedDamage;
    result.killed = before->hp > 0 && after->hp <= 0;
    g_sink->EnemyHitDone(result);
}

void __fastcall CharacterSubHpDetour(std::uintptr_t self, void*, std::int16_t amount) {
    HitScope* scope = t_hit;
    if (scope && scope->enemy == self && amount > 0 && g_sink) {
        const auto hp = engine::ReadHp(self);
        OutgoingHit hit;
        hit.enemy = self;
        hit.attacker = scope->attacker;
        hit.attackerIsPlayer = scope->attackerIsPlayer;
        hit.attackerIsLocal = scope->attackerIsLocal;
        hit.damage = amount;
        hit.targetHp = hp ? hp->hp : 0;
        hit.targetMaxHp = hp ? hp->max : 0;
        float multiplier = g_sink->OutgoingMultiplier(hit);
        // A body hit on Uroboros Wesker counts for its share only (the core takes it all).
        if (scope->bodyHit) multiplier *= g_bossBodyShare.load(std::memory_order_relaxed);
        int applied = std::isfinite(multiplier) ? ClampDamage(amount * static_cast<double>(multiplier)) : amount;
        // A boss's add that is a boss itself, and the boss with a phase left, keep their last health
        // point: the mod ends the one and puts the other down (mod.cpp).
        if (KeepsLastHealth(self) && hit.targetHp > 0 && applied >= hit.targetHp) applied = std::max(0, hit.targetHp - 1);
        scope->rawDamage += amount;
        scope->appliedDamage += applied;
        amount = static_cast<std::int16_t>(applied);
    } else if (amount > 0 && KeepsLastHealth(self)) {
        // Damage outside a hit (a burn, the game's own scripts) keeps it too.
        if (const auto hp = engine::ReadHp(self); hp && hp->hp > 0 && amount >= hp->hp) amount = static_cast<std::int16_t>(hp->hp - 1);
    }
    g_origCharacterSubHp(self, amount);
}

void __fastcall PlayerSubHpDetour(std::uintptr_t self, void*, std::int16_t amount) {
    if (amount <= 0 || !g_sink || engine::HumanPlayerSlot(self) < 0) {
        g_origPlayerSubHp(self, amount);
        return;
    }
    ++g_status.playerHits;
    const auto before = engine::ReadHp(self);
    IncomingHit hit;
    hit.player = self;
    // The hit being taken (the player's hit record, uCharacter +0x129C, its byte +0x48 set while it is live:
    // the reactions' 0x00B7B730 reads the same): who made it (Safari Chris's blasts, 2026-11-23).
    if (const auto record = ReadOr<std::uintptr_t>(self + re5::off::kEnemyHitRecord, 0); record && ReadOr<std::uint8_t>(record + 0x48, 0) != 0) {
        hit.attacker = engine::HitAttacker(record);
    }
    hit.damage = amount;
    hit.hp = before ? before->hp : 0;
    hit.maxHp = before ? before->max : 0;
    const IncomingDecision decision = g_sink->Incoming(hit);
    int applied = amount;
    if (std::isfinite(decision.multiplier)) {
        // A zero multiplier (developer invulnerability) skips the hit entirely.
        applied = decision.multiplier <= 0.0f ? 0 : ClampDamage(amount * static_cast<double>(decision.multiplier));
    }
    if (decision.preventDeath && before && applied >= before->hp) {
        applied = before->hp > decision.leaveHp ? before->hp - decision.leaveHp : 0;
    }
    if (applied > 0) {
        g_origPlayerSubHp(self, static_cast<std::int16_t>(applied));
    }
    if (decision.noReaction) {
        // Not even a flinch: the hit record's pending byte cleared, so the player's hit check (0x00B7CDD0, before the
        // reactions' 0x00B7B730 in the same update) finds nothing to react to.
        if (const auto record = ReadOr<std::uintptr_t>(self + re5::off::kEnemyHitRecord, 0); record != 0) {
            SafeStore<std::uint8_t>(record + re5::off::kHitRecordPending, 0);
        }
    }
    const auto after = engine::ReadHp(self);
    PlayerHitResult result;
    result.player = self;
    result.rawDamage = amount;
    result.appliedDamage = applied;
    result.hpBefore = hit.hp;
    result.hpAfter = after ? after->hp : 0;
    result.maxHp = after ? after->max : hit.maxHp;
    g_sink->PlayerHitDone(result);
}

SubHpFn g_origPlayerAddHp = nullptr;

// A healing item on the local player: the sink may change the amount (a build's heals).
void __fastcall PlayerAddHpDetour(std::uintptr_t self, void*, std::int16_t amount) {
    if (amount > 0 && g_sink && engine::HumanPlayerSlot(self) >= 0) {
        const int wanted = g_sink->HealAmount(self, amount);
        amount = static_cast<std::int16_t>(wanted < 0 ? 0 : (wanted > 32767 ? 32767 : wanted));
    }
    g_origPlayerAddHp(self, amount);
}

// The flamethrower with an element's look in place of its flame (SetFlameStream).
using Wp21FireFn = void(__thiscall*)(std::uintptr_t self, std::uintptr_t attack);
Wp21FireFn g_origWp21Fire = nullptr;
std::mutex g_streamMutex;
// One per player (M10: each player's flamethrower has its own look; index = seat).
struct StreamLook {
    std::uintptr_t unit = 0;
    std::string path, companion;
    bool loops = false;
    float scale = 1.0f;
    std::uint32_t effect = 0;           // the look the mod started at the nozzle (0: none)
    std::uint32_t companionEffect = 0;  // and its companion's
    DWORD effectAt = 0;
    DWORD firedAt = 0;
    int fireLogs = 0;
};
StreamLook g_streams[2];
constexpr DWORD kStreamLookRestartMs = 300;

StreamLook* StreamOfSeat(int seat) { return seat >= 0 && seat < 2 ? &g_streams[seat] : nullptr; }

StreamLook* StreamOfUnit(std::uintptr_t unit) {
    for (StreamLook& s : g_streams) {
        if (unit != 0 && s.unit == unit) return &s;
    }
    return nullptr;
}

// The flame's sound with an element's look (the user, 2026-11-24: "and flamethrower sound is gone?"): the
// flamethrower's update (0x00BBC8C0) plays the flame's sounds while its stream id (+0x1C8C, mEffIdFire) is set and
// stops them once it is 0, and makes its flame hits only from the effect that id finds (0x00770510, a walk of the
// effect manager's list; nothing for an id no effect has). Fire with the look sets this id, which no effect has
// (the manager counts its ids up from 1): the sound plays, no flame hits come; the look's end puts 0 back.
constexpr std::uint32_t kSilentStreamId = 0x7FFFFF21u;
std::atomic<bool> g_streamSound{true};

void StopStreamLookLocked(StreamLook& s) {
    if (s.effect != 0) engine::StopEffect(s.effect);
    if (s.companionEffect != 0) engine::StopEffect(s.companionEffect);
    s.effect = 0;
    s.companionEffect = 0;
    if (s.unit != 0 && ReadOr<std::uintptr_t>(s.unit, 0) == re5::addr::kVtableWp21 &&
        ReadOr<std::uint32_t>(s.unit + re5::off::kWp21StreamEffect, 0) == kSilentStreamId) {
        SafeStore<std::uint32_t>(s.unit + re5::off::kWp21StreamEffect, 0);  // its sound ends with it
    }
}

// With g_streamMutex held: the look at the nozzle (joint 0, where the flame starts) and its
// companion, the last ones stopped first; with `force` (a fire, as the game starts its flame again
// at each), else only when kStreamLookRestartMs have passed.
void RefreshStreamLookLocked(StreamLook& s, bool force) {
    if (ReadOr<std::uintptr_t>(s.unit, 0) != re5::addr::kVtableWp21) return;  // not (or no longer) the flamethrower
    const DWORD now = GetTickCount();
    if (!force && s.effect != 0 && now - s.effectAt < kStreamLookRestartMs) return;
    StopStreamLookLocked(s);
    s.effect = engine::StartEffect(s.path.c_str(), s.unit, 0, s.scale);
    if (!s.companion.empty()) s.companionEffect = engine::StartEffect(s.companion.c_str(), s.unit, 0, s.scale);
    s.effectAt = now;
}

// The game's fire (re5::addr::kFnWp21Fire) without its flame: the firing flag, the old stream
// stopped, no stream kept (so no flame, no flame hits; its sound by kSilentStreamId), the attack
// data; the element's look at the nozzle instead.
void __fastcall Wp21FireDetour(std::uintptr_t self, void*, std::uintptr_t attack) {
    std::unique_lock lock(g_streamMutex);
    StreamLook* s = StreamOfUnit(self);
    if (s == nullptr || s->path.empty()) {
        lock.unlock();
        g_origWp21Fire(self, attack);
        return;
    }
    SafeStore<std::uint8_t>(self + re5::off::kWp21Firing, 1);
    std::uint32_t old = 0;
    if (SafeRead(self + re5::off::kWp21StreamEffect, old) && old != 0 && old != kSilentStreamId) engine::StopEffect(old);
    // No stream the game's update could read hits from; the silent id keeps its flame's sound (kSilentStreamId).
    SafeStore<std::uint32_t>(self + re5::off::kWp21StreamEffect, g_streamSound.load(std::memory_order_relaxed) ? kSilentStreamId : 0u);
    SafeStore<std::uintptr_t>(self + re5::off::kWp21AttackData, attack);
    RefreshStreamLookLocked(*s, true);
    const DWORD now = GetTickCount();
    const DWORD before = s->firedAt;
    s->firedAt = now;
    if (s->fireLogs < 4) {
        ++s->fireLogs;
        RM_LOGI("flamethrower: %08X fired with %s in place of its flame (look %u; %lu ms since its last fire)", static_cast<unsigned>(self),
                s->path.c_str(), s->effect, before != 0 ? static_cast<unsigned long>(now - before) : 0ul);
    }
}

// The flamethrower's charge mode (re5::addr::kFnWp21Charge) in a run's match: never (the user,
// 2026-11-23: "in public assembly in the house with 3 stories there's always the "recharge
// flamethrower" prompt and if you press it, the character puts the flamethrower never to be attained
// again"). It sends the gun to the story's pump spot, wherever the player is; the mod refills its fuel
// itself (RefuelFlamethrower). The put's motion plays and the flamethrower stays the player's.
using Wp21ChargeFn = void(__thiscall*)(std::uintptr_t self);
Wp21ChargeFn g_origWp21Charge = nullptr;
std::atomic<int> g_chargeRefused{0};

void __fastcall Wp21ChargeDetour(std::uintptr_t self, void*) {
    if (g_runMatchHooks.load(std::memory_order_relaxed)) {
        if (g_chargeRefused.fetch_add(1) < 8) {
            RM_LOGI("flamethrower: %08X would go to the story's pump (its charge mode); kept with the player", static_cast<unsigned>(self));
        }
        return;
    }
    g_origWp21Charge(self);
}

// Developer: a unit's attach (uCoord vtable +0x34, 0x00489860: parent +0x18, joint +0x1C) to the
// probed parent (the local player), logged when its joint or parent changes, with the caller (dev
// "attachprobe"; 2026-11-19, a gun drawn nowhere: its joint 2 instead of the hand's).
using AttachFn = void(__fastcall*)(std::uintptr_t self, void* edx, std::uintptr_t parent, std::uint32_t joint);
AttachFn g_origAttach = nullptr;
std::atomic<std::uintptr_t> g_attachProbe{0};
std::atomic<int> g_attachProbeLines{0};

void __fastcall AttachDetour(std::uintptr_t self, void* edx, std::uintptr_t parent, std::uint32_t joint) {
    const std::uintptr_t probe = g_attachProbe.load(std::memory_order_relaxed);
    if (probe != 0 && parent == probe && self != 0) {
        const auto oldParent = *reinterpret_cast<std::uintptr_t*>(self + 0x18);
        const auto oldJoint = *reinterpret_cast<std::uint32_t*>(self + 0x1C);
        if ((oldParent != parent || oldJoint != joint) && g_attachProbeLines.fetch_add(1) < 300) {
            RM_LOGI("attach probe: %08X (vtable %08X) to joint %u (was %u, parent %s) from %08X", static_cast<unsigned>(self),
                    static_cast<unsigned>(*reinterpret_cast<std::uintptr_t*>(self)), joint, oldJoint,
                    oldParent == parent ? "same" : "new", static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(_ReturnAddress())));
        }
    }
    g_origAttach(self, edx, parent, joint);
}

bool Hook(std::uintptr_t target, void* detour, void** original, const char* name) {
    auto* fn = reinterpret_cast<void*>(target);
    MH_STATUS status = MH_CreateHook(fn, detour, original);
    if (status == MH_OK) {
        status = MH_QueueEnableHook(fn);
    }
    if (status != MH_OK) {
        RM_LOGE("hook %s at 0x%08X failed: %s", name, static_cast<unsigned>(target), MH_StatusToString(status));
        return false;
    }
    RM_LOGI("hook %s at 0x%08X queued", name, static_cast<unsigned>(target));
    return true;
}

}  // namespace

bool InstallCombatHooks() {
    const auto& caps = engine::Caps();
    if (!caps.enemyOnHit || !caps.characterSubHp || !caps.playerSubHp || !caps.hitAttacker || !caps.players) {
        RM_LOGE("combat hooks skipped: required signatures missing");
        return false;
    }
    bool ok = Hook(re5::addr::kFnEnemyOnHit, reinterpret_cast<void*>(&EnemyOnHitDetour),
                   reinterpret_cast<void**>(&g_origEnemyOnHit), "uEnemy::OnHit");
    ok = ok && Hook(re5::addr::kFnCharacterSubHp, reinterpret_cast<void*>(&CharacterSubHpDetour),
                    reinterpret_cast<void**>(&g_origCharacterSubHp), "uCharacter::SubHp");
    ok = ok && Hook(re5::addr::kFnPlayerSubHp, reinterpret_cast<void*>(&PlayerSubHpDetour),
                    reinterpret_cast<void**>(&g_origPlayerSubHp), "uPlayer::SubHp");
    if (ok && caps.playerAddHp) {
        Hook(re5::addr::kFnPlayerAddHp, reinterpret_cast<void*>(&PlayerAddHpDetour),
             reinterpret_cast<void**>(&g_origPlayerAddHp), "uPlayer::AddHp");
    }
    if (ok && caps.bossGunfire) {
        Hook(re5::addr::kFnEm82MayHurt, reinterpret_cast<void*>(&Em82MayHurtDetour), reinterpret_cast<void**>(&g_origEm82MayHurt),
             "uEm82 may-hurt (a run's boss takes gunfire)");
        if (engine::Caps().em82Tuning) {
            Hook(re5::addr::kFnEm82SetPose, reinterpret_cast<void*>(&Em82SetPoseDetour), reinterpret_cast<void**>(&g_origEm82SetPose),
                 "uEm82 form switch (a run's boss keeps his own body and hit shapes)");
        }
    }
    if (ok && caps.bossNoRefill) {
        Hook(re5::addr::kFnEm41Update, reinterpret_cast<void*>(&Em41UpdateDetour), reinterpret_cast<void**>(&g_origEm41Update),
             "uEm41 update (a run's boss keeps the damage it takes)");
    }
    if (ok && caps.partnerDeath) {
        Hook(re5::addr::kFnPlayerDeathEvent, reinterpret_cast<void*>(&DeathEventDetour), reinterpret_cast<void**>(&g_origDeathEvent),
             "player execution (a computer partner's does not end the run)");
    }
    if (ok && caps.gameOverCheck) {
        Hook(re5::addr::kFnGameOverCheck, reinterpret_cast<void*>(&GameOverCheckDetour), reinterpret_cast<void**>(&g_origGameOverCheck),
             "game-over check (a co-op run's game over waits for the last player standing)");
    }
    if (ok && caps.appRequest) {
        Hook(re5::addr::kFnAppRequest, reinterpret_cast<void*>(&AppRequestDetour), reinterpret_cast<void**>(&g_origAppRequest),
             "application request (a co-op run's game over waits for the last player standing)");
    }
    if (ok && caps.wpSlotUiVisible) {
        Hook(re5::addr::kFnWpSlotUiVisible, reinterpret_cast<void*>(&WpSlotUiVisibleDetour), reinterpret_cast<void**>(&g_origWpSlotUiVisible),
             "inventory grid visibility (a co-op fall keeps the other player's inventory)");
    }
    if (ok && caps.pickupPartner) {
        RM_LOGI("may-take-item check: its partner lookup %s (a co-op fall keeps the other player's pickups)",
                SetPickupPartnerPatch(true) ? "goes through the mod" : "left as it is (bytes not as expected)");
    }
    if (ok && caps.playerGameOver) {
        Hook(re5::addr::kFnPlayerGameOver, reinterpret_cast<void*>(&PlayerGameOverDetour), reinterpret_cast<void**>(&g_origPlayerGameOver),
             "player's own game over (a co-op fall does not end the match for the other player)");
    }
    if (ok && caps.playerMayAct) {
        Hook(re5::addr::kFnPlayerMayAct, reinterpret_cast<void*>(&PlayerMayActDetour), reinterpret_cast<void**>(&g_origPlayerMayAct),
             "player may-act check (a co-op fall keeps the other player's inventory)");
    }
    if (ok && caps.cockpitRings) {
        Hook(re5::addr::kFnCockpitRingsCheck, reinterpret_cast<void*>(&CockpitRingsDetour), reinterpret_cast<void**>(&g_origCockpitRings),
             "cockpit weapon rings check (a co-op fall keeps the other player's rings)");
    }
    if (ok && caps.playerDeathChecks) {
        Hook(re5::addr::kFnPlayerDeadOrFrozen, reinterpret_cast<void*>(&DeadOrFrozenDetour), reinterpret_cast<void**>(&g_origDeadOrFrozen),
             "player dead-or-frozen check (a co-op fall does not freeze the other player)");
        Hook(re5::addr::kFnPlayerInDeath, reinterpret_cast<void*>(&InDeathDetour), reinterpret_cast<void**>(&g_origInDeath),
             "player death-sequence check (a co-op fall does not freeze the other player)");
    }
    if (ok && caps.appRequestWord) {
        Hook(re5::addr::kFnAppRequestWord, reinterpret_cast<void*>(&AppRequestWordDetour), reinterpret_cast<void**>(&g_origAppRequestWord),
             "application request words (a co-op fall does not stop the other player's input)");
    }
    if (ok && caps.mercEndEvent) {
        Hook(re5::addr::kFnMercEndEvent, reinterpret_cast<void*>(&MercEndDetour), reinterpret_cast<void**>(&g_origMercEnd),
             "match end message (a co-op player's fall does not end the match for the other)");
    }
    if (ok && caps.bossBrain) {
        Hook(re5::addr::kFnEm40CanDive, reinterpret_cast<void*>(&Em40CanDiveDetour), reinterpret_cast<void**>(&g_origEm40CanDive),
             "uEm40 may-dive (a run has one player)");
    }
    if (ok && caps.flameStream) {
        Hook(re5::addr::kFnWp21Fire, reinterpret_cast<void*>(&Wp21FireDetour), reinterpret_cast<void**>(&g_origWp21Fire),
             "uWp21 fire (an element's look in place of the flame)");
    }
    if (ok && caps.flameCharge) {
        Hook(re5::addr::kFnWp21Charge, reinterpret_cast<void*>(&Wp21ChargeDetour), reinterpret_cast<void**>(&g_origWp21Charge),
             "uWp21 charge mode (never in a run: the story's pump)");
    }
    if (ok && caps.hitCheckProbe) {
        Hook(re5::addr::kFnEnemyHitCheck, reinterpret_cast<void*>(&HitCheckDetour), reinterpret_cast<void**>(&g_origHitCheck),
             "enemy hit check (developer probe)");
    }
    g_status.combat = ok;
    return ok;
}

void SetCoopDeathHold(bool on) {
    if (g_coopDeathHold.exchange(on) != on) RM_LOGI("co-op: a fallen player %s the match", on ? "no longer ends" : "ends");
}

void SetCoopMatchEndWanted(bool wanted) { g_coopEndWanted = wanted; }
void SetDeathEventPass(bool pass) { g_deathEventPass = pass; }
std::uintptr_t GameTask() {
    const auto root = ReadOr<std::uintptr_t>(re5::addr::kLinkUnitRoot, 0);
    const auto game = root ? ReadOr<std::uintptr_t>(root + re5::addr::kGameTaskOffset, 0) : 0;
    return game && engine::ClassName(game) == "aGame" ? game : 0;
}

int GamePlayerOff(int player) {
    const auto game = GameTask();
    const auto bytes = game && player >= 0 && player < 4 ? ReadOr<std::uintptr_t>(game + re5::addr::kGamePlayerOffBytes, 0) : 0;
    return bytes ? ReadOr<std::uint8_t>(bytes + static_cast<std::uintptr_t>(player), 0) : -1;
}

// The call (E8 rel32) at re5::addr::kPickupPartnerCall: to PickupPartner, or back to the game's own
// partner lookup. True when the call is as asked.
bool SetPickupPartnerPatch(bool on) {
    if (!engine::Caps().pickupPartner) return false;
    const std::uintptr_t site = re5::addr::kPickupPartnerCall;
    const auto bytesFor = [site](std::uintptr_t target, unsigned char out[5]) {
        const std::int32_t rel = static_cast<std::int32_t>(target - (site + 5));
        out[0] = 0xE8;
        std::memcpy(out + 1, &rel, sizeof(rel));
    };
    unsigned char game[5], mod[5];
    bytesFor(re5::addr::kFnPlayerPartner, game);
    bytesFor(reinterpret_cast<std::uintptr_t>(&PickupPartner), mod);
    const auto hex = [](const unsigned char b[5]) {
        char text[16];
        std::snprintf(text, sizeof(text), "%02X %02X %02X %02X %02X", b[0], b[1], b[2], b[3], b[4]);
        return std::string(text);
    };
    const unsigned char* want = on ? mod : game;
    if (BytesMatch(site, hex(want).c_str())) return true;
    return engine::WriteCode(site, hex(on ? game : mod).c_str(), want, 5);
}

void SetCoopStanding(std::uintptr_t first, std::uintptr_t second) {
    g_standing[0] = first;
    g_standing[1] = second;
}

void SetFlameStreamSound(bool on) { g_streamSound = on; }

void SetFlameStream(int seat, std::uintptr_t unit, const std::string& path, const std::string& companion, bool loops, float scale) {
    std::lock_guard lock(g_streamMutex);
    StreamLook* s = StreamOfSeat(seat);
    if (s == nullptr) return;
    const std::uintptr_t want = path.empty() ? 0 : unit;
    if (want == s->unit && path == s->path && companion == s->companion && loops == s->loops && scale == s->scale) return;
    StopStreamLookLocked(*s);
    s->unit = want;
    s->path = want != 0 ? path : std::string();
    s->companion = want != 0 ? companion : std::string();
    s->loops = loops;
    s->scale = scale;
    s->firedAt = 0;
    s->fireLogs = 0;
}

unsigned long FlameStreamFiredAt(int seat) {
    std::lock_guard lock(g_streamMutex);
    const StreamLook* s = StreamOfSeat(seat);
    return s != nullptr ? s->firedAt : 0;
}

void KeepFlameStreamLook(int seat, bool firing) {
    std::lock_guard lock(g_streamMutex);
    StreamLook* found = StreamOfSeat(seat);
    if (found == nullptr) return;
    StreamLook& s = *found;
    if (!firing || s.unit == 0 || s.path.empty()) {
        StopStreamLookLocked(s);
        return;
    }
    // A look that keeps going (a recoloured flame) is started by the pull's fire and left alone while
    // firing (2026-11-11: started again every 0.3 s, its opening burst flashed the whole screen; the
    // user: "can you make the particles way lesser? the whole screen flashes"). Only when there was
    // no fire (the look set while firing) is one started.
    if (s.loops) {
        if (s.effect == 0) RefreshStreamLookLocked(s, true);
        return;
    }
    if (GetTickCount() - s.firedAt < kStreamLookRestartMs) return;  // its fires start it again
    RefreshStreamLookLocked(s, false);
}

void SetPartnerUnit(std::uintptr_t unit) {
    g_partnerUnit = unit;
    g_partnerDeathLogged = 0;
}

void SetBossTakesGunfire(std::uintptr_t unit) {
    if (g_gunfireBoss.exchange(unit) != unit) {
        g_gunfireLogged = 0;
        g_em41Logged = 0;
    }
}

void SetBossAddUnits(const std::vector<std::uintptr_t>& units) {
    for (int i = 0; i < kBossAddSlots; ++i) {
        g_bossAdds[i].store(i < static_cast<int>(units.size()) ? units[static_cast<std::size_t>(i)] : 0, std::memory_order_relaxed);
    }
}

void SetRunMatchHooks(bool on) { g_runMatchHooks.store(on, std::memory_order_relaxed); }

void SetBossKeepsLastHealth(std::uintptr_t unit) { g_lastHealthBoss.store(unit, std::memory_order_relaxed); }

void SetBossBodyShare(float share) { g_bossBodyShare.store(std::clamp(share, 0.0f, 1.0f), std::memory_order_relaxed); }

bool InstallAttachProbe() {
    if (!engine::Caps().twinGuns) return false;  // the unit_attach signature with the others
    return Hook(re5::addr::kFnUnitAttach, reinterpret_cast<void*>(&AttachDetour), reinterpret_cast<void**>(&g_origAttach),
                "unit attach (developer probe)");
}

bool SetAttachProbe(std::uintptr_t parent) {
    g_attachProbeLines = 0;
    g_attachProbe = g_origAttach != nullptr ? parent : 0;
    return g_origAttach != nullptr;
}

void SetHitProbe(std::uintptr_t unit) {
    g_hitProbeLines = 0;
    g_hitProbe = g_origHitCheck != nullptr ? unit : 0;
}

}  // namespace rmre5
