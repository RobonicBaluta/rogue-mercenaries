// Arena host. See host.h.

#include "host.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>

#include "engine.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5::host {
namespace {

using SetupFn = void(__cdecl*)();

SetupFn g_origSetup = nullptr;
std::atomic<bool> g_available{false};
std::atomic<int> g_override{-1};

std::uintptr_t SavedStageField() {
    const auto save = ReadOr<std::uintptr_t>(re5::addr::kSaveData, 0);
    if (!save) return 0;
    const auto slot = ReadOr<std::int32_t>(save + re5::off::kSaveSlot, -1);
    if (slot < 0 || slot > 8) return 0;
    return save + static_cast<std::size_t>(slot) * re5::off::kSaveSlotStride + re5::off::kSaveStage;
}

void __cdecl SetupDetour() {
    g_origSetup();
    const int stage = g_override.load();
    const std::uintptr_t field = SavedStageField();
    const int chosen = field ? ReadOr<std::int32_t>(field, -1) : -1;
    if (stage >= 0 && field) {
        SafeStore<std::int32_t>(field, stage);
        RM_LOGI("host: match setup chose stage %d; loading stage %d instead", chosen, stage);
    } else {
        RM_LOGI("host: match setup chose stage %d", chosen);
    }
}

}  // namespace

bool Install() {
    if (!engine::Caps().matchSetup) {
        RM_LOGW("host: match setup signature missing; no stage override");
        return false;
    }
    auto* target = reinterpret_cast<void*>(re5::addr::kFnMercMatchSetup);
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&SetupDetour), reinterpret_cast<void**>(&g_origSetup));
    if (status == MH_OK) status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        RM_LOGE("host: hook on the match setup failed: %s", MH_StatusToString(status));
        return false;
    }
    g_available = true;
    RM_LOGI("host: hook on the Mercenaries match setup queued");
    return true;
}

bool Available() { return g_available.load(); }

void SetStageOverride(int stage) { g_override = stage; }
int StageOverride() { return g_override.load(); }

int SavedStage() {
    const std::uintptr_t field = SavedStageField();
    return field ? ReadOr<std::int32_t>(field, -1) : -1;
}

}  // namespace rmre5::host
