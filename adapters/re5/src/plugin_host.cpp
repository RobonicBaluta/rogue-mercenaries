#include "plugin_host.h"

#include <algorithm>
#include <cwchar>

namespace rmre5::plugins {
namespace {

std::vector<Entry> g_entries;
// Every plugin with DirectInput8Create, in load order: the game's call goes through the first, and its
// call to "dinput8.dll" (by name, this DLL) through the next, and so on, the last one's to the system's.
std::vector<CreateFn> g_chain;
thread_local int t_inChain = 0;

bool HasExtension(const std::wstring& name, const wchar_t* ext) {
    const std::size_t n = std::wcslen(ext);
    return name.size() > n && _wcsicmp(name.c_str() + name.size() - n, ext) == 0;
}

std::wstring Stem(const std::wstring& name) {
    const std::size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

// The files of `dir` matching `pattern` with exactly that extension (a short 8.3 name can make
// "*.dll" match "a.dllx"), in name order.
std::vector<std::wstring> Files(const std::wstring& dir, const wchar_t* pattern, const wchar_t* ext) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW((dir + pattern).c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name = found.cFileName;
        if (HasExtension(name, ext)) out.push_back(name);
    } while (FindNextFileW(h, &found));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

// The subfolders of `dir` (a mod's own folder dropped in plugins\ as it comes), in name order.
std::vector<std::wstring> Folders(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring name = found.cFileName;
        if (name != L"." && name != L"..") out.push_back(name);
    } while (FindNextFileW(h, &found));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

// No C++ objects here (__try): a plugin whose own start throws is a failed load, not the game's end.
// LOAD_WITH_ALTERED_SEARCH_PATH: the DLLs a plugin needs are looked for next to it first (in plugins\
// or its own folder), not only next to the game.
HMODULE LoadGuarded(const wchar_t* path, DWORD* error) {
    HMODULE module = nullptr;
    __try {
        module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        *error = module ? 0 : GetLastError();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        module = nullptr;
        *error = GetExceptionCode();
    }
    return module;
}

}  // namespace

void LoadAll(HMODULE self, const std::wstring& gameDir) {
    if (!g_entries.empty()) return;  // once
    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(self, selfPath, MAX_PATH);
    std::wstring dir = gameDir;
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    const std::wstring pluginDir = dir + L"plugins\\";
    struct Source {
        const std::wstring* dir;
        const wchar_t* pattern;
        const wchar_t* ext;
    };
    std::vector<Source> sources = {{&pluginDir, L"*.dll", L".dll"}, {&pluginDir, L"*.asi", L".asi"}};
    // A mod's own folder in plugins\ as it comes (its files beside it): its dinput8.dll and its .asi.
    std::vector<std::wstring> folders;
    for (const std::wstring& f : Folders(pluginDir)) folders.push_back(pluginDir + f + L"\\");
    for (const std::wstring& f : folders) {
        sources.push_back({&f, L"dinput8.dll", L".dll"});
        sources.push_back({&f, L"*.asi", L".asi"});
    }
    sources.push_back({&dir, L"*.asi", L".asi"});
    // A plugin that cannot load says so in the log, not in a message box at the game's start.
    DWORD oldMode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &oldMode);
    for (const Source& source : sources) {
        for (const std::wstring& name : Files(*source.dir, source.pattern, source.ext)) {
            Entry e;
            e.path = *source.dir + name;
            if (_wcsicmp(e.path.c_str(), selfPath) == 0) {
                e.skipped = true;
                e.why = "this DLL";
            } else if (_wcsicmp(Stem(name).c_str(), L"RogueMercenaries") == 0) {
                e.skipped = true;
                e.why = "a copy of Rogue Mercenaries (the dinput8.dll is the mod now; this file can go)";
            } else {
                const HMODULE module = LoadGuarded(e.path.c_str(), &e.error);
                if (module == self) {
                    FreeLibrary(module);
                    e.skipped = true;
                    e.why = "this DLL";
                } else if (module != nullptr) {
                    e.loaded = true;
                    if (const FARPROC create = GetProcAddress(module, "DirectInput8Create")) {
                        e.directInput = true;
                        e.chained = true;
                        e.why = g_chain.empty() ? "a dinput8.dll of its own: the game's DirectInput goes through it"
                                                : "a dinput8.dll of its own too: the game's DirectInput goes through it after the one before";
                        g_chain.push_back(reinterpret_cast<CreateFn>(create));
                    }
                } else {
                    e.why = "could not be loaded";
                }
            }
            g_entries.push_back(std::move(e));
        }
    }
    SetThreadErrorMode(oldMode, nullptr);
}

const std::vector<Entry>& Entries() { return g_entries; }

HRESULT DirectInput8Create(CreateFn fallback, HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    const int at = t_inChain;
    if (at >= 0 && static_cast<std::size_t>(at) < g_chain.size()) {
        ++t_inChain;
        const HRESULT result = g_chain[static_cast<std::size_t>(at)](instance, version, riid, out, outer);
        --t_inChain;
        return result;
    }
    return fallback ? fallback(instance, version, riid, out, outer) : E_FAIL;
}

}  // namespace rmre5::plugins
