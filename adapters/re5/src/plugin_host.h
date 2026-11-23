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
    bool chained = false;      // the game's DirectInput goes through it (every such plugin, in load order)
    const char* why = "";
};

// Loads every .dll and then every .asi of <gameDir>\plugins, then of each folder in it (a mod's own
// folder as it comes) its dinput8.dll and its .asi, then every .asi next to the game, each in name order
// (case-insensitive); any name will do, dinput8.dll too, and a plugin's own DLLs are found next to it. Leaves out this DLL itself and copies of Rogue Mercenaries
// (RogueMercenaries.dll or .asi: the plugin download of earlier versions). Called once, from
// DllMain, only when this DLL is the game's dinput8.dll (the other mods' own load time).
void LoadAll(HMODULE self, const std::wstring& gameDir);
[[nodiscard]] const std::vector<Entry>& Entries();

using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
// DirectInput8Create through every plugin that has one, in load order: the game's call to the first; a
// call that comes back into this DLL while one runs (it loaded "dinput8.dll" by name and got this one)
// to the next, and after the last to `fallback` (the system's), never round again.
HRESULT DirectInput8Create(CreateFn fallback, HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer);

}  // namespace rmre5::plugins
