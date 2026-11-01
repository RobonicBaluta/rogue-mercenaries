// Enemy director: spawn gate and spawning. See director.h.

#include "director.h"

#include <windows.h>
#include <intrin.h>

#include <MinHook.h>

#include <atomic>
#include <map>
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
std::atomic<bool> g_survey{false};

// The game's set spawn: refused while the mod owns the arena.
std::uintptr_t __fastcall SetSpawnDetour(std::uintptr_t self, void*, int group, int id, int flag) {
    if (g_blocked.load()) {
        if (g_blockedAttempts.fetch_add(1) < 3) RM_LOGI("director: native spawn refused (set %d, id %d)", group, id);
        return 0;
    }
    const std::uintptr_t unit = g_origSetSpawn(self, group, id, flag);
    if (unit && g_survey.load()) {
        // Survey line (scripts/survey_spawns.py): class, weapon id, set record, position.
        RM_LOGI("director survey: %s arm %X num %d set %d id %d at %.1f %.1f %.1f", engine::ClassName(unit).c_str(),
                ReadOr<std::uint32_t>(unit + re5::off::kEnemyArmId, 0), ReadOr<std::int32_t>(unit + re5::off::kEnemyArmId + 4, 0), group,
                id, ReadOr<float>(unit + re5::off::kUnitPosition, 0.0f), ReadOr<float>(unit + re5::off::kUnitPosition + 4, 0.0f),
                ReadOr<float>(unit + re5::off::kUnitPosition + 8, 0.0f));
    }
    return unit;
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

void SetSurvey(bool on) { g_survey = on; }

bool Kill(std::uintptr_t unit) {
    const auto hp = engine::ReadHp(unit);
    if (!hp || hp->hp <= 0) return false;
    using SubHpFn = void(__thiscall*)(std::uintptr_t self, std::int16_t amount);
    __try {
        reinterpret_cast<SubHpFn>(re5::addr::kFnCharacterSubHp)(unit, static_cast<std::int16_t>(hp->hp));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// The descriptor class that places `unitClass`: its own (uEm22 -> cSetInfoEm22), or for the
// Majini variants that have none (uEm11..uEm14, uEm20.., all derived from uEm10) the uEm10
// one. Empty when there is neither. Looked up once per class (game thread only).
static std::string InfoClassFor(const char* unitClass) {
    static std::map<std::string, std::string> cache;
    if (const auto it = cache.find(unitClass); it != cache.end()) return it->second;
    const auto lookup = reinterpret_cast<LookupFn>(re5::addr::kFnClassLookup);
    std::string info = std::string("cSetInfo") + (unitClass + 1);  // uEm10 -> cSetInfoEm10
    std::string chosen;
    if (lookup(info.c_str(), re5::addr::kDtiSetInfoPawn)) {
        chosen = info;
    } else {
        std::uintptr_t dti = lookup(unitClass, re5::addr::kDtiLookupUnitBase);
        for (int depth = 0; dti && depth < 24 && chosen.empty(); ++depth) {
            if (engine::DtiName(dti) == "uEm10") chosen = "cSetInfoEm10";
            dti = ReadOr<std::uintptr_t>(dti + re5::off::kDtiParent, 0);
        }
    }
    RM_LOGI("director: %s is placed with %s", unitClass, chosen.empty() ? "no descriptor (cannot spawn)" : chosen.c_str());
    cache.emplace(unitClass, chosen);
    return chosen;
}

std::uintptr_t Spawn(const char* unitClass, const float pos[3], int weapon) {
    if (!engine::Caps().enemySpawn || !unitClass || unitClass[0] != 'u') return 0;
    const std::string info = InfoClassFor(unitClass);
    if (info.empty()) return 0;
    // The weapon field (+0xF0 mArmId) is verified for cSetInfoEm10 only; other descriptors
    // keep their default arms.
    if (info != "cSetInfoEm10") weapon = -1;
    return SpawnNative(unitClass, info.c_str(), pos, weapon);
}

}  // namespace rmre5::director
