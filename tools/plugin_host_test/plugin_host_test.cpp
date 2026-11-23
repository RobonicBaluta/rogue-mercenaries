// Developer test only, never shipped: the plugin host (adapters/re5/src/plugin_host.cpp) on a stand-in
// game folder in %TEMP%: what it loads, what it leaves out, which plugin the game's DirectInput goes
// through, and that a plugin calling back into "dinput8.dll" cannot loop. Exit code 0 when all pass.
//   build\win32\tools\plugin_host_test\Release\rm_plugin_host_test.exe
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "plugin_host.h"

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

std::wstring Here() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

bool Copy(const std::wstring& from, const std::wstring& to) { return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0; }

void WriteText(const std::wstring& path, const char* text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    WriteFile(h, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    CloseHandle(h);
}

const rmre5::plugins::Entry* Find(const wchar_t* name) {
    for (const auto& e : rmre5::plugins::Entries()) {
        const std::size_t slash = e.path.find_last_of(L"\\/");
        if (_wcsicmp(e.path.c_str() + slash + 1, name) == 0) return &e;
    }
    return nullptr;
}

int g_fallbackCalls = 0;
HRESULT WINAPI Fallback(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN) {
    ++g_fallbackCalls;
    return 0x00000011;
}

// The chained plugin calling back into "dinput8.dll": the host's own export again.
HRESULT WINAPI CallBack() {
    LPVOID out = nullptr;
    return rmre5::plugins::DirectInput8Create(&Fallback, nullptr, 0x0800, IID_IUnknown, &out, nullptr);
}

long Calls(const wchar_t* name) {
    const std::wstring path = std::wstring(L"plugins\\") + name;
    for (const auto& e : rmre5::plugins::Entries()) {
        if (e.path.size() >= path.size() && _wcsicmp(e.path.c_str() + e.path.size() - path.size(), path.c_str()) == 0) {
            const HMODULE m = GetModuleHandleW(e.path.c_str());
            const auto fn = m ? reinterpret_cast<long (*)()>(GetProcAddress(m, "DummyCalls")) : nullptr;
            return fn ? fn() : -1;
        }
    }
    return -1;
}

}  // namespace

int wmain() {
    const std::wstring here = Here();
    const std::wstring fix = here + L"dummy_plugin.dll";
    const std::wstring plain = here + L"plain_plugin.dll";
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring game = std::wstring(temp) + L"rm_plugin_host_test_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    const std::wstring plugins = game + L"plugins\\";
    const std::wstring folder = plugins + L"SomeMod\\";
    CreateDirectoryW(game.c_str(), nullptr);
    CreateDirectoryW(plugins.c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    bool ready = Copy(plain, plugins + L"a_plain.dll") && Copy(fix, plugins + L"b_fix.dll") && Copy(fix, plugins + L"c_fix2.dll") &&
                 Copy(fix, plugins + L"RogueMercenaries.dll") && Copy(plain, plugins + L"x.asi") && Copy(plain, game + L"y.asi") &&
                 Copy(plain, game + L"z.dll") &&          // a DLL next to the game: not a plugin
                 Copy(plain, game + L"dinput8.dll") &&    // the host itself, as the game loads it
                 Copy(plain, plugins + L"dinput8.dll") && // another mod's dinput8.dll under its own name
                 Copy(plain, folder + L"dinput8.dll") &&  // a mod's own folder as it comes
                 Copy(plain, folder + L"helper.dll") &&   // its other DLLs: not plugins
                 Copy(plain, folder + L"m.asi");
    WriteText(plugins + L"broken.dll", "not a DLL");
    WriteText(plugins + L"README.txt", "notes");
    WriteText(folder + L"dinput8.ini", "[settings]");
    Check(ready, "stand-in game folder made");

    // The host is the game folder's dinput8.dll: a module of that name is loaded already.
    const HMODULE self = LoadLibraryW((game + L"dinput8.dll").c_str());
    Check(self != nullptr, "the stand-in host (the game folder's dinput8.dll) is loaded");
    rmre5::plugins::LoadAll(self, game);
    const auto& entries = rmre5::plugins::Entries();
    for (const auto& e : entries) {
        std::wprintf(L"      %ls: %ls%ls %hs\n", e.path.c_str(), e.loaded ? L"loaded" : e.skipped ? L"left out" : L"NOT loaded",
                     e.chained ? L" (chained)" : L"", e.why);
    }
    const wchar_t* order[] = {L"plugins\\a_plain.dll", L"plugins\\b_fix.dll", L"plugins\\broken.dll", L"plugins\\c_fix2.dll",
                              L"plugins\\dinput8.dll", L"plugins\\RogueMercenaries.dll", L"plugins\\x.asi",
                              L"SomeMod\\dinput8.dll", L"SomeMod\\m.asi", L"_plugin_host_test_"};
    constexpr std::size_t kLooked = sizeof(order) / sizeof(order[0]);
    Check(entries.size() == kLooked, "ten files looked at (6 .dll and 1 .asi in plugins, a folder's dinput8.dll and .asi, 1 .asi next to the game)");
    bool inOrder = entries.size() == kLooked;
    for (std::size_t i = 0; inOrder && i < kLooked - 1; ++i) {
        const std::wstring want = order[i];
        inOrder = entries[i].path.size() >= want.size() &&
                  _wcsicmp(entries[i].path.c_str() + entries[i].path.size() - want.size(), want.c_str()) == 0;
    }
    inOrder = inOrder && entries.back().path == game + L"y.asi";
    Check(inOrder, "plugins' .dll in name order, then their .asi, then a folder's dinput8.dll and .asi, then the game folder's .asi");
    const auto* a = Find(L"a_plain.dll");
    const auto* b = Find(L"b_fix.dll");
    const auto* c = Find(L"c_fix2.dll");
    const auto* broken = Find(L"broken.dll");
    const auto* rm = Find(L"RogueMercenaries.dll");
    Check(a && a->loaded && !a->directInput, "a plain plugin loads");
    Check(b && b->loaded && b->directInput && b->chained, "the first plugin with DirectInput8Create is chained");
    Check(c && c->loaded && c->directInput && c->chained, "a second one is chained too, after it");
    Check(broken && !broken->loaded && !broken->skipped && broken->error != 0, "a file that is no DLL fails with its error, nothing else happens");
    Check(rm && rm->skipped && !rm->loaded && GetModuleHandleW((plugins + L"RogueMercenaries.dll").c_str()) == nullptr,
          "an old RogueMercenaries.dll in plugins is left out, never loaded");
    Check(Find(L"x.asi") && Find(L"x.asi")->loaded && Find(L"y.asi") && Find(L"y.asi")->loaded, "the .asi files load");
    Check(Find(L"z.dll") == nullptr, "a DLL next to the game is not a plugin");
    // plugins\dinput8.dll under its own name: its own module, not the host's.
    const HMODULE own = GetModuleHandleW((plugins + L"dinput8.dll").c_str());
    bool ownLoaded = false;
    for (const auto& e : entries) {
        if (e.path == plugins + L"dinput8.dll") ownLoaded = e.loaded;
    }
    Check(ownLoaded && own != nullptr && own != self, "another mod's plugins\\dinput8.dll keeps its name and loads as its own module");
    bool folderLoaded = false, folderAsi = false;
    for (const auto& e : entries) {
        if (e.path == folder + L"dinput8.dll") folderLoaded = e.loaded;
        if (e.path == folder + L"m.asi") folderAsi = e.loaded;
    }
    Check(folderLoaded && folderAsi, "a mod's own folder in plugins: its dinput8.dll and its .asi load");
    Check(Find(L"helper.dll") == nullptr && GetModuleHandleW((folder + L"helper.dll").c_str()) == nullptr,
          "that folder's other DLLs are not plugins");

    LPVOID out = nullptr;
    HRESULT hr = rmre5::plugins::DirectInput8Create(&Fallback, nullptr, 0x0800, IID_IUnknown, &out, nullptr);
    Check(hr == 0x00000022 && Calls(L"b_fix.dll") == 1 && Calls(L"c_fix2.dll") == 0 && g_fallbackCalls == 0,
          "the game's DirectInput8Create goes through the first chained plugin");

    // The first calls back into the host: on to the second; the second calls back: on to the system's.
    const auto setter = [&](const wchar_t* name) {
        const HMODULE m = GetModuleHandleW((plugins + name).c_str());
        return m ? reinterpret_cast<void (*)(HRESULT(WINAPI*)())>(GetProcAddress(m, "DummySetCallback")) : nullptr;
    };
    const auto setB = setter(L"b_fix.dll");
    const auto setC = setter(L"c_fix2.dll");
    Check(setB != nullptr && setC != nullptr, "the dummies take a callback");
    if (setB) setB(&CallBack);
    hr = rmre5::plugins::DirectInput8Create(&Fallback, nullptr, 0x0800, IID_IUnknown, &out, nullptr);
    Check(hr == 0x00000022 && Calls(L"b_fix.dll") == 2 && Calls(L"c_fix2.dll") == 1 && g_fallbackCalls == 0,
          "a call back into the host from the first goes on to the second");
    if (setC) setC(&CallBack);
    hr = rmre5::plugins::DirectInput8Create(&Fallback, nullptr, 0x0800, IID_IUnknown, &out, nullptr);
    Check(hr == 0x00000011 && Calls(L"b_fix.dll") == 3 && Calls(L"c_fix2.dll") == 2 && g_fallbackCalls == 1,
          "from the last, to the system's, once: no loop");
    if (setB) setB(nullptr);
    if (setC) setC(nullptr);
    hr = rmre5::plugins::DirectInput8Create(&Fallback, nullptr, 0x0800, IID_IUnknown, &out, nullptr);
    Check(hr == 0x00000022 && Calls(L"b_fix.dll") == 4, "and the next call goes through the first again");

    std::printf(g_failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
