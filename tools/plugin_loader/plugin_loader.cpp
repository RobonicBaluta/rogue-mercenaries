// Developer test only, never shipped: a dinput8.dll that does what the RE5 plugin loaders players use
// are documented to do, to try Rogue Mercenaries as another loader's plugin (2026-11-18, a player:
// the RE5 input fixes need their own dinput8.dll). At load it loads every DLL in the game's
// plugins folder (Wesky's "Resident Evil 5 - Plugin Loader": "every Dynamic Link Library file in
// the plugins folder gets injected and executed at game launch") and every .asi file next to the
// game (as ASI loaders do), then forwards DirectInput8Create to the system dinput8.dll.
#include <windows.h>

#include <string>

namespace {

HMODULE g_real = nullptr;
FARPROC g_create = nullptr;

void LoadAll(const std::wstring& dir, const wchar_t* pattern) {
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW((dir + pattern).c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        LoadLibraryW((dir + found.cFileName).c_str());
    } while (FindNextFileW(h, &found));
    FindClose(h);
}

}  // namespace

extern "C" HRESULT WINAPI Loader_DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out, void* outer) {
    if (!g_real) {
        wchar_t system[MAX_PATH];
        GetSystemDirectoryW(system, MAX_PATH);
        g_real = LoadLibraryW((std::wstring(system) + L"\\dinput8.dll").c_str());
        g_create = g_real ? GetProcAddress(g_real, "DirectInput8Create") : nullptr;
    }
    if (!g_create) return E_FAIL;
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
    return reinterpret_cast<Fn>(g_create)(instance, version, riid, out, outer);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        LoadAll(dir + L"plugins\\", L"*.dll");
        LoadAll(dir, L"*.asi");
    }
    return TRUE;
}
