// Drives the game's own Mercenaries setup screens after START RUN.
//
// The native menu's START RUN is the game's own "Los Mercenarios" decide, so
// the game opens uIdStartUp20 (Iniciar, then Solo/Duo), uIdStartUp21 (stage)
// and uIdStartUp22 (character/costume) as usual. While armed, this module
// writes each screen's cursor field before its move() and feeds the game's
// menu input query a "decide" press for that call only, so every transition
// (sounds, fades, game work writes, the loading screen) is the game's own.

#include "autopilot.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

#include "engine.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5::autopilot {
namespace {

using MoveFn = void(__thiscall*)(std::uintptr_t self);
using MenuInputFn = std::uint32_t(__stdcall*)(int pad, int mode);

constexpr std::uint8_t kStateInput = 2;
constexpr std::uint8_t kStateModeInput = 10;  // uIdStartUp20's Solo/Duo page
constexpr std::uint8_t kStateLoading = 80;    // uIdStartUp22 after the character decide
constexpr int kItemStart = 0;                 // "Iniciar"
constexpr int kModeSolo = 0;
constexpr DWORD kTimeoutMs = 20000;
// A decide the screen has not taken after this long means the row is locked
// in the game's own save (stages and costumes unlock with ranks).
constexpr DWORD kDenyMs = 1500;

MenuInputFn g_origMenuInput = nullptr;
MoveFn g_origMove20 = nullptr;
MoveFn g_origMove21 = nullptr;
MoveFn g_origMove22 = nullptr;
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_armed{false};
std::atomic<int> g_stage{0};
std::atomic<int> g_costume{0};
std::atomic<int> g_screen{0};  // screen being driven, for the log
std::atomic<DWORD> g_armedAt{0};
std::atomic<int> g_stageCount{8};
std::atomic<int> g_stageTries{0};
std::atomic<bool> g_costumeFallback{false};
std::atomic<DWORD> g_injectSince{0};  // first decide injected on the current screen
// Set around one screen's move(): its own input query then reports decide.
thread_local bool t_inject = false;

std::uint32_t DecideMask() {
    const auto pad = ReadOr<std::uintptr_t>(re5::addr::kPadInstance, 0);
    return pad ? ReadOr<std::uint32_t>(pad + re5::off::kPadDecideMask, 0) : 0;
}

std::uint32_t __stdcall MenuInputDetour(int pad, int mode) {
    const std::uint32_t bits = g_origMenuInput(pad, mode);
    return t_inject ? (bits | DecideMask()) : bits;
}

void Drive(std::uintptr_t self, MoveFn orig, int screen) {
    bool inject = false;
    if (g_enabled.load() && g_armed.load()) {
        const auto state = ReadOr<std::uint8_t>(self + re5::off::kMenuState, 0);
        switch (screen) {
            case 20:
                if (state == kStateInput) {
                    SafeStore<std::int32_t>(self + re5::off::kMenuCursor, kItemStart);
                    inject = true;
                } else if (state == kStateModeInput) {
                    SafeStore<std::int32_t>(self + re5::off::kMenuModeCursor, kModeSolo);
                    inject = true;
                }
                break;
            case 21:
                if (state == kStateInput) {
                    const DWORD now = GetTickCount();
                    if (g_screen.load() == 21 && now - g_injectSince.load() > kDenyMs) {
                        // Locked stage: try the next one; stage 0 is always available.
                        const int count = std::max(1, g_stageCount.load());
                        const int next = g_stageTries.fetch_add(1) + 1 >= count ? 0 : (g_stage.load() + 1) % count;
                        RM_LOGI("autopilot: stage %d is locked in the game, trying %d", g_stage.load(), next);
                        g_stage = next;
                        g_injectSince = now;
                    }
                    SafeStore<std::int32_t>(self + re5::off::kMenuCursor, g_stage.load());
                    inject = true;
                }
                break;
            case 22:
                if (state == kStateInput) {
                    const DWORD now = GetTickCount();
                    if (g_screen.load() == 22 && !g_costumeFallback.load() && now - g_injectSince.load() > kDenyMs) {
                        RM_LOGW("autopilot: costume slot %d is locked in the game, using slot 0", g_costume.load());
                        g_costumeFallback = true;
                        g_costume = 0;
                        g_injectSince = now;
                    }
                    SafeStore<std::int32_t>(self + re5::off::kMenuCharacterIndex, g_costume.load());
                    inject = true;
                } else if (state >= kStateLoading) {
                    Disarm("character chosen");
                }
                break;
            default: break;
        }
        if (inject && g_screen.exchange(screen) != screen) {
            g_injectSince = GetTickCount();
            RM_LOGI("autopilot: driving uIdStartUp%d (state %u)", screen, state);
        }
    }
    t_inject = inject;
    orig(self);
    t_inject = false;
}

void __fastcall Move20Detour(std::uintptr_t self, void*) { Drive(self, g_origMove20, 20); }
void __fastcall Move21Detour(std::uintptr_t self, void*) { Drive(self, g_origMove21, 21); }
void __fastcall Move22Detour(std::uintptr_t self, void*) { Drive(self, g_origMove22, 22); }

bool QueueHook(std::uintptr_t address, void* detour, void** original) {
    auto* target = reinterpret_cast<void*>(address);
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_OK) status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        RM_LOGE("autopilot: hook at 0x%08X failed: %s", static_cast<unsigned>(address), MH_StatusToString(status));
        return false;
    }
    return true;
}

}  // namespace

bool Install() {
    if (!engine::Caps().setupScreens) {
        RM_LOGW("autopilot: setup screen signatures missing; START RUN keeps the game's screens");
        return false;
    }
    if (!QueueHook(re5::addr::kFnMenuInput, reinterpret_cast<void*>(&MenuInputDetour),
                   reinterpret_cast<void**>(&g_origMenuInput)) ||
        !QueueHook(re5::addr::kFnStartUp20Move, reinterpret_cast<void*>(&Move20Detour),
                   reinterpret_cast<void**>(&g_origMove20)) ||
        !QueueHook(re5::addr::kFnStartUp21Move, reinterpret_cast<void*>(&Move21Detour),
                   reinterpret_cast<void**>(&g_origMove21)) ||
        !QueueHook(re5::addr::kFnStartUp22Move, reinterpret_cast<void*>(&Move22Detour),
                   reinterpret_cast<void**>(&g_origMove22))) {
        return false;
    }
    g_enabled = true;
    RM_LOGI("autopilot: hooks on the Mercenaries setup screens queued");
    return true;
}

bool Available() { return g_enabled.load(); }

void Arm(const Plan& plan) {
    g_stage = plan.stage;
    g_costume = plan.costume;
    g_stageCount = plan.stage_count;
    g_stageTries = 0;
    g_costumeFallback = false;
    g_screen = 0;
    g_armedAt = GetTickCount();
    g_armed = true;
    RM_LOGI("autopilot: armed (stage %d, costume slot %d)", plan.stage, plan.costume);
}

void Disarm(const char* reason) {
    if (!g_armed.exchange(false)) return;
    const auto gameWork = ReadOr<std::uintptr_t>(ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0) + re5::off::kGameWork, 0);
    RM_LOGI("autopilot: %s (game work stage %d, costume slot %d)", reason,
            gameWork ? ReadOr<std::int32_t>(gameWork + re5::off::kGameWorkStage, -1) : -1,
            gameWork ? ReadOr<std::int32_t>(gameWork + re5::off::kGameWorkCharacter, -1) : -1);
}

bool Armed() { return g_armed.load(); }

void OnGameTick() {
    if (g_armed.load() && GetTickCount() - g_armedAt.load() > kTimeoutMs) Disarm("timed out");
}

}  // namespace rmre5::autopilot
