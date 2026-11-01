// Mercenaries countdown control. The authoritative countdown is a field of
// the match object; game work +0x680 is only a per-frame mirror, so writes
// there are lost. The detour applies the mod's floor / end request right
// before the game's own update, on the game's thread.

#include <windows.h>
#include <intrin.h>

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

using AddTimeFn = void(__thiscall*)(std::uintptr_t self, int seconds);
using ActionFn = void(__thiscall*)(std::uintptr_t self);
AddTimeFn g_origAddTime = nullptr;
ActionFn g_origTimeBonus = nullptr;
std::atomic<bool> g_suppressAddTime{false};
std::atomic<int> g_pickups{0};

void __fastcall AddTimeDetour(std::uintptr_t self, void*, int seconds) {
    if (g_suppressAddTime.load()) return;  // a run holds the countdown; bonuses are chests instead
    g_origAddTime(self, seconds);
}

void __fastcall TimeBonusActionDetour(std::uintptr_t self, void*) {
    const auto before = ReadOr<std::uint8_t>(self + 0x14, 0xFF);
    const auto object = ReadOr<std::uintptr_t>(self + 0x18, 0);
    g_origTimeBonus(self);
    if (before == 1 && object && ReadOr<std::uint8_t>(self + 0x14, 0) == 2) {
        g_pickups.fetch_add(1);
        RM_LOGI("time bonus picked up: object %08X (%s, %d s)", static_cast<unsigned>(object),
                engine::ClassName(object).c_str(), ReadOr<std::int16_t>(object + 0x1364, 0));
    }
}

}  // namespace

void SetMercTimerFloor(float seconds) { g_floor = seconds; }
void SuppressMercAddTime(bool suppress) { g_suppressAddTime = suppress; }
int TakeTimeBonusPickups() { return g_pickups.exchange(0); }
void SimulateTimeBonusPickup() { g_pickups.fetch_add(1); }

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
    if (engine::Caps().timeBonus) {
        auto* addTime = reinterpret_cast<void*>(re5::addr::kFnMercAddCountdown);
        auto* action = reinterpret_cast<void*>(re5::addr::kFnTimeBonusAction);
        MH_STATUS a = MH_CreateHook(addTime, reinterpret_cast<void*>(&AddTimeDetour), reinterpret_cast<void**>(&g_origAddTime));
        if (a == MH_OK) a = MH_QueueEnableHook(addTime);
        MH_STATUS b = MH_CreateHook(action, reinterpret_cast<void*>(&TimeBonusActionDetour), reinterpret_cast<void**>(&g_origTimeBonus));
        if (b == MH_OK) b = MH_QueueEnableHook(action);
        if (a == MH_OK && b == MH_OK) {
            RM_LOGI("hooks cMercs::AddTime and the time-bonus pickup queued (chests)");
        } else {
            RM_LOGE("time-bonus hooks failed: %s / %s", MH_StatusToString(a), MH_StatusToString(b));
        }
    }
    return true;
}

}  // namespace rmre5
