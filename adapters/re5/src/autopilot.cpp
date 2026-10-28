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
using FadeFn = void(__thiscall*)(std::uintptr_t self, float alpha, int frames);
using FlowStepFn = int(__thiscall*)(std::uintptr_t ctx, int kind);
using FlowStep20Fn = int(__thiscall*)(std::uintptr_t ctx);

constexpr std::uint8_t kStateInput = 2;
constexpr std::uint8_t kStateModeInput = 10;  // uIdStartUp20's Solo/Duo page
constexpr std::uint8_t kStateLoading = 80;    // uIdStartUp22 after the character decide
constexpr int kItemStart = 0;                 // "Iniciar"
constexpr int kModeSolo = 0;
constexpr DWORD kTimeoutMs = 45000;  // a stage change passes the time-up and the results screen
// A decide the screen has not taken after this long means the row is locked
// in the game's own save (stages and costumes unlock with ranks).
constexpr DWORD kDenyMs = 1500;

MenuInputFn g_origMenuInput = nullptr;
FlowStepFn g_origFlowStep = nullptr;
FlowStep20Fn g_origFlowStep20 = nullptr;
std::atomic<bool> g_flowHooks{false};
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
std::atomic<bool> g_faded{false};
std::atomic<DWORD> g_injectSince{0};  // first decide injected on the current screen
std::atomic<unsigned> g_lockedStages{0};  // bit per stage the game refused this session
// Set around one screen's move(): its own input query then reports decide.
thread_local std::uint32_t t_injectBits = 0;  // menu input bits added for this frame
std::atomic<bool> g_resultsHook{false};

std::uint32_t DecideMask() {
    const auto pad = ReadOr<std::uintptr_t>(re5::addr::kPadInstance, 0);
    return pad ? ReadOr<std::uint32_t>(pad + re5::off::kPadDecideMask, 0) : 0;
}

// The game's own fade to black (what the character screen does after its
// decide), so the driven screens pass behind black.
void FadeToBlack(int frames) {
    if (!engine::Caps().fader) return;
    const auto fader = ReadOr<std::uintptr_t>(re5::addr::kFadeInstance, 0);
    const auto vtable = fader ? ReadOr<std::uintptr_t>(fader, 0) : 0;
    const auto fn = vtable ? ReadOr<std::uintptr_t>(vtable + re5::off::kFadeVtableOut, 0) : 0;
    if (!fn || fn < re5::kImageBase) return;
    reinterpret_cast<FadeFn>(fn)(fader, 1.0f, frames);
    RM_LOGI("autopilot: fade to black requested (%d frames)", frames);
}

std::uintptr_t GameWork() {
    return ReadOr<std::uintptr_t>(ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0) + re5::off::kGameWork, 0);
}

// Title-flow script: the Iniciar/Solo screen. Answer "Iniciar, Solo" at once.
int __fastcall FlowStep20Detour(std::uintptr_t ctx, void*) {
    if (g_armed.load() && ReadOr<std::uint8_t>(ctx + re5::off::kFlowStepState, 1) == 0) {
        if (const auto gw = GameWork()) {
            SafeStore<std::int32_t>(gw + re5::off::kGameWorkMercItem, 0);
            SafeStore<std::int32_t>(gw + re5::off::kGameWorkMercMode, 0);
            SafeStore<std::uint8_t>(gw + re5::off::kGameWorkMercFlag, 0);
            RM_LOGI("autopilot: flow Iniciar/Solo answered, screen skipped");
            return 1;
        }
    }
    return g_origFlowStep20(ctx);
}

// Title-flow script: stage (kind 0) and character (kind 1) screens. Write the
// choice and report "accepted" without creating the screen; the script then
// starts the match itself (its state 0x14).
int __fastcall FlowStepDetour(std::uintptr_t ctx, void*, int kind) {
    if (g_armed.load() && (kind == 0 || kind == 1) && ReadOr<std::uint8_t>(ctx + re5::off::kFlowStepState, 1) == 0) {
        if (const auto gw = GameWork()) {
            if (kind == 0) {
                SafeStore<std::int32_t>(gw + re5::off::kGameWorkStage, g_stage.load());
                RM_LOGI("autopilot: flow stage %d written, stage screen skipped", g_stage.load());
            } else {
                SafeStore<std::int32_t>(gw + re5::off::kGameWorkCharacter, g_costume.load());
                RM_LOGI("autopilot: flow costume slot %d written, character screen skipped", g_costume.load());
                Disarm("match requested by the flow script");
            }
            return 1;
        }
    }
    return g_origFlowStep(ctx, kind);
}

std::uint32_t __stdcall MenuInputDetour(int pad, int mode) {
    const std::uint32_t bits = g_origMenuInput(pad, mode);
    return bits | t_injectBits;
}

void Drive(std::uintptr_t self, MoveFn orig, int screen) {
    bool inject = false;
    if (g_enabled.load() && g_armed.load()) {
        const auto state = ReadOr<std::uint8_t>(self + re5::off::kMenuState, 0);
        switch (screen) {
            case 20:
                if (state == kStateInput) {
                    if (!g_faded.exchange(true)) FadeToBlack(4);
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
                        // Locked stage: remember it and try the next one; stage 0 is always available.
                        const int count = std::max(1, g_stageCount.load());
                        g_lockedStages |= 1u << (g_stage.load() & 31);
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
    t_injectBits = inject ? DecideMask() : 0;
    orig(self);
    t_injectBits = 0;
}

// Mercenaries results screen (uIdMerceEd00) between the stages of one run: while the
// autopilot is armed the screen is accepted at once (a decide press is fed to its input
// query every frame) and kept behind the fader, so the game returns to its Mercenaries
// menu scene, where the title-flow script restarts the match on the armed stage.
MoveFn g_origResultsMove = nullptr;
std::atomic<int> g_resultsState{-1};

void __fastcall ResultsMoveDetour(std::uintptr_t self, void*) {
    std::uint32_t inject = 0;
    if (g_enabled.load() && g_armed.load()) {
        const auto state = ReadOr<std::uint8_t>(self + re5::off::kMenuState, 0);
        const bool optionsPhase = state == 10;
        const int last = g_resultsState.exchange(state);
        if (last != state) {
            if (last < 0) FadeToBlack(1);
            RM_LOGI("autopilot: results screen state %d -> %u%s", last, state, optionsPhase ? " (options)" : "");
        }
        // "Aceptar" reads the decide bits straight from the menu input query; the option
        // list that follows checks cancel before its fade gate, so cancel closes it even
        // behind the fader.
        if (state == 0x1E) inject = DecideMask();
        if (optionsPhase) inject = DecideMask() | re5::off::kMenuInputCancelBit;
    } else {
        g_resultsState = -1;
    }
    t_injectBits = inject;
    g_origResultsMove(self);
    t_injectBits = 0;
    if (g_enabled.load() && g_armed.load()) {
        // Once initialised (state 0 done), jump to the screen's own exit states (0x5A..0x5C:
        // flow request, wait, close) so the stats page and the record save never run.
        const auto after = ReadOr<std::uint8_t>(self + re5::off::kMenuState, 0);
        if (after >= 1 && after <= 0x1F) {
            SafeStore<std::uint8_t>(self + re5::off::kMenuState, re5::off::kResultsExitState);
            RM_LOGI("autopilot: results screen skipped (state %u -> 0x5A)", after);
        }
    }
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
    if (engine::Caps().flowScript &&
        QueueHook(re5::addr::kFnFlowStep, reinterpret_cast<void*>(&FlowStepDetour),
                  reinterpret_cast<void**>(&g_origFlowStep)) &&
        QueueHook(re5::addr::kFnFlowStep20, reinterpret_cast<void*>(&FlowStep20Detour),
                  reinterpret_cast<void**>(&g_origFlowStep20))) {
        g_flowHooks = true;
        RM_LOGI("autopilot: title-flow script hooks queued (screens are skipped)");
    }
    if (g_flowHooks.load() && engine::Caps().resultsScreen &&
        QueueHook(re5::addr::kFnMerceResultsMove, reinterpret_cast<void*>(&ResultsMoveDetour),
                  reinterpret_cast<void**>(&g_origResultsMove))) {
        g_resultsHook = true;
        RM_LOGI("autopilot: results screen hook queued (accepted while armed)");
    }
    return true;
}

bool CanChangeStage() { return g_enabled.load() && g_flowHooks.load() && g_resultsHook.load(); }

bool Available() { return g_enabled.load(); }

void Arm(const Plan& plan) {
    // Skip stages the game refused earlier this session.
    int stage = plan.stage;
    const int count = std::max(1, plan.stage_count);
    for (int i = 0; i < count && (g_lockedStages.load() & (1u << (stage & 31))); ++i) stage = (stage + 1) % count;
    g_stage = stage;
    g_costume = plan.costume;
    g_stageCount = plan.stage_count;
    g_stageTries = 0;
    g_costumeFallback = false;
    g_faded = false;
    g_screen = 0;
    g_armedAt = GetTickCount();
    g_armed = true;
    RM_LOGI("autopilot: armed (stage %d, costume slot %d)", plan.stage, plan.costume);
    // With the flow hooks no screen ever takes input; black out from here.
    if (g_flowHooks.load() && !g_faded.exchange(true)) FadeToBlack(4);
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
