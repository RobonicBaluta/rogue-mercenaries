// Mercenaries countdown control. The authoritative countdown is a field of
// the match object; game work +0x680 is only a per-frame mirror, so writes
// there are lost. The detour applies the mod's floor / end request right
// before the game's own update, on the game's thread.

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>

#include "engine.h"
#include "hooks.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5 {
namespace {

using UpdateCountdownFn = void(__thiscall*)(std::uintptr_t self, float dt);
UpdateCountdownFn g_origUpdate = nullptr;

std::atomic<std::uintptr_t> g_object{0};
std::atomic<DWORD> g_lastSeen{0};
std::atomic<float> g_floor{-1.0f};
std::atomic<bool> g_endRequested{false};

void __fastcall UpdateCountdownDetour(std::uintptr_t self, void*, float dt) {
    if (g_object.exchange(self) != self) {
        RM_LOGI("mercenaries match object %08X", static_cast<unsigned>(self));
    }
    g_lastSeen = GetTickCount();
    float countdown = 0.0f;
    std::int32_t state = -1;
    if (SafeRead(self + re5::off::kMercCountdown, countdown) && SafeRead(self + re5::off::kMercState, state) &&
        state == 0) {
        if (g_endRequested.exchange(false)) {
            // Zero lets the game's own time-up path end the match next update.
            SafeStore(self + re5::off::kMercCountdown, 0.0f);
            RM_LOGI("mercenaries countdown forced to 0 (was %.2f)", countdown);
        } else {
            const float floor = g_floor.load();
            if (floor > 0.0f && countdown < floor) {
                SafeStore(self + re5::off::kMercCountdown, floor);
            }
        }
    }
    g_origUpdate(self, dt);
}

}  // namespace

void SetMercTimerFloor(float seconds) { g_floor = seconds; }

void RequestMercMatchEnd() { g_endRequested = true; }
void ClearMercMatchEnd() { g_endRequested = false; }

MercTimerView ReadMercTimer() {
    MercTimerView view;
    view.object = g_object.load();
    view.recent = view.object != 0 && GetTickCount() - g_lastSeen.load() < 500;
    if (view.recent) {
        view.countdown = ReadOr<float>(view.object + re5::off::kMercCountdown, 0.0f);
        view.state = ReadOr<std::int32_t>(view.object + re5::off::kMercState, -1);
    }
    return view;
}

bool InstallMercHooks() {
    if (!engine::Caps().mercCountdown) {
        RM_LOGE("mercenaries countdown hook skipped: signature missing");
        return false;
    }
    auto* target = reinterpret_cast<void*>(re5::addr::kFnMercUpdateCountdown);
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&UpdateCountdownDetour),
                                     reinterpret_cast<void**>(&g_origUpdate));
    if (status == MH_OK) {
        status = MH_QueueEnableHook(target);
    }
    if (status != MH_OK) {
        RM_LOGE("hook cMercs::UpdateCountdown failed: %s", MH_StatusToString(status));
        return false;
    }
    RM_LOGI("hook cMercs::UpdateCountdown at 0x%08X queued", static_cast<unsigned>(re5::addr::kFnMercUpdateCountdown));
    return true;
}

}  // namespace rmre5
