// Enemy director: spawn gate and spawning. See director.h.

#include "director.h"

#include <windows.h>
#include <intrin.h>

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <set>
#include <cstring>
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
ApplyFn g_origApplyEm10 = nullptr;
thread_local bool t_ownSpawn = false;

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

// Survey probe: the whole descriptor (+0x00..+0x11F: placement, cSetInfoEnemy behaviour fields,
// cSetInfoEm10 look and weapon) of every uEm10-family placement, the game's own and the mod's.
// scripts/survey_spawns.py turns the native ones into the roster's profiles.
int __fastcall ApplyEm10Detour(std::uintptr_t info, void*, std::uintptr_t unit) {
    if (g_survey.load()) {
        char text[900];
        int n = 0;
        for (std::size_t off = 0; off < 0x120 && n >= 0 && n < static_cast<int>(sizeof(text)) - 12; off += 4) {
            n += std::snprintf(text + n, sizeof(text) - n, "%s%08X", off % 0x40 == 0 ? " |" : " ", ReadOr<std::uint32_t>(info + off, 0));
        }
        RM_LOGI("director descriptor %s %s:%s", t_ownSpawn ? "mod" : "native", engine::ClassName(unit).c_str(), text);
    }
    return g_origApplyEm10(info, unit);
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

// Descriptor writes prepared before SpawnNative (no C++ unwinding inside its __try).
struct DescriptorWrite {
    std::uint16_t offset = 0;
    char type = 'i';  // 'b' u8, 'i' u32, 'f' float
    std::uint32_t bits = 0;
};
struct SpawnPlan {
    const char* unitClass = nullptr;
    const char* infoClass = nullptr;
    float pos[3]{};
    bool hasFacing = false;
    float facing = 0.0f;
    bool enemyLayout = false;  // descriptor derives from cSetInfoEnemy
    bool em10Layout = false;   // descriptor derives from cSetInfoEm10
    int count = 0;
    DescriptorWrite writes[40];
};

// A new descriptor only has what its constructor sets; the rest is leftover heap memory
// (seen: mIsScrHitOff = 3, pointers in the placement fields). These ranges are zero in every
// descriptor the game itself loads for Mercenaries (survey 2026-11-01), so they are cleared.
struct ZeroRange {
    std::size_t first;
    std::size_t second;
};
constexpr ZeroRange kEnemyZero[] = {
    {0x0C, 0x20}, {0x2C, 0x40}, {0x4C, 0x50}, {0x54, 0x64}, {0x68, 0x7C}, {0x80, 0xB0}, {0xBC, 0xE0},
};
constexpr ZeroRange kEm10Zero[] = {{0xE0, 0x110}};

std::uintptr_t SpawnNative(const SpawnPlan& plan) {
    __try {
        const auto lookup = reinterpret_cast<LookupFn>(re5::addr::kFnClassLookup);
        const std::uintptr_t unitDti = lookup(plan.unitClass, re5::addr::kDtiLookupUnitBase);
        const std::uintptr_t infoDti = lookup(plan.infoClass, re5::addr::kDtiSetInfoPawn);
        if (!unitDti || !infoDti) return 0;
        const auto newInstance = *reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(infoDti) + 4);
        const std::uintptr_t info = reinterpret_cast<NewFn>(newInstance)(infoDti);
        if (!info) return 0;
        if (plan.enemyLayout) {
            for (const auto& range : kEnemyZero) std::memset(reinterpret_cast<void*>(info + range.first), 0, range.second - range.first);
            // As in the game's Mercenaries descriptors: mIsForNet and mIsFindPL set, so the
            // enemy starts out knowing where the player is and comes for them.
            *reinterpret_cast<std::uint8_t*>(info + re5::off::kSetInfoIsForNet) = 1;
            *reinterpret_cast<std::uint8_t*>(info + re5::off::kSetInfoIsFindPl) = 1;
        }
        if (plan.em10Layout) {
            for (const auto& range : kEm10Zero) std::memset(reinterpret_cast<void*>(info + range.first), 0, range.second - range.first);
            // As in the game's own Majini descriptors: armed (179 of 187 surveyed), mask -1 (all 187).
            *reinterpret_cast<std::uint8_t*>(info + re5::off::kSetInfoEm10Armed) = 1;
            *reinterpret_cast<std::uint32_t*>(info + re5::off::kSetInfoEm10Mask) = 0xFFFFFFFFu;
        }
        *reinterpret_cast<float*>(info + re5::off::kSetInfoPosition) = plan.pos[0];
        *reinterpret_cast<float*>(info + re5::off::kSetInfoPosition + 4) = plan.pos[1];
        *reinterpret_cast<float*>(info + re5::off::kSetInfoPosition + 8) = plan.pos[2];
        if (plan.hasFacing) *reinterpret_cast<float*>(info + re5::off::kSetInfoAngleY) = plan.facing;
        for (int i = 0; i < plan.count; ++i) {
            const DescriptorWrite& w = plan.writes[i];
            if (w.type == 'b') {
                *reinterpret_cast<std::uint8_t*>(info + w.offset) = static_cast<std::uint8_t>(w.bits);
            } else {
                *reinterpret_cast<std::uint32_t*>(info + w.offset) = w.bits;
            }
        }
        const auto sEnemy = *reinterpret_cast<std::uintptr_t*>(re5::addr::kSEnemyInstance);
        const auto create = *reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(sEnemy) + re5::off::kSEnemyVtableCreate);
        const std::uintptr_t unit = reinterpret_cast<CreateFn>(create)(sEnemy, unitDti, re5::addr::kEnemyUnitGroup, 1, 1);
        if (!unit) return 0;
        *reinterpret_cast<std::int32_t*>(unit + re5::off::kEnemySetGroup) = 0;
        *reinterpret_cast<std::int32_t*>(unit + re5::off::kEnemySetId) = -1;
        const auto apply = *reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(info) + re5::off::kSetInfoVtableApply);
        t_ownSpawn = true;
        reinterpret_cast<ApplyFn>(apply)(info, unit);
        t_ownSpawn = false;
        const auto linker = *reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(re5::addr::kLinkUnitRoot) + re5::off::kLinkUnitManager);
        if (linker) reinterpret_cast<LinkFn>(re5::addr::kFnLinkUnits)(linker, unit);
        return unit;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_ownSpawn = false;
        return 0;
    }
}

// Profile fields (maps.json "profiles", from the survey) -> descriptor offsets. The cSetInfoEm10
// ones (FUN_00CC6C20) only apply to descriptors derived from it; the cSetInfoEnemy ones
// (FUN_00CCE070) to every enemy descriptor.
struct SpawnFieldDef {
    const char* name;
    std::uint16_t offset;
    char type;
    bool em10;
};
constexpr SpawnFieldDef kSpawnFields[] = {
    {"search_type", 0xB0, 'i', false}, {"search_forward", 0xB4, 'f', false}, {"search_circle", 0xB8, 'f', false},
    {"area_keep_type", 0xC0, 'i', false}, {"area_keep_dist", 0xC4, 'f', false},
    {"is_extra", 0xE0, 'b', true},       {"extra_no", 0xE4, 'i', true},       {"is_acting", 0xE8, 'b', true},
    {"armed", 0xE9, 'b', true},          {"arm_num", 0xEC, 'i', true},        {"arm", 0xF0, 'i', true},
    {"arm_set_joint", 0xF4, 'i', true},  {"attribute", 0xF8, 'i', true},      {"plaga", 0xFC, 'i', true},
    {"sub_bomb", 0x100, 'i', true},      {"attack", 0x104, 'i', true},        {"mask", 0x108, 'i', true},
    {"lefty", 0x10C, 'b', true},         {"neck_event", 0x10D, 'b', true},
    // cSetInfoEnemy's mModelType and mMontageNo (FUN_00CCE070, read in game 2026-11-18): the
    // look. A Majini's montage picks its head; uEm21 (the megaphone Majini, one model, em2100) has
    // montage 0 in every placement of the game's (s100, s114, s400) and spawned headless without.
    {"model_type", 0x74, 'i', false},    {"montage", 0x7C, 'i', false},
};
constexpr std::size_t kArmField = 10;  // "arm"

struct ResolvedInfo {
    std::string infoClass;  // empty: cannot spawn
    bool enemyLayout = false;
    bool em10Layout = false;
};

bool DtiDerivesFrom(std::uintptr_t dti, const char* base) {
    for (int depth = 0; dti && depth < 24; ++depth) {
        if (engine::DtiName(dti) == base) return true;
        dti = ReadOr<std::uintptr_t>(dti + re5::off::kDtiParent, 0);
    }
    return false;
}

// The descriptor class that places `unitClass`: its own (uEm22 -> cSetInfoEm22); for the
// Majini variants that have none (uEm11..uEm14, uEm21.., derived from uEm10) the uEm10 one;
// otherwise the generic cSetInfoEnemy. Looked up once per class (game thread only).
const ResolvedInfo& ResolveInfo(const std::string& unitClass) {
    static std::map<std::string, ResolvedInfo> cache;
    if (const auto it = cache.find(unitClass); it != cache.end()) return it->second;
    const auto lookup = reinterpret_cast<LookupFn>(re5::addr::kFnClassLookup);
    ResolvedInfo r;
    const std::string own = "cSetInfo" + unitClass.substr(1);  // uEm10 -> cSetInfoEm10
    std::uintptr_t infoDti = lookup(own.c_str(), re5::addr::kDtiSetInfoPawn);
    if (infoDti) {
        r.infoClass = own;
    } else if (DtiDerivesFrom(lookup(unitClass.c_str(), re5::addr::kDtiLookupUnitBase), "uEm10")) {
        r.infoClass = "cSetInfoEm10";
        infoDti = lookup("cSetInfoEm10", re5::addr::kDtiSetInfoPawn);
    } else if ((infoDti = lookup("cSetInfoEnemy", re5::addr::kDtiSetInfoPawn)) != 0) {
        // Enemies with neither (uEm20, and uEm26 derived from it): the generic enemy descriptor.
        r.infoClass = "cSetInfoEnemy";
    }
    if (infoDti) {
        r.enemyLayout = DtiDerivesFrom(infoDti, "cSetInfoEnemy");
        r.em10Layout = DtiDerivesFrom(infoDti, "cSetInfoEm10");
    }
    RM_LOGI("director: %s is placed with %s (enemy fields %s, Majini fields %s)", unitClass.c_str(),
            r.infoClass.empty() ? "no descriptor, cannot spawn" : r.infoClass.c_str(), r.enemyLayout ? "yes" : "no",
            r.em10Layout ? "yes" : "no");
    return cache.emplace(unitClass, r).first->second;
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
    // The descriptor probe only where the vtable holds the expected function (no signature).
    if (ReadOr<std::uintptr_t>(re5::addr::kSetInfoEm10Vtable + re5::off::kSetInfoVtableApply, 0) == re5::addr::kFnSetInfoEm10Apply) {
        Queue(re5::addr::kFnSetInfoEm10Apply, reinterpret_cast<void*>(&ApplyEm10Detour), reinterpret_cast<void**>(&g_origApplyEm10),
              "cSetInfoEm10 apply");
    }
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

bool Damage(std::uintptr_t unit, int amount) {
    const auto hp = engine::ReadHp(unit);
    if (!hp || hp->hp <= 0 || amount <= 0) return false;
    using SubHpFn = void(__thiscall*)(std::uintptr_t self, std::int16_t amount);
    const auto take = static_cast<std::int16_t>(amount < hp->hp ? amount : hp->hp);
    __try {
        reinterpret_cast<SubHpFn>(re5::addr::kFnCharacterSubHp)(unit, take);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

std::uintptr_t Spawn(const SpawnSpec& spec) {
    if (!engine::Caps().enemySpawn || spec.unitClass.size() < 2 || spec.unitClass[0] != 'u') return 0;
    const ResolvedInfo& info = ResolveInfo(spec.unitClass);
    if (info.infoClass.empty()) return 0;
    SpawnPlan plan;
    plan.unitClass = spec.unitClass.c_str();
    plan.infoClass = info.infoClass.c_str();
    std::memcpy(plan.pos, spec.pos, sizeof(plan.pos));
    plan.hasFacing = spec.facing.has_value();
    plan.facing = spec.facing.value_or(0.0f);
    plan.enemyLayout = info.enemyLayout;
    plan.em10Layout = info.em10Layout;
    auto add = [&](const SpawnFieldDef& def, double value) {
        if (plan.count >= static_cast<int>(std::size(plan.writes))) return;
        DescriptorWrite& w = plan.writes[plan.count++];
        w.offset = def.offset;
        w.type = def.type;
        if (def.type == 'f') {
            const float f = static_cast<float>(value);
            std::memcpy(&w.bits, &f, sizeof(f));
        } else {
            w.bits = static_cast<std::uint32_t>(static_cast<std::int64_t>(value));
        }
    };
    for (const auto& [name, value] : spec.fields) {
        const SpawnFieldDef* def = nullptr;
        for (const SpawnFieldDef& d : kSpawnFields) {
            if (name == d.name) def = &d;
        }
        if (!def) {
            static std::set<std::string> warned;
            if (warned.insert(name).second) RM_LOGW("director: unknown spawn field '%s' ignored", name.c_str());
            continue;
        }
        if ((def->em10 && !info.em10Layout) || (!def->em10 && !info.enemyLayout)) continue;
        add(*def, value);
    }
    // The weapon field (+0xF0 mArmId) is verified for cSetInfoEm10 (and what derives from it).
    if (spec.weapon >= 0 && info.em10Layout) add(kSpawnFields[kArmField], spec.weapon);
    return SpawnNative(plan);
}

std::uintptr_t Spawn(const char* unitClass, const float pos[3], int weapon) {
    SpawnSpec spec;
    spec.unitClass = unitClass ? unitClass : "";
    std::memcpy(spec.pos, pos, sizeof(spec.pos));
    spec.weapon = weapon;
    return Spawn(spec);
}

}  // namespace rmre5::director
