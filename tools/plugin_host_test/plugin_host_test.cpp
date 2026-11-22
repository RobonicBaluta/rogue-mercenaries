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
    CreateDirectoryW(game.c_str(), nullptr);
    CreateDirectoryW(plugins.c_str(), nullptr);
    bool ready = Copy(plain, plugins + L"a_plain.dll") && Copy(fix, plugins + L"b_fix.dll") && Copy(fix, plugins + L"c_fix2.dll") &&
                 Copy(fix, plugins + L"RogueMercenaries.dll") && Copy(plain, plugins + L"x.asi") && Copy(plain, game + L"y.asi") &&
                 Copy(plain, game + L"z.dll");  // a DLL next to the game: not a plugin
    WriteText(plugins + L"broken.dll", "not a DLL");
    WriteText(plugins + L"README.txt", "notes");
    Check(ready, "stand-in game folder made");

    rmre5::plugins::LoadAll(GetModuleHandleW(nullptr), game);
    const auto& entries = rmre5::plugins::Entries();
    for (const auto& e : entries) {
        std::wprintf(L"      %ls: %ls%ls %hs\n", e.path.c_str(), e.loaded ? L"loaded" : e.skipped ? L"left out" : L"NOT loaded",
                     e.chained ? L" (chained)" : L"", e.why);
    }
    Check(entries.size() == 7, "seven files looked at (5 .dll and 1 .asi in plugins, 1 .asi next to the game)");
    const wchar_t* order[] = {L"a_plain.dll", L"b_fix.dll", L"broken.dll", L"c_fix2.dll", L"RogueMercenaries.dll", L"x.asi", L"y.asi"};
    bool inOrder = entries.size() == 7;
    for (std::size_t i = 0; inOrder && i < 7; ++i) {
        const std::size_t slash = entries[i].path.find_last_of(L"\\/");
        inOrder = _wcsicmp(entries[i].path.c_str() + slash + 1, order[i]) == 0;
    }
    Check(inOrder, "plugins' .dll in name order, then their .asi, then the game folder's .asi");
    const auto* a = Find(L"a_plain.dll");
    const auto* b = Find(L"b_fix.dll");
    const auto* c = Find(L"c_fix2.dll");
    const auto* broken = Find(L"broken.dll");
    const auto* rm = Find(L"RogueMercenaries.dll");
    Check(a && a->loaded && !a->directInput, "a plain plugin loads");
    Check(b && b->loaded && b->directInput && b->chained, "the first plugin with DirectInput8Create is chained");
    Check(c && c->loaded && c->directInput && !c->chained, "a second one loads but is not chained");
    Check(broken && !broken->loaded && !broken->skipped && broken->error != 0, "a file that is no DLL fails with its error, nothing else happens");
    Check(rm && rm->skipped && !rm->loaded && GetModuleHandleW((plugins + L"RogueMercenaries.dll").c_str()) == nullptr,
          "an old RogueMercenaries.dll in plugins is left out, never loaded");
    Check(Find(L"x.asi") && Find(L"x.asi")->loaded && Find(L"y.asi") && Find(L"y.asi")->loaded, "the .asi files load");
    Check(Find(L"z.dll") == nullptr, "a DLL next to the game is not a plugin");

    LPVOID out = nullptr;
    HRESULT hr = rmre5::plugins::DirectInput8Create(&Fallback, nullptr, 0x0800, IID_IUnknown, &out, nullptr);
    Check(hr == 0x00000022 && Calls(L"b_fix.dll") == 1 && Calls(L"c_fix2.dll") == 0 && g_fallbackCalls == 0,
          "the game's DirectInput8Create goes through the chained plugin");

    // The chained plugin calls back into the host: the inner call goes to the system's, no loop.
    const HMODULE bm = GetModuleHandleW((plugins + L"b_fix.dll").c_str());
    const auto setCallback = bm ? reinterpret_cast<void (*)(HRESULT(WINAPI*)())>(GetProcAddress(bm, "DummySetCallback")) : nullptr;
    Check(setCallback != nullptr, "the dummy takes a callback");
    if (setCallback) setCallback(&CallBack);
    hr = rmre5::plugins::DirectInput8Create(&Fallback, nullptr, 0x0800, IID_IUnknown, &out, nullptr);
    Check(hr == 0x00000011 && Calls(L"b_fix.dll") == 2 && g_fallbackCalls == 1, "a call back into the host goes to the system's, once");
    if (setCallback) setCallback(nullptr);
    hr = rmre5::plugins::DirectInput8Create(&Fallback, nullptr, 0x0800, IID_IUnknown, &out, nullptr);
    Check(hr == 0x00000022 && Calls(L"b_fix.dll") == 3, "and the next call goes through the plugin again");

    std::printf(g_failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
