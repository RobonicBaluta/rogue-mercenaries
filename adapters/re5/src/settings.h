#pragma once

// adapter.json (next to the mod's configs in <game>\RogueMercenaries\).
// Missing or invalid files fall back to these defaults and log why.

#include <filesystem>
#include <string>

namespace rmre5 {

struct Paths {
    std::filesystem::path gameDir;     // folder containing re5dx9.exe and this DLL
    std::filesystem::path modDir;      // <game>\RogueMercenaries
    std::filesystem::path configDir;   // <game>\RogueMercenaries\configs
    std::filesystem::path dataDir;     // %LOCALAPPDATA%\RogueMercenaries
    std::filesystem::path logDir;      // <data>\logs
};

struct Settings {
    std::string logLevel = "info";
    int overlayKey = 0x76;  // VK_F7
    bool developerMode = false;
    // Diagnostics: log engine state every N ms (0 = off) and every hit/kill.
    int probeIntervalMs = 0;
    bool probeHits = false;
    // Silences this process' audio session; for unattended test launches.
    bool muteAudio = false;
    // Component switches for bisecting startup problems.
    bool disableInputHooks = false;
    bool disableD3d9Hooks = false;
    bool disableCombatHooks = false;
    bool disableOverlayDraw = false;
    // Scripted run with synthetic kills on a sandbox profile, for UI captures.
    bool uiDemo = false;
    // Mercenaries timer handling while an arena is active:
    //   "hold"  keeps the countdown at or above timerHoldSeconds
    //   "game"  leaves the game's own countdown alone
    std::string timerPolicy = "hold";
    float timerHoldSeconds = 90.0f;
    // End the Mercenaries match (timer -> 0) once the arena's kill budget is met.
    bool endMatchOnClear = true;
};

Paths ResolvePaths(void* module);
Settings LoadSettings(const Paths& paths, std::string* error);

}  // namespace rmre5
