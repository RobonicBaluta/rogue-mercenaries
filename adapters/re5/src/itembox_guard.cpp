#include "itembox_guard.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>

#include "engine.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5::itemboxguard {
namespace {

// Panel modes (FUN_00A0EF00's switch): 0 the top menu (Organize, Buy, Exit), 1 the organize
// grid, 7 the Buy list, 10 Upgrade (entered from the grid for an upgradable item).
constexpr std::uint8_t kModeTopMenu = 0;
constexpr std::uint8_t kModeBuy = 7;
constexpr std::uint8_t kModeUpgrade = 10;
constexpr std::uint8_t kTopMenuBuy = 1;   // top-menu cursor entries: 0 Organize, 1 Buy, 2 Exit
constexpr std::size_t kPanelMode = 0x00;
constexpr std::size_t kPanelStep = 0x01;
constexpr std::size_t kPanelPlayer = 0x02;
constexpr std::size_t kPanelCursor = 0x19;

using PanelUpdateFn = void(__thiscall*)(std::uintptr_t box, std::uintptr_t panel);
using PanelTabFn = void(__thiscall*)(std::uintptr_t box, std::uintptr_t panel, int tab);
PanelUpdateFn g_origUpdate = nullptr;
std::atomic<bool> g_guard{false};
std::uint8_t g_lastCursor[2] = {0, 0};

void __fastcall PanelUpdateDetour(std::uintptr_t box, void*, std::uintptr_t panel) {
    if (!g_guard.load() || !panel) {
        g_origUpdate(box, panel);
        return;
    }
    const auto mode = ReadOr<std::uint8_t>(panel + kPanelMode, 0xFF);
    const auto step = ReadOr<std::uint8_t>(panel + kPanelStep, 0);
    const std::size_t player = ReadOr<std::uint8_t>(panel + kPanelPlayer, 0) & 1;
    // The cursor skips "Buy" the way it moved: down from Organize to Exit, up from Exit to Organize.
    if (mode == kModeTopMenu) {
        const auto cursor = ReadOr<std::uint8_t>(panel + kPanelCursor, 0);
        if (cursor == kTopMenuBuy) {
            SafeStore<std::uint8_t>(panel + kPanelCursor, g_lastCursor[player] == 2 ? 0 : 2);
        }
    }
    g_origUpdate(box, panel);
    const auto after = ReadOr<std::uint8_t>(panel + kPanelMode, mode);
    if (after == kModeBuy && mode != kModeBuy) {
        // Reached anyway: the top menu again, from its start.
        SafeStore<std::uint8_t>(panel + kPanelMode, kModeTopMenu);
        SafeStore<std::uint8_t>(panel + kPanelStep, 0);
        RM_LOGI("itembox guard: Buy refused (the run buys in the SHOP)");
    } else if (after == kModeUpgrade && mode != kModeUpgrade) {
        // Back to the grid where the choice was made, its layout as the grid shows it.
        SafeStore<std::uint8_t>(panel + kPanelMode, mode);
        SafeStore<std::uint8_t>(panel + kPanelStep, step);
        __try {
            reinterpret_cast<PanelTabFn>(re5::addr::kFnItemBoxPanelTab)(box, panel, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        RM_LOGI("itembox guard: Upgrade refused (the run upgrades in the SHOP)");
    }
    if (ReadOr<std::uint8_t>(panel + kPanelMode, 0xFF) == kModeTopMenu) {
        const auto cursor = ReadOr<std::uint8_t>(panel + kPanelCursor, 0);
        if (cursor != kTopMenuBuy) g_lastCursor[player] = cursor;
    }
}

}  // namespace

bool Install() {
    if (!engine::Caps().itemBoxGuard) {
        RM_LOGW("itembox guard: signatures missing; the game's Organize screen keeps Buy and Upgrade");
        return false;
    }
    auto* target = reinterpret_cast<void*>(re5::addr::kFnItemBoxPanelUpdate);
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&PanelUpdateDetour), reinterpret_cast<void**>(&g_origUpdate));
    if (status == MH_OK) status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        RM_LOGE("itembox guard: hook failed: %s", MH_StatusToString(status));
        return false;
    }
    RM_LOGI("itembox guard: hook on the Organize screen's panels queued");
    return true;
}

void SetGuard(bool on) {
    if (g_guard.exchange(on) != on) {
        g_lastCursor[0] = g_lastCursor[1] = 0;
        RM_LOGI("itembox guard: %s", on ? "on (no Buy, no Upgrade)" : "off");
    }
}

}  // namespace rmre5::itemboxguard
