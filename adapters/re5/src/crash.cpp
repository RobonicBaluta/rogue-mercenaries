#include "crash.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

#include "log.h"

namespace rmre5 {
namespace {

thread_local bool t_guarded = false;
std::atomic<int> g_reported{0};
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

bool IsFatalClass(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN (fast fail)
            return true;
        default:
            return false;
    }
}

void Describe(const EXCEPTION_POINTERS* info, const char* kind) {
    const auto* record = info->ExceptionRecord;
    const auto address = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(record->ExceptionAddress), &module)) {
        GetModuleFileNameA(module, name, MAX_PATH);
    }
    const char* base = std::strrchr(name, '\\');
    const auto* ctx = info->ContextRecord;
    RM_LOGE("%s exception 0x%08lX at %08X (%s+0x%X) info=[%08X %08X] eax=%08lX ecx=%08lX edx=%08lX esi=%08lX "
            "edi=%08lX esp=%08lX",
            kind, record->ExceptionCode, static_cast<unsigned>(address), base ? base + 1 : name,
            static_cast<unsigned>(address - reinterpret_cast<std::uintptr_t>(module)),
            record->NumberParameters > 0 ? static_cast<unsigned>(record->ExceptionInformation[0]) : 0u,
            record->NumberParameters > 1 ? static_cast<unsigned>(record->ExceptionInformation[1]) : 0u, ctx->Eax,
            ctx->Ecx, ctx->Edx, ctx->Esi, ctx->Edi, ctx->Esp);
}

// The executable's packer stub (section at VA 0x0137F000, size 0x764000)
// raises divide-by-zero and illegal-instruction exceptions on purpose during
// startup and handles them itself; they are not faults.
bool IsPackerTrick(const EXCEPTION_RECORD* record) {
    const auto address = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
    return address >= 0x0137F000 && address < 0x01AE3000 && record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION;
}

constexpr DWORD kCppException = 0xE06D7363;
std::atomic<int> g_cppReported{0};

LONG CALLBACK Vectored(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (!t_guarded && IsFatalClass(code) && !IsPackerTrick(info->ExceptionRecord) && g_reported.fetch_add(1) < 40) {
        Describe(info, "first-chance");
    } else if (code == kCppException && g_cppReported.fetch_add(1) < 10) {
        // Usually caught by our own guards; logged so an escaping one is visible.
        Describe(info, "c++ throw");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI Unhandled(EXCEPTION_POINTERS* info) {
    Describe(info, "UNHANDLED");
    return g_previousFilter ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void SetGuardedAccess(bool active) { t_guarded = active; }

void InstallCrashDiagnostics() {
    AddVectoredExceptionHandler(1, &Vectored);
    g_previousFilter = SetUnhandledExceptionFilter(&Unhandled);
    RM_LOGI("crash diagnostics installed");
}

}  // namespace rmre5
