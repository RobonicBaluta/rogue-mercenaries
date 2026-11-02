#include "crash.h"

#include <windows.h>

#include <MinHook.h>

#include <cstdarg>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <cstdint>
#include <cstdio>
#include <string>

#include "engine.h"
#include "hooks.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"

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

// Developer: the game's main thread stopped presenting frames. Up to six samples of where it is
// (instruction pointer and the game code addresses on its stack), read while the thread is
// briefly suspended and logged after it runs again (it might hold the log's lock).
DWORD WINAPI HangWatchdog(LPVOID) {
    unsigned long long lastPresents = 0;
    DWORD lastChange = GetTickCount();
    int samples = 0;
    for (;;) {
        Sleep(500);
        const HookStatus& status = Status();
        if (status.presents != lastPresents) {
            if (samples > 0) RM_LOGW("hang watchdog: frames again after %lu ms", GetTickCount() - lastChange);
            lastPresents = status.presents;
            lastChange = GetTickCount();
            samples = 0;
            continue;
        }
        const DWORD stalled = GetTickCount() - lastChange;
        if (lastPresents == 0 || stalled < 4000 || samples >= 6 || !status.gameThreadId) continue;
        ++samples;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, status.gameThreadId);
        if (!thread) continue;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        bool read = false;
        static std::uint32_t stack[2048];  // 8 KB from the stack pointer (this thread only)
        std::fill(std::begin(stack), std::end(stack), 0u);
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            read = GetThreadContext(thread, &context) != FALSE;
            for (int i = 0; read && i < 2048; ++i) {
                if (!SafeRead(context.Esp + static_cast<std::uintptr_t>(i) * 4, stack[i])) break;
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
        if (!read) continue;
        std::string code;
        int found = 0;
        for (int i = 0; i < 512 && found < 24; ++i) {
            if (stack[i] >= 0x00401000 && stack[i] < 0x00F4A000) {
                char buf[12];
                std::snprintf(buf, sizeof(buf), " %08X", stack[i]);
                code += buf;
                ++found;
            }
        }
        RM_LOGW("hang watchdog: no frame for %lu ms; main thread eip %08lX esp %08lX eax %08lX ecx %08lX; stack code:%s",
                stalled, context.Eip, context.Esp, context.Eax, context.Ecx, code.c_str());
        // Text on the stack (the engine's fatal error handler formats its message there).
        if (samples == 1) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(stack);
            std::string run;
            int texts = 0;
            for (std::size_t i = 0; i < sizeof(stack) && texts < 6; ++i) {
                const unsigned char c = bytes[i];
                if (c >= 0x20 && c < 0x7F) {
                    run += static_cast<char>(c);
                    continue;
                }
                if (run.size() >= 16) {
                    RM_LOGW("hang watchdog: text on the stack: %s", run.c_str());
                    ++texts;
                }
                run.clear();
            }
        }
    }
}

// The engine's fatal error handler formats its message, shows it (FatalAppExit) and exits;
// the message goes to our log first, with the game code on the stack (who raised it).
using FatalFn = void(__cdecl*)(void* context, const char* format, ...);
FatalFn g_origFatal = nullptr;

void __cdecl FatalDetour(void* context, const char* format, ...) {
    char text[1024] = "(no message)";
    if (format) {
        va_list args;
        va_start(args, format);
        _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
        va_end(args);
    }
    RM_LOGE("game fatal error (the game shows it and exits): %s", text);
    const auto top = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
    std::string code;
    int found = 0;
    for (std::uintptr_t at = top; at < top + 0x4000 && found < 32; at += 4) {
        std::uint32_t value = 0;
        if (!SafeRead(at, value)) break;
        if (value >= 0x00401000 && value < 0x00F4A000) {
            char buf[12];
            std::snprintf(buf, sizeof(buf), " %08X", value);
            code += buf;
            ++found;
        }
    }
    RM_LOGE("game fatal error: stack code:%s", code.c_str());
    g_origFatal(context, "%s", text);
}

}  // namespace

void SetGuardedAccess(bool active) { t_guarded = active; }

bool InstallFatalErrorHook() {
    if (!engine::Caps().fatalHandler) return false;
    auto* target = reinterpret_cast<void*>(re5::addr::kFnGameFatalError);
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&FatalDetour), reinterpret_cast<void**>(&g_origFatal));
    if (status == MH_OK) status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        RM_LOGE("hook on the game's fatal error handler failed: %s", MH_StatusToString(status));
        return false;
    }
    RM_LOGI("hook on the game's fatal error handler queued");
    return true;
}

void StartHangWatchdog() {
    if (HANDLE thread = CreateThread(nullptr, 0, &HangWatchdog, nullptr, 0, nullptr)) {
        CloseHandle(thread);
        RM_LOGI("hang watchdog started");
    }
}

void InstallCrashDiagnostics() {
    AddVectoredExceptionHandler(1, &Vectored);
    g_previousFilter = SetUnhandledExceptionFilter(&Unhandled);
    RM_LOGI("crash diagnostics installed");
}

}  // namespace rmre5
