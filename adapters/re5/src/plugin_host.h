#pragma once

// The plugin host (2026-11-22, the user: "can we make it that the releases is just one, and it a
// dinput8 that uses the plugin things so people can just add whatever other mod they are using to
// the plugins"). As the game's dinput8.dll, Rogue Mercenaries loads the other RE5 mods the way the
// scene's dinput8.dll loaders do (Wesky's "Resident Evil 5 - Plugin Loader": every DLL in plugins\
// at launch; ASI loaders: the .asi files): another mod's dinput8.dll moved into plugins\ keeps
// working, and when it has DirectInput8Create (an input fix that wraps the devices), the game's
// DirectInput goes through it.

#include <windows.h>

#include <unknwn.h>

#include <string>
#include <vector>

namespace rmre5::plugins {

// One file the host looked at.
struct Entry {
    std::wstring path;
    bool loaded = false;
    bool skipped = false;      // left out on purpose (why)
    DWORD error = 0;           // LoadLibrary's error (or an exception code) when it failed
    bool directInput = false;  // it exports DirectInput8Create (another mod's dinput8.dll)
    bool chained = false;      // the game's DirectInput goes through it (the first such plugin)
    const char* why = "";
};

// Loads every .dll and then every .asi of <gameDir>\plugins, then every .asi next to the game, each
// in name order (case-insensitive). Leaves out this DLL itself and copies of Rogue Mercenaries
// (RogueMercenaries.dll or .asi: the plugin download of earlier versions). Called once, from
// DllMain, only when this DLL is the game's dinput8.dll (the other mods' own load time).
void LoadAll(HMODULE self, const std::wstring& gameDir);
[[nodiscard]] const std::vector<Entry>& Entries();

using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
// DirectInput8Create through the first plugin that has one, else `fallback` (the system's). A call
// that comes back into this DLL while that plugin's runs (it loaded "dinput8.dll" by name and got
// this one) goes to `fallback`, never round again.
HRESULT DirectInput8Create(CreateFn fallback, HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer);

}  // namespace rmre5::plugins
