#pragma once

// Crash and exit diagnostics: logs fatal-class exceptions (with module and
// offset) before the game's own handlers run.

namespace rmre5 {

void InstallCrashDiagnostics();
// Developer: logs where the game's main thread is when it stops presenting frames.
void StartHangWatchdog();
// Logs the message of the engine's fatal error handler (FUN_006CFEF0) before the game shows it
// and exits. Queued with the other hooks.
bool InstallFatalErrorHook();
// Marks the current thread as inside a guarded memory access so expected
// faults from SafeRead/SafeWrite are not reported.
void SetGuardedAccess(bool active);

}  // namespace rmre5
