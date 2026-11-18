#include <windows.h>

#include <MinHook.h>

#include <string>

#include "crash.h"
#include "dev_audio.h"
#include "dev_capture.h"
#include "engine.h"
#include "file_redirect.h"
#include "area_cameras.h"
#include "stage_fixes.h"
#include "hooks.h"
#include "log.h"
#include "menu_probe.h"
#include "autopilot.h"
#include "native_menu.h"
#include "match_ui.h"
#include "itembox_guard.h"
#include "director.h"
#include "resources.h"
#include "world.h"
#include "host.h"
#include "mod.h"
#include "overlay.h"
#include "probe.h"
#include "proxy.h"
#include "settings.h"
#include "version.h"
#include "rm/common.h"

namespace {

HMODULE g_module = nullptr;

rmre5::LogLevel ParseLevel(const std::string& name) {
    if (name == "error") return rmre5::LogLevel::Error;
    if (name == "warn") return rmre5::LogLevel::Warn;
    if (name == "debug") return rmre5::LogLevel::Debug;
    return rmre5::LogLevel::Info;
}

DWORD StartMod() {
    using namespace rmre5;
    const Paths paths = ResolvePaths(g_module);
    std::string settingsError;
    const Settings settings = LoadSettings(paths, &settingsError);
    LogOpen(paths.logDir / L"adapter.log", ParseLevel(settings.logLevel));
    RM_LOGI("Rogue Mercenaries %s (adapter re5 %s) starting", kModVersion, kAdapterVersion);
    {
        // How it was loaded: as the game's dinput8.dll, or as another loader's plugin (its
        // plugins folder, an .asi), which keeps that loader's dinput8.dll and its fixes.
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(g_module, self, MAX_PATH);
        const std::filesystem::path me(self);
        const bool proxy = me.parent_path() == paths.gameDir && _wcsicmp(me.filename().c_str(), L"dinput8.dll") == 0;
        RM_LOGI("loaded from %s (%s)", rm::PathText(me).c_str(), proxy ? "the game's dinput8.dll" : "a plugin of another loader");
    }
    RM_LOGI("game dir: %s", rm::PathText(paths.gameDir).c_str());
    RM_LOGI("data dir: %s", rm::PathText(paths.dataDir).c_str());
    if (!settingsError.empty()) {
        RM_LOGW("%s", settingsError.c_str());
    }

    InstallCrashDiagnostics();
    if (settings.developerMode) StartHangWatchdog();
    if (settings.muteAudio || std::filesystem::exists(paths.dataDir / L"dev_muted.flag")) {
        // Own thread so the mute lands before the intro starts playing.
        static Paths audioPaths = paths;
        static bool audioMute = settings.muteAudio;
        if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                ManageDevAudio(audioPaths.dataDir, audioMute);
                return 0;
            }, nullptr, 0, nullptr)) {
            CloseHandle(t);
        }
    }
    if (!LoadRealDinput8()) {
        RM_LOGE("system dinput8.dll could not be loaded; input will fail");
    }
    if (!engine::VerifySignatures(120000)) {
        RM_LOGE("engine verification failed; gameplay hooks disabled (overlay still attempted)");
    }
    if (MH_Initialize() != MH_OK) {
        RM_LOGE("MinHook initialization failed");
        return 0;
    }

    probe::Configure(settings);
    if (settings.developerMode) nativemenu::SetDevSimulation(settings.devNoExtraContent, settings.devMercenariesLocked);
    if (settings.developerMode) {
        ConfigureDevCapture(paths.dataDir);
    }
    ModInit(paths, settings);
    SetHookSink(ModSink());
    overlay::SetToggleKey(settings.overlayKey);
    overlay::SetDrawCallback(settings.disableOverlayDraw ? nullptr : &ModDraw);

    if (settings.disableInputHooks) {
        RM_LOGW("dev: input hooks disabled");
    } else if (!InstallInputHooks(RealDirectInput8Create())) {
        RM_LOGE("input hooks failed: no game tick and no input blocking");
    }
    if (settings.disableD3d9Hooks) {
        RM_LOGW("dev: d3d9 hooks disabled");
    } else if (!InstallD3d9Hooks()) {
        RM_LOGE("d3d9 hooks failed: no overlay");
    }
    if (engine::Caps().buildIdentity) {
        InstallFatalErrorHook();
        engine::ApplyCodeFixes();
    }
    if (settings.disableCombatHooks) {
        RM_LOGW("dev: combat hooks disabled");
    } else if (engine::Caps().buildIdentity) {
        InstallCombatHooks();
        InstallMercHooks();
    }
    if (settings.nativeMenu && engine::Caps().buildIdentity) {
        nativemenu::Install();
        if (settings.developerMode) nativemenu::EnableDrawProbe();
        if (settings.nativeMatchUi) matchui::Install();
        itemboxguard::Install();
        director::Install();
        resources::Install();
        world::Install();
        host::Install();
        areacams::Install();
        stagefix::Install();
        if (settings.directStart) autopilot::Install();
    }
    if (settings.developerMode && settings.devMenuProbe && engine::Caps().buildIdentity) {
        menuprobe::Install();
    }
    // Supported third-party mods: their files read from their own folder (empty until a mod skin is chosen).
    fileredirect::Install();
    // Enable everything in one batch: MinHook suspends every thread per apply,
    // and repeated suspensions coincided with silent clean exits of the game.
    const DWORD applyStart = GetTickCount();
    const MH_STATUS applied = MH_ApplyQueued();
    if (applied != MH_OK) {
        RM_LOGE("MH_ApplyQueued failed: %s; no hooks active", MH_StatusToString(applied));
        MarkHooksInactive();
    } else {
        RM_LOGI("all queued hooks enabled in one batch (%lu ms)", GetTickCount() - applyStart);
    }
    ModCapabilitiesReady();
    RM_LOGI("initialization complete");
    return 0;
}

// No exception may leave this thread: it would end the game, not only the mod.
DWORD WINAPI Worker(LPVOID) {
    try {
        return StartMod();
    } catch (const std::exception& e) {
        RM_LOGE("the mod could not start: %s", e.what());
    } catch (...) {
        RM_LOGE("the mod could not start: unknown exception");
    }
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        // One copy runs the mod: with this DLL both as the game's dinput8.dll and in another
        // loader's plugins folder, the second one only forwards DirectInput (its exports).
        wchar_t name[64];
        swprintf_s(name, L"Local\\RogueMercenaries.%lu", GetCurrentProcessId());
        HANDLE once = CreateMutexW(nullptr, FALSE, name);
        if (once != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(once);
            g_module = nullptr;  // not the running copy: no shutdown on detach either
            return TRUE;
        }
        if (HANDLE thread = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_module == nullptr) return TRUE;  // the idle copy (DLL_PROCESS_ATTACH)
        // reserved != nullptr: the process is exiting (ExitProcess), not FreeLibrary.
        RM_LOGI("process detach (%s)", reserved ? "process exit" : "FreeLibrary");
        if (reserved == nullptr) {
            rmre5::ModShutdown();
            rmre5::LogClose();
        }
    }
    return TRUE;
}
