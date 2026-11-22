// Developer test only, never shipped: a stand-in for another RE5 mod in plugins\ (tools/plugin_host_test).
// Built twice: with DirectInput8Create (another mod's dinput8.dll, an input fix) and, with DUMMY_PLAIN,
// without (a plain plugin).
#include <windows.h>

#include <unknwn.h>

namespace {
long g_calls = 0;
using Callback = HRESULT(WINAPI*)();
Callback g_callback = nullptr;
}  // namespace

extern "C" {

// cdecl: exported by its plain name on x86 (a stdcall one would be _DummyCalls@0).
__declspec(dllexport) long DummyCalls() { return g_calls; }

#ifndef DUMMY_PLAIN
__declspec(dllexport) void DummySetCallback(Callback callback) { g_callback = callback; }

// What an input fix's dinput8.dll does: the game's DirectInput8Create comes here. With a callback
// set it calls back into "dinput8.dll" as a careless proxy would (by name, getting the host's own).
HRESULT WINAPI Dummy_DirectInput8Create(HINSTANCE, DWORD, REFIID, LPVOID* out, LPUNKNOWN) {
    ++g_calls;
    if (g_calls > 5) return E_UNEXPECTED;  // a loop
    if (g_callback) return g_callback();
    if (out) *out = nullptr;
    return 0x00000022;
}
#endif

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
