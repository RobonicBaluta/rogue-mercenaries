// Enemy director: spawn gate and spawning. See director.h.

#include "director.h"

#include <windows.h>
#include <intrin.h>

#include <MinHook.h>

#include <atomic>
#include <string>

#include "engine.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5::director {
namespace {

using SetSpawnFn = std::uintptr_t(__thiscall*)(std::uintptr_t self, int group, int id, int flag);
using CreateFn = std::uintptr_t(__thiscall*)(std::uintptr_t self, std::uintptr_t dti, int group, int mode, int flag);
using LookupFn = std::uintptr_t(__cdecl*)(const char* name, std::uintptr_t baseDti);
using NewFn = std::uintptr_t(__thiscall*)(std::uintptr_t dti);
using ApplyFn = int(__thiscall*)(std::uintptr_t info, std::uintptr_t unit);
using LinkFn = void(__thiscall*)(std::uintptr_t self, std::uintptr_t unit);

SetSpawnFn g_origSetSpawn = nullptr;
CreateFn g_origCreate = nullptr;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_blocked{false};
std::atomic<int> g_blockedAttempts{0};
std::atomic<int> g_createLogged{0};

// The game's set spawn: refused while the mod owns the arena.
std::uintptr_t __fastcall SetSpawnDetour(std::uintptr_t self, void*, int group, int id, int flag) {
    if (g_blocked.load()) {
        if (g_blockedAttempts.fetch_add(1) < 3) RM_LOGI("director: native spawn refused (set %d, id %d)", group, id);
        return 0;
    }
    return g_origSetSpawn(self, group, id, flag);
}

// Debug probe: class, arguments and caller of every enemy creation.
std::uintptr_t __fastcall CreateDetour(std::uintptr_t self, void*, std::uintptr_t dti, int group, int mode, int flag) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const std::uintptr_t unit = g_origCreate(self, dti, group, mode, flag);
    if (g_createLogged.fetch_add(1) < 60) {
        const auto name = ReadOr<std::uintptr_t>(dti + 4, 0);
        RM_LOGD("sEnemy::Create %s group %d mode %d flag %d -> %08X (caller %08X)", name ? ReadCString(name, 32).c_str() : "?",
                group, mode, flag & 0xFF, static_cast<unsigned>(unit), static_cast<unsigned>(caller));
    }
    return unit;
}

bool Queue(std::uintptr_t address, void* detour, void** original, const char* what) {
    auto* target = reinterpret_cast<void*>(address);
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_OK) status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        RM_LOGE("director: hook %s at 0x%08X failed: %s", what, static_cast<unsigned>(address), MH_StatusToString(status));
        return false;
    }
    return true;
}

std::uintptr_t SpawnNative(const char* unitClass, const char* infoClass, const float* pos, int weapon) {
    __try {
        const auto lookup = reinterpret_cast<LookupFn>(re5::addr::kFnClassLookup);
        const std::uintptr_t unitDti = lookup(unitClass, re5::addr::kDtiLookupUnitBase);
        const std::uintptr_t infoDti = lookup(infoClass, re5::addr::kDtiSetInfoPawn);
        if (!unitDti || !infoDti) return 0;
        const auto newInstance = *reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(infoDti) + 4);
        const std::uintptr_t info = reinterpret_cast<NewFn>(newInstance)(infoDti);
        if (!info) return 0;
        *reinterpret_cast<float*>(info + re5::off::kSetInfoPosition) = pos[0];
        *reinterpret_cast<float*>(info + re5::off::kSetInfoPosition + 4) = pos[1];
        *reinterpret_cast<float*>(info + re5::off::kSetInfoPosition + 8) = pos[2];
        if (weapon >= 0) *reinterpret_cast<std::int32_t*>(info + re5::off::kSetInfoEm10Weapon) = weapon;
        const auto sEnemy = *reinterpret_cast<std::uintptr_t*>(re5::addr::kSEnemyInstance);
        const auto create = *reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(sEnemy) + re5::off::kSEnemyVtableCreate);
        const std::uintptr_t unit = reinterpret_cast<CreateFn>(create)(sEnemy, unitDti, re5::addr::kEnemyUnitGroup, 1, 1);
        if (!unit) return 0;
        *reinterpret_cast<std::int32_t*>(unit + re5::off::kEnemySetGroup) = 0;
        *reinterpret_cast<std::int32_t*>(unit + re5::off::kEnemySetId) = -1;
        const auto apply = *reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(info) + re5::off::kSetInfoVtableApply);
        reinterpret_cast<ApplyFn>(apply)(info, unit);
        const auto linker = *reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(re5::addr::kLinkUnitRoot) + re5::off::kLinkUnitManager);
        if (linker) reinterpret_cast<LinkFn>(re5::addr::kFnLinkUnits)(linker, unit);
        return unit;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

}  // namespace

bool Install() {
    if (!engine::Caps().enemySpawn) {
        RM_LOGW("director: spawn signatures missing; native spawns cannot be controlled");
        return false;
    }
    const bool gate = Queue(re5::addr::kFnEnemySetSpawn, reinterpret_cast<void*>(&SetSpawnDetour),
                            reinterpret_cast<void**>(&g_origSetSpawn), "set spawn");
    Queue(re5::addr::kFnEnemyCreate, reinterpret_cast<void*>(&CreateDetour), reinterpret_cast<void**>(&g_origCreate),
          "sEnemy::Create");
    g_installed = gate;
    if (gate) RM_LOGI("director: hooks on the set spawn and sEnemy::Create queued");
    return gate;
}

bool Available() { return g_installed.load(); }

void BlockNativeSpawns(bool blocked) {
    if (g_blocked.exchange(blocked) != blocked) RM_LOGI("director: native spawns %s", blocked ? "blocked" : "allowed");
}

bool NativeSpawnsBlocked() { return g_blocked.load(); }

int TakeBlockedAttempts() { return g_blockedAttempts.exchange(0); }

std::uintptr_t Spawn(const char* unitClass, const float pos[3], int weapon) {
    if (!engine::Caps().enemySpawn || !unitClass || unitClass[0] != 'u') return 0;
    const std::string info = std::string("cSetInfo") + (unitClass + 1);  // uEm10 -> cSetInfoEm10
    return SpawnNative(unitClass, info.c_str(), pos, weapon);
}

}  // namespace rmre5::director
