// Campaign stage setup fixes. See stage_fixes.h.

#include "stage_fixes.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
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

// Stage 100's own checks (stage_fixes.h): the bytes the game has, and the ones a run's match gets.
struct CodePatch {
    std::uintptr_t address;
    const char* original;
    const char* patched;
    const char* what;
};
constexpr CodePatch kStage100[] = {
    {0x008661BE, "75 09", "EB 09", "the walk without a weapon"},
    {0x00B70141, "74 25", "90 90", "the weapon model"},
    {0x00B65F82, "75 38", "EB 38", "the holster"},
    {0x00B668FB, "75 0A", "EB 0A", "the weapon's mode"},
    {0x00B8B480, "74 2B", "90 90", "the weapon's buttons"},
    {0x009F11B9, "74 5A", "90 90", "the cockpit's grid state"},
    {0x00A70C69, "74 91", "90 90", "the inventory grid on screen"},
    {0x00C6C440, "0F 84 18 0C 00 00", "66 0F 1F 44 00 00", "the inventory's opening"},
};
bool g_stage100Off = false;  // the patched bytes are in

std::size_t HexBytes(const char* hex, unsigned char* out, std::size_t room) {
    std::size_t n = 0;
    for (const char* p = hex; *p && n < room;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        out[n++] = static_cast<unsigned char>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16));
        p += 2;
    }
    return n;
}

// Every site to `off` (the run's bytes) or back; true when each one is as asked.
bool SwitchStage100(bool off) {
    int done = 0;
    for (const auto& patch : kStage100) {
        unsigned char bytes[8];
        const std::size_t size = HexBytes(off ? patch.patched : patch.original, bytes, sizeof(bytes));
        if (BytesMatch(patch.address, off ? patch.patched : patch.original) ||
            engine::WriteCode(patch.address, off ? patch.original : patch.patched, bytes, size)) {
            ++done;
        } else {
            RM_LOGW("stage fix: s100's check of %s at 0x%08X is not as expected; left as it is", patch.what,
                    static_cast<unsigned>(patch.address));
        }
    }
    const int total = static_cast<int>(sizeof(kStage100) / sizeof(kStage100[0]));
    RM_LOGI("stage fix: s100's story checks %s (%d of %d)", off ? "off for the run (weapon and inventory as on every stage)" : "back",
            done, total);
    return done == total;
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

void SetInRun(bool inRun, bool runWanted, bool inMatch) {
    g_inRun = inRun;
    if (inMatch || runWanted == g_stage100Off) return;
    SwitchStage100(runWanted);
    g_stage100Off = runWanted;  // once per change, even when a site was not as expected
}

}  // namespace rmre5::stagefix
