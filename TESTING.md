# Testing Strategy

## Current test status

The portable core (`rm_core`) has an automated Catch2 suite (`rm_tests`, 63 test cases) and a headless simulator (`rm_sim`); both build and pass on x64 and Win32 (see Commands). These results say nothing about in-game behaviour. The target executable identity is measured locally and reference projects have been inspected. A game runtime test must be recorded separately from any build or simulation result.

## Core automated tests

Build a headless test runner with no RE5 dependency. Cover:

- Seeded RNG reproducibility and generator versioning.
- Reward candidates, rarity tables and route generation for fixed seeds.
- Perk eligibility, activation triggers, stacking, exclusions and character restrictions.
- Character and skill unlock conditions.
- Currency invariants, failed purchases, repeat transaction IDs and no negative balance.
- Fresh-run reset versus profile persistence.
- Save/load, schema migration, truncated/invalid JSON and recovery from a backup/empty profile.
- Encounter phase transitions, finite enemy budgets, arena completion and death/failure flow.
- Weapon modifiers and caps against extreme additive/multiplicative combinations.

Use a maintained C++ test framework if one is available; otherwise use a small self-contained test executable until dependency review. Test code can use Catch2 or GoogleTest after licensing, version, architecture and dependency acquisition are recorded.

## Headless simulation

Run at least 10,000 distinct seeds and report run count, invalid encounters, average supplies, credits/run, perk appearance rates, rarity distribution, max/percentile damage multipliers, zero/negative balance checks and progression IDs that are never reachable. Fix or explain any invalid state before calling the simulation acceptable.

## Adapter tests

Use a fake adapter to exercise event ordering and absent capabilities. Adapter tests must verify that unknown health/enemy counts remain unknown, repeated observations do not double-award rewards, and failed engine commands do not commit core state.

## Runtime validation

Keep game tests short and isolate one capability at a time:

1. Capture baseline hash/build and verify project proxy file is not already present.
2. Build x86 DLL and inspect PE architecture/exports.
3. Stage a diagnostic proxy only after checking name conflicts; keep a record of every created file.
4. Launch the game and verify an initialization log, menu, input and clean exit. If no log/menu, collect crash evidence and remove staged files.
5. Test a harmless overlay; then player/health; damage; outgoing damage; kills; counts; and round reset as separate cases.
6. Compare baseline and modified gameplay with the same stage/weapon/enemy setup. Keep the test executable identity attached to each log.
7. After each test remove only project-created files and launch without them once to verify rollback.

Do not infer a runtime result from a successful compiler exit, static signature match or simulator result. Screenshots/logs and reproducible in-game observations are required for runtime claims.

## Telemetry and diagnostics

JSONL should record UTC time, mod version, adapter version, executable build/hash, run ID, arena, event name and event-specific fields. Log initialization, hook verification, state transitions, damage, kills, perk/economy events, run results, exceptions and errors. Avoid per-frame logging. Diagnostic export should package current logs, sanitized configs, version/build summary and relevant crash information into ZIP; exclude saves unless explicitly needed, game files, usernames and unrelated personal data.

## Human verification package

When visual/gameplay evidence cannot be automated, prepare a build and give the user one focused test with exact installation, launch, reproduction, expected result, telemetry check, rollback and diagnostic-export commands. The first expected request should be a few-minute loader or combat test, not an open-ended play session.

## Commands

Run from the repository root in PowerShell. CMake/CTest are not on PATH; the scripts locate the
Visual Studio copy. For manual commands set
`$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`
and `$ctest` to `ctest.exe` in the same folder.

- Configure + build + test both architectures (x64 -> `build\x64`, Win32 -> `build\win32`; exits non-zero on any failure):
  `powershell -ExecutionPolicy Bypass -File scripts\build_core.ps1`
  Options: `-CoreOnly` (only `rm_core`, `rm_tests`, `rm_sim`; skips `adapters\re5`), `-Config Debug`, `-SkipX64`, `-SkipWin32`.
- Manual configure/build (one architecture):
  `& $cmake -S . -B build\x64 -G "Visual Studio 17 2022" -A x64` then `& $cmake --build build\x64 --config Release --parallel`
  (Win32: `-B build\win32 -A Win32`). CMake options: `RM_BUILD_TESTS` (ON), `RM_BUILD_SIM` (ON), `RM_WARNINGS_AS_ERRORS` (ON).
- Automated tests via CTest (14 entries: one per tag, an untagged safety net, the ZIP fixture and the Python `zipfile` reader):
  `& $ctest --test-dir build\x64 -C Release --output-on-failure` (same for `build\win32`).
  Last result (2026-10-27): `100% tests passed, 0 tests failed out of 14` on both x64 and Win32.
- Catch2 directly: `build\x64\Release\rm_tests.exe` (all; last result `All tests passed (120272 assertions in 63 test cases)` on x64 and Win32),
  or by tag, e.g. `build\win32\Release\rm_tests.exe "[persistence]"`. Tags: rng, content, perks, weapons, economy, shop, rewards, routes, run, persistence, telemetry, zip.
- Independent ZIP check (also run by CTest): `python -I tests\check_zip.py build\x64\test_output\stored.zip build\x64\test_output\diagnostics.zip`.
- Simulator (10,000 runs, deterministic; identical output from the x64 and Win32 builds):
  `build\x64\Release\rm_sim.exe --runs 10000 --seed 1 --config configs --out build/sim --date 2026-10-27 --log-runs 3`
  Writes `build\sim\summary.json`, `build\sim\summary.md` and (with `--log-runs N`) `build\sim\sim_events.jsonl` for the first N runs.
  Exit code 0 = all invariants pass, 1 = an invariant failed, 2 = usage/config error.
  Other options: `--capabilities all|none|<comma list>` (e.g. drop `heal` to check capability gating), `--policy mix|random|greedy|safe`.
  Publish: `Copy-Item build\sim\summary.md research\sim\summary-<date>.md` (current: `research/sim/summary-2026-10-27.md`).
- Content validation only: any `rm_tests "[content]"` run loads `configs/` and fails with the loader's error list if the data is invalid.
- Runtime launch/log collection: pending (RE5 adapter, Phase 1).
- Diagnostic export: core API `rm::ExportDiagnostics(outZip, logDir, configDir, extraFiles, summaryText)` is implemented and tested (`[zip]`); the in-game command that calls it is pending in the adapter.
