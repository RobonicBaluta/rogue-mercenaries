#pragma once

// Crash and exit diagnostics: logs fatal-class exceptions (with module and
// offset) before the game's own handlers run.

namespace rmre5 {

void InstallCrashDiagnostics();
// Marks the current thread as inside a guarded memory access so expected
// faults from SafeRead/SafeWrite are not reported.
void SetGuardedAccess(bool active);

}  // namespace rmre5
