// World objects. See world.h.

#include "world.h"

#include <windows.h>
#include <intrin.h>

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "engine.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5::world {
namespace {

using SetSpawnFn = std::uintptr_t(__thiscall*)(std::uintptr_t self, int group, int id);
using CreateFn = std::uintptr_t(__thiscall*)(std::uintptr_t self, std::uintptr_t dti, int group, int mode, int flag);
using RecordFn = std::uintptr_t(__thiscall*)(std::uintptr_t setManager, int group, int id);
using LookupFn = std::uintptr_t(__cdecl*)(const char* name, std::uintptr_t baseDti);
using NewFn = std::uintptr_t(__thiscall*)(std::uintptr_t dti);
using ApplyFn = void(__thiscall*)(std::uintptr_t info, std::uintptr_t unit);
using LinkFn = void(__thiscall*)(std::uintptr_t linker, std::uintptr_t unit);

// The game's own time-bonus statues (survey 2026-11-02, The Mines: 8 x uOm0100 from set group
// 61): cSetInfoInsModel dwords from +0x04 (+0x0C stays the constructor's), position +0x20,
// rotation +0x30, scale +0x40, bonus seconds +0x64, +0x74 = 0x8020.
constexpr std::uint32_t kStatueDescriptor[32] = {
    0, 0, 0, 0, 0, 0, 0, 0,                                         // +0x00..+0x1C (+0, +0xC skipped)
    0, 0, 0, 0, 0, 0, 0, 0,                                         // +0x20 position, +0x30 rotation
    0x3F800000, 0x3F800000, 0x3F800000, 0, 4, 0, 0, 0,              // +0x40 scale, +0x50 4
    1, 30, 0, 0, 0, 0x8020, 0, 0,                                   // +0x60 1, +0x64 seconds, +0x74
};
constexpr std::size_t kStatuePosition = 0x20;
constexpr std::size_t kStatueSeconds = 0x64;
constexpr int kObjectGroup = 12;  // what the set spawn passes to sObjModel::Create for statues

// Two set managers place the stage's objects: sObjModel (map objects) and sItem (items).
constexpr const char* kManagerNames[] = {"sObjModel", "sItem"};
SetSpawnFn g_origSetSpawn[2] = {};
CreateFn g_origCreate[2] = {};
std::atomic<bool> g_available{false};
std::atomic<bool> g_survey{false};
std::atomic<bool> g_blockChests{false};
std::atomic<int> g_blockedChests{0};
constexpr const char* kChestClass = "uOm0100";

std::string DtiName(std::uintptr_t dti) {
    const auto name = dti ? ReadOr<std::uintptr_t>(dti + 4, 0) : 0;
    return name ? ReadCString(name, 64) : std::string("?");
}

std::uintptr_t Record(std::uintptr_t manager, int group, int id) {
    __try {
        return reinterpret_cast<RecordFn>(re5::addr::kFnSetRecord)(manager, group, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Survey line (one per object) and the descriptor's dwords 0x00..0x7F.
void LogObject(int m, std::uintptr_t manager, int group, int id, std::uintptr_t unit) {
    const std::uintptr_t record = Record(manager, group, id);
    const auto descriptor = record ? ReadOr<std::uintptr_t>(record + 0x10, 0) : 0;
    RM_LOGI("world survey: %s (%s) set %d id %d at %.1f %.1f %.1f | +6 %u +20 %u +120 %d +124 %d +128 %d +D78 %d +FAC %u +1364 %d | %s",
            engine::ClassName(unit).c_str(), kManagerNames[m], group, id, ReadOr<float>(unit + re5::off::kUnitPosition, 0.0f),
            ReadOr<float>(unit + re5::off::kUnitPosition + 4, 0.0f), ReadOr<float>(unit + re5::off::kUnitPosition + 8, 0.0f),
            ReadOr<std::uint8_t>(unit + 6, 0), ReadOr<std::uint16_t>(unit + 0x20, 0), ReadOr<std::int32_t>(unit + 0x120, 0),
            ReadOr<std::int32_t>(unit + 0x124, 0), ReadOr<std::int32_t>(unit + 0x128, 0), ReadOr<std::int32_t>(unit + 0xD78, 0),
            ReadOr<std::uint8_t>(unit + 0xFAC, 0), ReadOr<std::int16_t>(unit + 0x1364, 0),
            descriptor ? engine::ClassName(descriptor).c_str() : "no descriptor");
    if (!descriptor) return;
    std::string dump;
    for (std::size_t off = 0; off < 0x80; off += 4) {
        char buf[12];
        std::snprintf(buf, sizeof(buf), " %08X", ReadOr<std::uint32_t>(descriptor + off, 0));
        dump += buf;
    }
    RM_LOGI("world descriptor %s:%s", engine::ClassName(unit).c_str(), dump.c_str());
}

std::uintptr_t PlaceNative(const float* pos, int seconds) {
    __try {
        const auto lookup = reinterpret_cast<LookupFn>(re5::addr::kFnClassLookup);
        const std::uintptr_t unitDti = lookup(kChestClass, re5::addr::kDtiLookupUnitBase);
        const std::uintptr_t infoDti = lookup("cSetInfoInsModel", re5::addr::kDtiSetInfoPawn);
        const auto manager = *reinterpret_cast<std::uintptr_t*>(re5::addr::kObjModelManager);
        if (!unitDti || !infoDti || !manager) return 0;
        const auto newInstance = *reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(infoDti) + 4);
        const std::uintptr_t info = reinterpret_cast<NewFn>(newInstance)(infoDti);
        if (!info) return 0;
        for (std::size_t i = 1; i < 32; ++i) {
            if (i == 3) continue;
            *reinterpret_cast<std::uint32_t*>(info + i * 4) = kStatueDescriptor[i];
        }
        for (int k = 0; k < 3; ++k) *reinterpret_cast<float*>(info + kStatuePosition + k * 4) = pos[k];
        *reinterpret_cast<std::int32_t*>(info + kStatueSeconds) = seconds;
        const auto create = *reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(manager) + 11 * 4);
        const std::uintptr_t unit = reinterpret_cast<CreateFn>(create)(manager, unitDti, kObjectGroup, 1, 1);
        if (!unit) return 0;
        *reinterpret_cast<std::int32_t*>(unit + re5::off::kEnemySetGroup) = 0;
        *reinterpret_cast<std::int32_t*>(unit + re5::off::kEnemySetId) = -1;
        const auto apply = *reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(info) + re5::off::kSetInfoVtableApply);
        reinterpret_cast<ApplyFn>(apply)(info, unit);
        // What the set spawn writes after the descriptor (0x007E28FA..0x007E294C), with the
        // values the game's own statues end up with.
        *reinterpret_cast<std::int32_t*>(unit + 0xD78) = 1;
        if (*reinterpret_cast<std::uint8_t*>(unit + 0xFAC) == 0) {
            *reinterpret_cast<std::uint32_t*>(unit + 0x120) = 0x8020;
            *reinterpret_cast<std::uint8_t*>(unit + 0xFAC) = 1;
        }
        *reinterpret_cast<std::uint8_t*>(unit + 6) = 13;
        *reinterpret_cast<std::int32_t*>(unit + 0x124) = 3;
        *reinterpret_cast<std::int32_t*>(unit + 0x128) = 1;
        *reinterpret_cast<std::uint16_t*>(unit + 0x20) = 4;
        const auto linker = *reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(re5::addr::kLinkUnitRoot) + re5::off::kLinkUnitManager);
        if (linker) reinterpret_cast<LinkFn>(re5::addr::kFnLinkUnits)(linker, unit);
        return unit;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The class a set record names (record +0xC holds the name at +8).
std::string RecordClass(std::uintptr_t manager, int group, int id) {
    const std::uintptr_t record = Record(manager, group, id);
    const auto holder = record ? ReadOr<std::uintptr_t>(record + 0xC, 0) : 0;
    return holder ? ReadCString(holder + 8, 64) : std::string();
}

template <int M>
std::uintptr_t __fastcall SetSpawnDetour(std::uintptr_t self, void*, int group, int id) {
    if constexpr (M == 0) {
        if (g_blockChests.load() && RecordClass(self, group, id) == kChestClass) {
            if (g_blockedChests.fetch_add(1) < 2) RM_LOGI("world: the stage's own time bonus (set %d id %d) not created", group, id);
            return 0;
        }
    }
    const std::uintptr_t unit = g_origSetSpawn[M](self, group, id);
    if (unit && g_survey.load()) LogObject(M, self, group, id, unit);
    return unit;
}

template <int M>
std::uintptr_t __fastcall CreateDetour(std::uintptr_t self, void*, std::uintptr_t dti, int group, int mode, int flag) {
    const std::uintptr_t unit = g_origCreate[M](self, dti, group, mode, flag);
    if (g_survey.load()) {
        RM_LOGI("world create: %s (%s) group %d mode %d flag %d -> %08X", DtiName(dti).c_str(), kManagerNames[M], group,
                mode, flag, static_cast<unsigned>(unit));
    }
    return unit;
}

using CtorFn = std::uintptr_t(__thiscall*)(std::uintptr_t self);
CtorFn g_origHourGlassCtor = nullptr;

// Survey: who constructs a time-bonus statue (code addresses found on the stack above us).
std::uintptr_t __fastcall HourGlassCtorDetour(std::uintptr_t self, void*) {
    const std::uintptr_t result = g_origHourGlassCtor(self);
    if (g_survey.load()) {
        const auto* frame = reinterpret_cast<const std::uintptr_t*>(_AddressOfReturnAddress());
        std::string callers;
        int found = 0;
        for (int i = 0; i < 96 && found < 10; ++i) {
            std::uintptr_t value = 0;
            if (!SafeRead(reinterpret_cast<std::uintptr_t>(frame + i), value)) break;
            if (value >= 0x00401000 && value < 0x00F4A000) {
                char buf[12];
                std::snprintf(buf, sizeof(buf), " %08X", static_cast<unsigned>(value));
                callers += buf;
                ++found;
            }
        }
        RM_LOGI("world survey: uOmHourGlass constructed at %08X; stack code addresses:%s", static_cast<unsigned>(self),
                callers.c_str());
    }
    return result;
}

bool Queue(std::uintptr_t address, void* detour, void** original, const char* what) {
    auto* target = reinterpret_cast<void*>(address);
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_OK) status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        RM_LOGE("world: hook %s at 0x%08X failed: %s", what, static_cast<unsigned>(address), MH_StatusToString(status));
        return false;
    }
    return true;
}

}  // namespace

bool Install() {
    if (!engine::Caps().worldObjects) {
        RM_LOGW("world: object manager signatures missing; no object hooks");
        return false;
    }
    const bool ok = Queue(re5::addr::kFnObjSetSpawn, reinterpret_cast<void*>(&SetSpawnDetour<0>),
                          reinterpret_cast<void**>(&g_origSetSpawn[0]), "sObjModel set spawn") &&
                    Queue(re5::addr::kFnObjCreate, reinterpret_cast<void*>(&CreateDetour<0>),
                          reinterpret_cast<void**>(&g_origCreate[0]), "sObjModel::Create") &&
                    Queue(re5::addr::kFnItemSetSpawn, reinterpret_cast<void*>(&SetSpawnDetour<1>),
                          reinterpret_cast<void**>(&g_origSetSpawn[1]), "sItem set spawn") &&
                    Queue(re5::addr::kFnItemCreate, reinterpret_cast<void*>(&CreateDetour<1>),
                          reinterpret_cast<void**>(&g_origCreate[1]), "sItem::Create") &&
                    Queue(re5::addr::kFnHourGlassCtor, reinterpret_cast<void*>(&HourGlassCtorDetour),
                          reinterpret_cast<void**>(&g_origHourGlassCtor), "uOmHourGlass constructor");
    g_available = ok;
    if (g_available) RM_LOGI("world: hooks on the set spawns and creates of sObjModel and sItem queued");
    return g_available;
}

bool Available() { return g_available.load(); }

void SetSurvey(bool on) { g_survey = on; }

void BlockNativeChests(bool blocked) {
    if (g_blockChests.exchange(blocked) != blocked) {
        RM_LOGI("world: the stage's own time bonuses %s", blocked ? "blocked" : "allowed");
        if (!blocked) g_blockedChests = 0;
    }
}

std::uintptr_t PlaceChest(const float pos[3], int seconds) {
    if (!g_available.load()) return 0;
    const std::uintptr_t unit = PlaceNative(pos, seconds);
    if (unit) {
        RM_LOGI("world: chest %s %08X at %.0f %.0f %.0f | +6 %u +20 %u +120 %d +124 %d +128 %d +D78 %d +FAC %u +1364 %d",
                engine::ClassName(unit).c_str(), static_cast<unsigned>(unit), pos[0], pos[1], pos[2],
                ReadOr<std::uint8_t>(unit + 6, 0), ReadOr<std::uint16_t>(unit + 0x20, 0), ReadOr<std::int32_t>(unit + 0x120, 0),
                ReadOr<std::int32_t>(unit + 0x124, 0), ReadOr<std::int32_t>(unit + 0x128, 0), ReadOr<std::int32_t>(unit + 0xD78, 0),
                ReadOr<std::uint8_t>(unit + 0xFAC, 0), ReadOr<std::int16_t>(unit + 0x1364, 0));
    } else {
        RM_LOGW("world: chest at %.0f %.0f %.0f not placed", pos[0], pos[1], pos[2]);
    }
    return unit;
}

}  // namespace rmre5::world
