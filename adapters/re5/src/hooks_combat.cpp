#include <windows.h>

#include <MinHook.h>

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

std::uint32_t __fastcall Em82MayHurtDetour(std::uintptr_t self, void*, std::uint32_t region, std::uint32_t amount) {
    const std::uint32_t own = g_origEm82MayHurt(self, region, amount);
    if ((own & 0xFFu) != 0 || self == 0 || self != g_gunfireBoss.load(std::memory_order_relaxed)) return own;
    if (g_gunfireLogged.fetch_add(1) < 3) {
        RM_LOGI("boss: %s takes a hit its own check refused (region %u)", engine::ClassName(self).c_str(), region & 0xFFFFu);
    }
    return 1;
}

// Uroboros Aheri (uEm41) as a run's boss: its update (0x008B6040) runs the Uroboros' (0x008AE2B0)
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

void __fastcall DeathEventDetour(std::uintptr_t self, void* edx, int player) {
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

void __fastcall Em41UpdateDetour(std::uintptr_t self, void* edx) {
    if (self == 0 || self != g_gunfireBoss.load(std::memory_order_relaxed)) {
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
        scope.attackerIsLocal = scope.attackerIsPlayer && scope.attacker == engine::LocalPlayer();
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
        const float multiplier = g_sink->OutgoingMultiplier(hit);
        const int applied = std::isfinite(multiplier) ? ClampDamage(amount * static_cast<double>(multiplier)) : amount;
        scope->rawDamage += amount;
        scope->appliedDamage += applied;
        amount = static_cast<std::int16_t>(applied);
    }
    g_origCharacterSubHp(self, amount);
}

void __fastcall PlayerSubHpDetour(std::uintptr_t self, void*, std::int16_t amount) {
    if (amount <= 0 || !g_sink || self != engine::LocalPlayer()) {
        g_origPlayerSubHp(self, amount);
        return;
    }
    ++g_status.playerHits;
    const auto before = engine::ReadHp(self);
    IncomingHit hit;
    hit.player = self;
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
    if (amount > 0 && g_sink && self == engine::LocalPlayer()) {
        const int wanted = g_sink->HealAmount(self, amount);
        amount = static_cast<std::int16_t>(wanted < 0 ? 0 : (wanted > 32767 ? 32767 : wanted));
    }
    g_origPlayerAddHp(self, amount);
}

// The flamethrower with an element's look in place of its flame (SetFlameStream).
using Wp21FireFn = void(__thiscall*)(std::uintptr_t self, std::uintptr_t attack);
Wp21FireFn g_origWp21Fire = nullptr;
std::mutex g_streamMutex;
std::uintptr_t g_streamUnit = 0;
std::string g_streamPath, g_streamCompanion;
std::uint32_t g_streamEffect = 0;     // the look the mod started at the nozzle (0: none)
std::uint32_t g_companionEffect = 0;  // and its companion's
DWORD g_streamEffectAt = 0;
std::atomic<DWORD> g_streamFiredAt{0};
int g_streamFireLogs = 0;
constexpr DWORD kStreamLookRestartMs = 300;

void StopStreamLookLocked() {
    if (g_streamEffect != 0) engine::StopEffect(g_streamEffect);
    if (g_companionEffect != 0) engine::StopEffect(g_companionEffect);
    g_streamEffect = 0;
    g_companionEffect = 0;
}

// With g_streamMutex held: the look at the nozzle (joint 0, where the flame starts) and its
// companion, the last ones stopped first; with `force` (a fire, as the game starts its flame again
// at each), else only when kStreamLookRestartMs have passed.
void RefreshStreamLookLocked(std::uintptr_t unit, bool force) {
    if (ReadOr<std::uintptr_t>(unit, 0) != re5::addr::kVtableWp21) return;  // not (or no longer) the flamethrower
    const DWORD now = GetTickCount();
    if (!force && g_streamEffect != 0 && now - g_streamEffectAt < kStreamLookRestartMs) return;
    StopStreamLookLocked();
    g_streamEffect = engine::StartEffect(g_streamPath.c_str(), unit, 0);
    if (!g_streamCompanion.empty()) g_companionEffect = engine::StartEffect(g_streamCompanion.c_str(), unit, 0);
    g_streamEffectAt = now;
}

// The game's fire (re5::addr::kFnWp21Fire) without its flame: the firing flag, the old stream
// stopped, no stream kept (so no flame, no flame hits, no flame sound), the attack data; the
// element's look at the nozzle instead.
void __fastcall Wp21FireDetour(std::uintptr_t self, void*, std::uintptr_t attack) {
    std::unique_lock lock(g_streamMutex);
    if (self == 0 || self != g_streamUnit || g_streamPath.empty()) {
        lock.unlock();
        g_origWp21Fire(self, attack);
        return;
    }
    SafeStore<std::uint8_t>(self + re5::off::kWp21Firing, 1);
    std::uint32_t old = 0;
    if (SafeRead(self + re5::off::kWp21StreamEffect, old) && old != 0) engine::StopEffect(old);
    SafeStore<std::uint32_t>(self + re5::off::kWp21StreamEffect, 0);
    SafeStore<std::uintptr_t>(self + re5::off::kWp21AttackData, attack);
    RefreshStreamLookLocked(self, true);
    const DWORD now = GetTickCount();
    const DWORD before = g_streamFiredAt.exchange(now);
    if (g_streamFireLogs < 4) {
        ++g_streamFireLogs;
        RM_LOGI("flamethrower: fired with %s in place of its flame (look %u; %lu ms since its last fire)", g_streamPath.c_str(),
                g_streamEffect, before != 0 ? static_cast<unsigned long>(now - before) : 0ul);
    }
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
    }
    if (ok && caps.bossNoRefill) {
        Hook(re5::addr::kFnEm41Update, reinterpret_cast<void*>(&Em41UpdateDetour), reinterpret_cast<void**>(&g_origEm41Update),
             "uEm41 update (a run's boss keeps the damage it takes)");
    }
    if (ok && caps.partnerDeath) {
        Hook(re5::addr::kFnPlayerDeathEvent, reinterpret_cast<void*>(&DeathEventDetour), reinterpret_cast<void**>(&g_origDeathEvent),
             "player execution (a computer partner's does not end the run)");
    }
    if (ok && caps.bossBrain) {
        Hook(re5::addr::kFnEm40CanDive, reinterpret_cast<void*>(&Em40CanDiveDetour), reinterpret_cast<void**>(&g_origEm40CanDive),
             "uEm40 may-dive (a run has one player)");
    }
    if (ok && caps.flameStream) {
        Hook(re5::addr::kFnWp21Fire, reinterpret_cast<void*>(&Wp21FireDetour), reinterpret_cast<void**>(&g_origWp21Fire),
             "uWp21 fire (an element's look in place of the flame)");
    }
    if (ok && caps.hitCheckProbe) {
        Hook(re5::addr::kFnEnemyHitCheck, reinterpret_cast<void*>(&HitCheckDetour), reinterpret_cast<void**>(&g_origHitCheck),
             "enemy hit check (developer probe)");
    }
    g_status.combat = ok;
    return ok;
}

void SetFlameStream(std::uintptr_t unit, const std::string& path, const std::string& companion) {
    std::lock_guard lock(g_streamMutex);
    if (unit == g_streamUnit && path == g_streamPath && companion == g_streamCompanion) return;
    StopStreamLookLocked();
    g_streamUnit = path.empty() ? 0 : unit;
    g_streamPath = g_streamUnit != 0 ? path : std::string();
    g_streamCompanion = g_streamUnit != 0 ? companion : std::string();
    g_streamFiredAt = 0;
    g_streamFireLogs = 0;
}

unsigned long FlameStreamFiredAt() { return g_streamFiredAt.load(); }

void KeepFlameStreamLook(bool firing) {
    std::lock_guard lock(g_streamMutex);
    if (!firing || g_streamUnit == 0 || g_streamPath.empty()) {
        StopStreamLookLocked();
        return;
    }
    if (GetTickCount() - g_streamFiredAt.load() < kStreamLookRestartMs) return;  // its fires start it again
    RefreshStreamLookLocked(g_streamUnit, false);
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

void SetHitProbe(std::uintptr_t unit) {
    g_hitProbeLines = 0;
    g_hitProbe = g_origHitCheck != nullptr ? unit : 0;
}

}  // namespace rmre5
