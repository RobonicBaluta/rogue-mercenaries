#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD userIndex, void* state);

int Fail(const char* message, DWORD error = 0) {
    std::fprintf(stderr, "FAIL: %s (error=%lu)\n", message,
                 static_cast<unsigned long>(error));
    return 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fwprintf(stderr, L"Usage: proxy_smoke.exe <absolute-path-to-xinput1_3.dll>\n");
        return 2;
    }

    const std::wstring dllPath = argv[1];
    HMODULE proxy = LoadLibraryW(dllPath.c_str());
    if (proxy == nullptr) {
        return Fail("LoadLibraryW failed", GetLastError());
    }

    auto getState = reinterpret_cast<XInputGetStateFn>(
        GetProcAddress(proxy, "XInputGetState"));
    if (getState == nullptr) {
        const DWORD error = GetLastError();
        FreeLibrary(proxy);
        return Fail("XInputGetState export is missing", error);
    }

    alignas(16) std::array<std::uint8_t, 64> state{};
    const DWORD result = getState(0, state.data());
    std::printf("LoadLibraryW=OK\nXInputGetState=OK\nforwarded_result=%lu\n",
                static_cast<unsigned long>(result));

    // RE5Patch initializes asynchronously after DLL_PROCESS_ATTACH. Keep the
    // module mapped until its worker has finished before exercising detach.
    Sleep(2000);
    FreeLibrary(proxy);
    if (result != ERROR_SUCCESS && result != ERROR_DEVICE_NOT_CONNECTED) {
        return Fail("forwarded XInput call returned an unexpected status", result);
    }

    return 0;
}
