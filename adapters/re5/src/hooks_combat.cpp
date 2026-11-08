#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdint>

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
    if (ok && caps.hitCheckProbe) {
        Hook(re5::addr::kFnEnemyHitCheck, reinterpret_cast<void*>(&HitCheckDetour), reinterpret_cast<void**>(&g_origHitCheck),
             "enemy hit check (developer probe)");
    }
    g_status.combat = ok;
    return ok;
}

void SetBossTakesGunfire(std::uintptr_t unit) {
    if (g_gunfireBoss.exchange(unit) != unit) g_gunfireLogged = 0;
}

void SetHitProbe(std::uintptr_t unit) {
    g_hitProbeLines = 0;
    g_hitProbe = g_origHitCheck != nullptr ? unit : 0;
}

}  // namespace rmre5
