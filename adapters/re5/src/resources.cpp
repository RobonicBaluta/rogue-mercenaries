// Archive preloading. See resources.h.

#include "resources.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include "engine.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5::resources {
namespace {

using CommitFn = int(__thiscall*)(std::uintptr_t loader);
using LoadClassFn = void(__thiscall*)(std::uintptr_t loader, std::uintptr_t dti, int flag);
using LoadUnitFn = void(__thiscall*)(std::uintptr_t loader, int category, unsigned id, unsigned variant, int flag);
using LookupFn = std::uintptr_t(__cdecl*)(const char* name, std::uintptr_t baseDti);
using ByIdFn = std::uintptr_t(__cdecl*)(std::uint32_t classId);

// Loader ([kResourceLoader]): list n has its request count at +0x2070C + n*0x14 and its
// request array at +0x20718 + n*0x14; +0x207D0 is the list being built. A request (0x54
// bytes) holds the path at +4, the loaded resource at +0x44, its state at +0x48 (0 not asked
// for this time, 1/4 new, 3 kept) and the class at +0x4C.
constexpr std::size_t kListCount = 9;
constexpr std::size_t kListSize = 0x2070C;
constexpr std::size_t kListArray = 0x20718;
constexpr std::size_t kListStride = 0x14;
constexpr std::size_t kCurrentList = 0x207D0;
constexpr std::size_t kRequestPath = 0x4;
constexpr std::size_t kRequestResource = 0x44;
constexpr std::size_t kRequestState = 0x48;
constexpr int kPlayerList = 3;
constexpr int kEnemyList = 4;
constexpr int kWeaponCategory = 7;  // uWpNN: category 7, id = the weapon's item id 0x01NN

CommitFn g_origCommit = nullptr;
std::atomic<bool> g_available{false};
std::mutex g_mutex;
Extras g_run;
Extras g_dev;

bool InMercenaries() {
    const auto app = ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0);
    const auto work = app ? ReadOr<std::uintptr_t>(app + re5::off::kGameWork, 0) : 0;
    const int mode = work ? ReadOr<std::int32_t>(work + re5::off::kGameMode, -1) : -1;
    return mode == 2 || mode == 5;
}

bool QueueClass(std::uintptr_t loader, const char* name) {
    __try {
        const auto lookup = reinterpret_cast<LookupFn>(re5::addr::kFnClassLookup);
        const std::uintptr_t dti = lookup(name, re5::addr::kDtiLookupUnitBase);
        if (!dti) return false;
        reinterpret_cast<LoadClassFn>(re5::addr::kFnLoadClassArchive)(loader, dti, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool QueueUnit(std::uintptr_t loader, int category, unsigned id) {
    __try {
        reinterpret_cast<LoadUnitFn>(re5::addr::kFnLoadUnitArchive)(loader, category, id, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
void AddUnique(std::vector<T>& to, const std::vector<T>& from) {
    for (const T& v : from) {
        if (std::find(to.begin(), to.end(), v) == to.end()) to.push_back(v);
    }
}

int __fastcall CommitDetour(std::uintptr_t loader, void*) {
    const int list = ReadOr<std::int32_t>(loader + kCurrentList, -1);
    if ((list == kPlayerList || list == kEnemyList) && InMercenaries()) {
        Extras extras;
        {
            std::lock_guard lock(g_mutex);
            extras = g_run;
            AddUnique(extras.enemyClasses, g_dev.enemyClasses);
            AddUnique(extras.enemyWeapons, g_dev.enemyWeapons);
            AddUnique(extras.playerWeapons, g_dev.playerWeapons);
        }
        // Requests already in the list are only kept (the loader skips duplicates).
        std::string done;
        int failed = 0;
        if (list == kEnemyList) {
            for (const std::string& name : extras.enemyClasses) {
                if (QueueClass(loader, name.c_str())) done += " " + name;
                else ++failed;
            }
            for (const std::uint32_t weapon : extras.enemyWeapons) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), " %X", weapon);
                if (QueueUnit(loader, static_cast<int>(weapon >> 16), weapon & 0xFFFF)) done += buf;
                else ++failed;
            }
        } else {
            for (const std::uint16_t item : extras.playerWeapons) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), " %04X", item);
                if (QueueUnit(loader, kWeaponCategory, item)) done += buf;
                else ++failed;
            }
        }
        if (!done.empty() || failed) {
            RM_LOGI("resources: list %d (%s) extras queued:%s%s", list, list == kEnemyList ? "enemies" : "player items",
                    done.c_str(), failed ? " (some unknown)" : "");
        }
    }
    return g_origCommit(loader);
}

std::uintptr_t Loader() { return ReadOr<std::uintptr_t>(re5::addr::kResourceLoader, 0); }

template <typename Visit>
void ForEachRequest(Visit&& visit) {
    const std::uintptr_t loader = Loader();
    if (!loader) return;
    for (std::size_t n = 0; n < kListCount; ++n) {
        const int count = ReadOr<std::int32_t>(loader + kListSize + n * kListStride, 0);
        const auto array = ReadOr<std::uintptr_t>(loader + kListArray + n * kListStride, 0);
        if (count <= 0 || count > 1024 || !array) continue;
        for (int i = 0; i < count; ++i) {
            const auto request = ReadOr<std::uintptr_t>(array + static_cast<std::size_t>(i) * 4, 0);
            if (request) visit(static_cast<int>(n), request);
        }
    }
}

}  // namespace

bool Install() {
    if (!engine::Caps().resources) {
        RM_LOGW("resources: loader signatures missing; no preloading");
        return false;
    }
    auto* target = reinterpret_cast<void*>(re5::addr::kFnLoadListCommit);
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&CommitDetour), reinterpret_cast<void**>(&g_origCommit));
    if (status == MH_OK) status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        RM_LOGE("resources: hook on the load list commit failed: %s", MH_StatusToString(status));
        return false;
    }
    g_available = true;
    RM_LOGI("resources: hook on the load list commit queued");
    return true;
}

bool Available() { return g_available.load(); }

void SetRunExtras(Extras extras) {
    std::lock_guard lock(g_mutex);
    g_run = std::move(extras);
}

void SetDevExtras(Extras extras) {
    std::lock_guard lock(g_mutex);
    g_dev = std::move(extras);
}

namespace {
std::uintptr_t ClassById(std::uint32_t classId) {
    __try {
        return reinterpret_cast<ByIdFn>(re5::addr::kFnClassById)(classId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
}  // namespace

std::string ClassNameById(std::uint32_t classId) {
    const std::uintptr_t dti = ClassById(classId);
    const auto name = dti ? ReadOr<std::uintptr_t>(dti + 4, 0) : 0;
    return name ? ReadCString(name, 64) : std::string();
}

bool WeaponLoaded(std::uint16_t item) {
    const std::string name = ClassNameById(static_cast<std::uint32_t>(kWeaponCategory) << 24 | static_cast<std::uint32_t>(item) << 8);
    return !name.empty() && ArchiveLoaded(name);
}

bool ArchiveLoaded(const std::string& name) {
    const std::string path = "Image\\Archive\\" + name;
    bool loaded = false;
    ForEachRequest([&](int, std::uintptr_t request) {
        if (!loaded && _stricmp(ReadCString(request + kRequestPath, 64).c_str(), path.c_str()) == 0) {
            loaded = ReadOr<std::uintptr_t>(request + kRequestResource, 0) != 0;
        }
    });
    return loaded;
}

void LogLists() {
    const std::uintptr_t loader = Loader();
    RM_LOGI("resources: loader %08X, list being built %d", static_cast<unsigned>(loader),
            loader ? ReadOr<std::int32_t>(loader + kCurrentList, -1) : -1);
    ForEachRequest([](int list, std::uintptr_t request) {
        RM_LOGI("resources: list %d %-40s state %d resource %08X", list, ReadCString(request + kRequestPath, 64).c_str(),
                ReadOr<std::int32_t>(request + kRequestState, -1),
                static_cast<unsigned>(ReadOr<std::uintptr_t>(request + kRequestResource, 0)));
    });
}

}  // namespace rmre5::resources
