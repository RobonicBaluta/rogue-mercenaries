// dinput8.dll proxy: forwards the five public exports to the system DLL.
// RE5 imports DINPUT8.dll, so this module loads at process start.

#include <windows.h>

#include <unknwn.h>

#include <string>

#include "hooks.h"
#include "proxy.h"

namespace {

HMODULE g_real = nullptr;
FARPROC g_create = nullptr;
FARPROC g_canUnload = nullptr;
FARPROC g_getClassObject = nullptr;
FARPROC g_register = nullptr;
FARPROC g_unregister = nullptr;

}  // namespace

namespace rmre5 {

bool LoadRealDinput8() {
    if (g_real) {
        return true;
    }
    wchar_t system[MAX_PATH];
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);  // SysWOW64 for this 32-bit process
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    const std::wstring path = std::wstring(system) + L"\\dinput8.dll";
    g_real = LoadLibraryW(path.c_str());
    if (!g_real) {
        return false;
    }
    g_create = GetProcAddress(g_real, "DirectInput8Create");
    g_canUnload = GetProcAddress(g_real, "DllCanUnloadNow");
    g_getClassObject = GetProcAddress(g_real, "DllGetClassObject");
    g_register = GetProcAddress(g_real, "DllRegisterServer");
    g_unregister = GetProcAddress(g_real, "DllUnregisterServer");
    return g_create != nullptr;
}

void* RealDirectInput8Create() { return reinterpret_cast<void*>(g_create); }

}  // namespace rmre5

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    if (!rmre5::LoadRealDinput8()) {
        return E_FAIL;
    }
    rmre5::RecordDirectInputCreate(&riid);
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    return reinterpret_cast<Fn>(g_create)(instance, version, riid, out, outer);
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
    if (!rmre5::LoadRealDinput8() || !g_canUnload) {
        return S_FALSE;
    }
    return reinterpret_cast<HRESULT(WINAPI*)()>(g_canUnload)();
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    if (!rmre5::LoadRealDinput8() || !g_getClassObject) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>(g_getClassObject)(clsid, riid, out);
}

HRESULT WINAPI Proxy_DllRegisterServer() {
    if (!rmre5::LoadRealDinput8() || !g_register) {
        return E_FAIL;
    }
    return reinterpret_cast<HRESULT(WINAPI*)()>(g_register)();
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
    if (!rmre5::LoadRealDinput8() || !g_unregister) {
        return E_FAIL;
    }
    return reinterpret_cast<HRESULT(WINAPI*)()>(g_unregister)();
}

}  // extern "C"
