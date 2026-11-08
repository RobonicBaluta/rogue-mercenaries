// Campaign stage setup fixes. See stage_fixes.h.

#include "stage_fixes.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "engine.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5::stagefix {
namespace {

using SetupFn = void(__fastcall*)(std::uintptr_t scene, void* edx);
using FindModelFn = std::uintptr_t(__thiscall*)(std::uintptr_t list, int kind, int id);

SetupFn g_origS308Setup = nullptr;
std::atomic<bool> g_inRun{false};

// The player's flamethrower as the stage looks it up: the model list's uBioModel of kind 0, id 1
// that is a uWp21 (0: none, or the lookup faulted).
std::uintptr_t CallFindModel(std::uintptr_t list, int kind, int id) {
    __try {
        return reinterpret_cast<FindModelFn>(re5::addr::kFnFindModel)(list, kind, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::uintptr_t FindFlamethrower() {
    const auto list = ReadOr<std::uintptr_t>(re5::addr::kModelList, 0);
    const std::uintptr_t model = list ? CallFindModel(list, 0, 1) : 0;
    return model && engine::ClassName(model) == "uWp21" ? model : 0;
}

bool BaseSetup(std::uintptr_t scene) {
    __try {
        reinterpret_cast<SetupFn>(re5::addr::kFnSceneBaseSetup)(scene, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall S308SetupDetour(std::uintptr_t scene, void* edx) {
    const bool inRun = g_inRun.load();
    const std::uintptr_t flamethrower = FindFlamethrower();
    if (!inRun && flamethrower) {
        g_origS308Setup(scene, edx);
        return;
    }
    const bool ok = BaseSetup(scene);
    RM_LOGI("stage fix: s308's own setup skipped (%s); its base setup %s", inRun ? "a run's arena" : "no flamethrower",
            ok ? "ran" : "faulted");
}

}  // namespace

bool Install() {
    if (!engine::Caps().stage308Setup) {
        RM_LOGW("stage fix: s308's setup signatures missing; s308 is left as it is");
        return false;
    }
    auto* target = reinterpret_cast<void*>(re5::addr::kFnS308Setup);
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&S308SetupDetour), reinterpret_cast<void**>(&g_origS308Setup));
    if (status == MH_OK) status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        RM_LOGE("stage fix: hook s308 setup at 0x%08X failed: %s", static_cast<unsigned>(re5::addr::kFnS308Setup),
                MH_StatusToString(status));
        return false;
    }
    RM_LOGI("stage fix: s308 setup hook queued");
    return true;
}

void SetInRun(bool inRun) { g_inRun = inRun; }

}  // namespace rmre5::stagefix
