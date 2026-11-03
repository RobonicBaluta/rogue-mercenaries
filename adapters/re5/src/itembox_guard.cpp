#include "itembox_guard.h"

#include <windows.h>

#include <MinHook.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "engine.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"
#include "resources.h"

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

// The screen's item records (re5_layout.h, kItemBoxTable).
#pragma pack(push, 1)
struct BoxItem {
    std::uint16_t item;
    std::uint16_t picture;  // "item_NNN"
    std::uint16_t most;     // per storage record
    std::uint8_t flags[2];
    const char* model;      // the preview's
};
#pragma pack(pop)
static_assert(sizeof(BoxItem) == 12, "the screen's 12-byte item record (32-bit build)");
using BoxTable = std::array<BoxItem, re5::addr::kItemBoxTableRecords>;

// Guns a run hands out that the campaign never stocks have no record: no picture in their cell
// (or the cell's last one) and the preview keeps the last item's model (user playtest
// 2026-11-03, the Px4). The Px4 has its own picture in the layout, unused (item_13e, the gap
// between the M93R's and the Dragunov's); the Samurai Edge has none and takes the M92F's, its
// base gun. They go after the last handgun (the M93R), so the storage sorts them with the
// handguns; the last records, treasures, make the room (a run's box holds no treasures).
struct ExtraGun {
    std::uint16_t item;
    std::uint16_t picture;
    const char* model;
};
constexpr ExtraGun kExtraGuns[] = {
    {0x011F, 0x13E, "pawn\\wp\\wp1f\\model\\wp1f00"},  // Px4
    {0x0129, 0x124, "pawn\\wp\\wp29\\model\\wp2900"},  // Samurai Edge (S.T.A.R.S. Wesker's)
    {0x0112, 0x124, "pawn\\wp\\wp12\\model\\wp1200"},  // Samurai Edge (the other record)
};
constexpr std::uint16_t kInsertAfter = 0x011E;  // M93R
constexpr std::uint16_t kFirstTreasure = 0x0400;

BoxTable g_tableOriginal{};
bool g_tableWanted = false;
bool g_tablePatched = false;

bool WriteTable(const BoxTable& table) {
    const auto address = re5::addr::kItemBoxTable;
    const std::size_t size = sizeof(BoxTable);
    if (SafeWrite(address, table.data(), size)) return true;
    DWORD old = 0;  // the table may sit in read-only data
    if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_READWRITE, &old)) return false;
    const bool ok = SafeWrite(address, table.data(), size);
    VirtualProtect(reinterpret_cast<void*>(address), size, old, &old);
    return ok;
}

// The records with the run's guns in them; false (nothing to write) when the table is not the one
// expected or every gun has a record already.
bool BuildTable(const BoxTable& original, BoxTable& out) {
    bool anchor = false;
    std::vector<ExtraGun> missing;
    for (const ExtraGun& gun : kExtraGuns) {
        bool known = false;
        for (const BoxItem& record : original) known = known || record.item == gun.item;
        if (!known) missing.push_back(gun);
    }
    for (const BoxItem& record : original) anchor = anchor || record.item == kInsertAfter;
    if (!anchor || missing.empty()) return false;
    // The room: as many records from the end, all of them treasures.
    const std::size_t keep = original.size() - missing.size();
    for (std::size_t i = keep; i < original.size(); ++i) {
        if (original[i].item < kFirstTreasure) return false;
    }
    std::size_t at = 0;
    for (std::size_t i = 0; i < keep; ++i) {
        out[at++] = original[i];
        if (original[i].item != kInsertAfter) continue;
        for (const ExtraGun& gun : missing) {
            // The preview loads the model at once: only from an archive this stage has loaded.
            const bool loaded = resources::WeaponLoaded(gun.item);
            out[at++] = BoxItem{gun.item, gun.picture, 1, {0, 0}, loaded ? gun.model : nullptr};
            RM_LOGI("itembox pictures: %04X gets item_%03x%s", gun.item, gun.picture, loaded ? " and its model" : " (its model is not loaded here)");
        }
    }
    return at == out.size();
}

void PatchTable(bool on) {
    if (on == g_tableWanted) return;
    g_tableWanted = on;
    if (!engine::Caps().itemBoxPictures) return;
    if (!on) {
        if (!g_tablePatched) return;
        g_tablePatched = false;
        RM_LOGI("itembox pictures: the screen's item table restored: %s", WriteTable(g_tableOriginal) ? "ok" : "failed");
        return;
    }
    if (!SafeCopy(g_tableOriginal.data(), re5::addr::kItemBoxTable, sizeof(BoxTable))) {
        RM_LOGE("itembox pictures: the screen's item table could not be read");
        return;
    }
    BoxTable patched{};
    if (!BuildTable(g_tableOriginal, patched)) {
        RM_LOGW("itembox pictures: the screen's item table is not as expected (or complete); left as it is");
        return;
    }
    g_tablePatched = WriteTable(patched);
    RM_LOGI("itembox pictures: the run's guns in the screen's item table: %s", g_tablePatched ? "ok" : "failed");
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
    PatchTable(on);
}

}  // namespace rmre5::itemboxguard
