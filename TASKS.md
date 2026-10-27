# Development Tasks

Evidence is written on every completed item. `[~]` means partially verified (the gap is stated). `[blocked]` means a specific external prerequisite prevents progress; it is not used for untested work. Update this file when evidence changes.

## Phase 0 — Environment and documentation

- [x] Inspect the workspace and installed games/tools. Evidence: Windows 11 Pro x64; Steam manifest app 21690 is installed under `G:\SteamLibrary`; Build Tools 2022, x86 MSVC, Windows SDK, bundled CMake, Git, Python 3.11 and .NET 10 found. See `research/capabilities.md`.
- [x] Identify target executable architecture/version/hash/build ID. Evidence: PE32 machine `14C` (x86), file version `1.0.0.129`, SHA-256 `751726F0EC8BD01C00B037AF8AE64CDF0BC5CC5B9025F3F4D782EF6F66E04E26`, PE timestamp `0x63E9CE00`, Steam build `11465250`. The exe is packed (SteamStub + compressed sections); analysis uses a runtime dump (`scripts/dump_image.py`).
- [x] Clone and inspect reference projects. Evidence: RE5Patch `a82342321202a117fdaec9826b1d28d4b0babb36`; RE5 randomizer `81a256e229567d9871b4d006200b5d92faf83120`; RE5 trainer `c7f02fdf88da3c386d4054610dd664826a8e4880` (addresses do not match this build); RE5VR `0c621b52a8b77e4ef6a272e837b65c8dae6a9192` (same build, no license file: used as research only, no code copied). Notes in `RESEARCH.md`.
- [x] Write development plan, architecture, research, testing and capability baseline. Evidence: `PLAN.md`, `ARCHITECTURE.md`, `RESEARCH.md`, `TESTING.md`, `research/capabilities.md`, `research/re5-engine-map.md`.
- [x] Initialize project Git repository and ignore generated/reference artifacts. Evidence: first commit 2026-10-27; `.gitignore` excludes `build/`, reference clones, downloaded tools and scratch data.
- [~] Evaluate and validate ARCtool against a copied target-game ARC. Partial: copied `s100.arc` (610 files) extracted, repacked (96 XFS rebuilt from XML), re-extracted and one TEX converted to DDS and back (`research/scratch/arc_validation/*.log`). LOT/GMD conversion not exercised. Deferred: v0.1 uses runtime Mercenaries matches, not ARC edits.
- [x] Establish project build and compile a diagnostic x86 proxy. Evidence: `scripts/build_core.ps1` builds `build/win32/adapters/re5/Release/dinput8.dll` (PE32, exports DirectInput8Create/DllCanUnloadNow/DllGetClassObject/DllRegisterServer/DllUnregisterServer, static CRT, imports only system DLLs).
- [x] Verify clean game launch and log output with diagnostic proxy; remove staged files after run. Evidence: `scripts/smoke.ps1` (install via manifest → Steam launch → log/frame capture → close → uninstall → exe hash check); 20+ runs on 2026-10-27 under `build/runs/`.

## Phase 1 — RE5 feasibility

See `research/capabilities.md` for the per-capability ledger and `research/re5-engine-map.md` for addresses and derivations.

- [x] Verify DLL proxy load and export forwarding inside the installed build. Evidence: adapter log shows init, 12/12 signatures OK, DirectInput8Create forwarded (game input works through the proxy), clean `process detach` on exit; 5/5 consecutive stable launches after batching hook enable (`build/runs/*-batch1..5`).
- [x] Verify safe game-loop/render callback and test overlay. Evidence: Present runs on the game main thread (~120 Hz); ImGui HUD and run menu rendered in game (`build/runs/20261027-190135-uidemo/frame-00*.png`, in-process back-buffer captures).
- [~] Identify local player and bounded health read. Static: `sPlayer` list/count, HP `+0x1364`/`+0x1366`, DTI type checks; runtime read returns count 0 at title. Needs an in-match observation (user test T1).
- [~] Detect received damage and distinguish healing/scripted changes. Static + hook installed on `uPlayer::SubHp` (heals are negative amounts, ignored). Needs an in-match hit (user test T1).
- [~] Verify one reversible outgoing weapon-damage modification. Static + hooks installed (OnHit scope + base SubHp scaling). Needs an in-match comparison (user test T1).
- [~] Detect enemy death and duplicate/non-player kill behavior. Static + OnHit hook; core ignores repeated enemy ids. Needs in-match kills (user test T1).
- [~] Obtain living enemy count in a named encounter. Static `sEnemy` list; not yet observed in a match.
- [ ] Verify finite encounter completion and a second arena round/reset. Design: Mercenaries match = arena, mod ends the match by zeroing the countdown (`+0x680`), Retry starts the next arena. Needs user test T2.
- [~] Validate clean mission exit and diagnostic log export. Clean exit verified at the title screen; in-game diagnostics button implemented, not yet pressed in game.
- [ ] Complete feasibility recommendation; compare RE4 Remake only if the RE5 gate has failed with evidence. Not triggered: every RE5 capability has a verified-static path and the runtime pieces tested so far work.

## Phase 2 — Portable roguelite core

- [x] Add a game-independent C++ core and adapter interface. Evidence: `rm_core` static library (`core/`, C++20, `/W4 /permissive- /WX`) builds warning-free for x64 and Win32 via `scripts/build_core.ps1`; no game headers, addresses or `windows.h` in `core/include/rm/`. Adapter contract: `RunController` facade + `Capabilities` + `IEventSink` in `core/include/rm/run_controller.h`; `adapters/re5` links `rm_core` in the Win32 tree.
- [x] Implement deterministic seeded RNG and stable run identifiers. Evidence: xoshiro256** seeded via splitmix64, `kRngVersion` 1, independent named streams (reward/route/loot/shop/combat); golden values from an independent Python implementation pass on x86 and x64 (`tests/test_rng.cpp`); run ids `run_NNNN` from the profile counter (`tests/test_run_flow.cpp`).
- [x] Add data-defined Chris and Sheva starting definitions (content enabled only if engine character selection is verified). Evidence: `configs/characters.json` (Chris/Sheva free, Jill/Wesker `enabled: false` with credit costs); disabled/locked characters are rejected by `StartRun` (`[economy]` tests). Engine-side character selection is still unverified (Phase 1/3); the adapter must not offer a character it cannot start.
- [x] Implement perk definitions, stacking, exclusions and triggers. Evidence: 22 perks in `configs/perks.json` (all non-co-op categories, 5 rarities, exclusive pair, Chris/Sheva-only perks, 2 curses, disabled co-op perk); 7 effect kinds, 5 conditions, per-stack scaling, capability gating; `tests/test_perks.cpp`.
- [x] Implement weapon-instance modifiers and supported stat application. Evidence: 10 base weapons, 5 affix kinds and 2 legendary uniques in `configs/weapons.json`; weapon-id vs single-sidearm rule documented in `core/include/rm/weapons.h`; `tests/test_weapons.cpp`. In-game application depends on the adapter's verified damage hook.
- [x] Implement transactional credits/supplies and validated shop operations. Evidence: idempotent `rm::Ledger` (no negatives/overdrafts), all-or-nothing `Purchase` with debit rollback on failed grant (`[economy]`, `[shop]` tests); 0 negative balances in 10,000 simulated runs.
- [x] Implement versioned atomic save and corrupt-save recovery. Evidence: `rm::ProfileStore` (tmp + flush + `.bak` + rename, checksum, `.corrupt-<n>` preservation, v0 -> v1 migration, crash checkpoint) in `core/persistence/profile.cpp`; `tests/test_persistence.cpp`.
- [x] Implement finite encounter state, performance metrics, rewards and route choices. Evidence: `RunController` phase machine, enemy budgets, S/A/B/C ratings, 3-choice reward offers, shop, 2-3 route choices (safe + harder) with curses; `tests/test_run_flow.cpp`, `tests/test_rewards.cpp`.
- [x] Implement JSONL runtime logging and diagnostic ZIP packaging. Evidence: `rm::JsonlSink` (level filter, rotation, flush policy, injectable clock), `rm::WriteStoredZip`, `rm::ExportDiagnostics`; archives verified by an independent reader and Python `zipfile` (`rm_zip_python_reader` CTest).
- [x] Add automated tests for RNG, rewards, perks, unlocks, economy, resets and persistence. Evidence (2026-10-27): `rm_tests` 63 test cases / 120,272 assertions pass on x64 and Win32; CTest 14/14 on both (`TESTING.md` Commands).
- [x] Add headless run simulator; run at least 10,000 seeds and publish balance summary. Evidence: `rm_sim --runs 10000 --seed 1` -> `research/sim/summary-2026-10-27.md` (all invariants PASS: 0 invalid encounters, 0 negative balances, 0 duplicate reward grants of 31,992 attempts, no unreachable enabled content); x64 and Win32 builds produce identical `summary.json`.

## Phase 3 — Three-round vertical slice

- [x] Define the chosen existing arena and verified encounter entry procedure. Design: The Mercenaries (solo, no AI partner); each match is one arena, detected by local player present + countdown running. Entry procedure documented in `README.md`/`TESTING.md`. Runtime confirmation pending (T2).
- [~] Start a run with reset temporary state and selected supported character. Implemented in the overlay (Chris/Sheva selectable; in-game class mapped from DTI `uPl00Chris`/`uPl01Sheba`); UI demo run started in game (`*-uidemo`). Character match check needs an in-match test.
- [~] Track kills, received damage, time and finite remaining enemies. Hooks + core wired (`adapters/re5/src/mod.cpp`); HUD shows remaining enemies (UI demo). Real kills need T1.
- [~] Offer and apply three reward choices using at least four tested perks across content. 20 of 22 perks are offerable with the adapter's capabilities; reward selection rendered and applied in game via UI demo. Gameplay effect needs T1.
- [x] Award/spend supplies without negative balances or duplicate transactions. Evidence: core ledger tests + 10k-run sim; shop rendered in game (UI demo).
- [x] Show arena results and deterministic route choices. Evidence: UI demo frames (results card, route selection) from the real core inside the game.
- [ ] Complete three successive rounds using a verified game transition. Needs T2.
- [~] Persist end-of-run credits and show final result. Core-tested (`[persistence]`, `[run]`); in-game EndRun path implemented (auto-bank on RunComplete/RunFailed).
- [x] Verify the next run clears temporary progression. Evidence: core tests (`StartRun resets temporary state while profile persists`).
- [~] Produce manual installation, playthrough and diagnostic-export instructions. Draft in `README.md` (install/uninstall/test T1–T2).

## Phase 4 — Expansion

- [ ] Expand to ten perks and rarity tiers.
- [ ] Add weapon upgrades, shop offers and elite modifiers.
- [ ] Add harder routes and character/skill unlock progression.
- [ ] Add additional arenas only after repeatability and softlock checks.

## Phase 5 — Stabilization/release

- [ ] Repeat three-round run and inspect crash logs.
- [ ] Test save corruption and duplicate reward recovery.
- [ ] Profile adapter and logging overhead.
- [ ] Perform clean install/uninstall and Steam file verification.
- [ ] Produce release ZIP, hash manifest and rollback guide.
- [ ] Document unsupported engine features and tested game build.
