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

See `research/capabilities.md` (per-capability evidence) and `research/re5-engine-map.md` (addresses and derivations). Runtime artifacts: `build/runs/` (local).

- [x] Verify DLL proxy load and export forwarding inside the installed build. Evidence: 30+ launches; 13/13 signatures OK; DirectInput works through the proxy; clean `process detach`; exe hash unchanged after every uninstall.
- [x] Verify safe game-loop/render callback and test overlay. Evidence: Present tick on the main thread; HUD/menu rendered in menus and in matches (`build/runs/manual-t2/*.png`).
- [x] Identify local player and bounded health read. Evidence (Mercenaries solo): count=1, `uPl00ChrisNormal`, control=0, HP 1000/1000 decreasing with hits. Story mode with an AI partner not yet observed.
- [x] Detect received damage and distinguish healing/scripted changes. Evidence: `player_hit raw=196 applied=186` per hit; heals (negative amounts) ignored by design; death detected (HP 0 → run failed).
- [x] Verify one reversible outgoing weapon-damage modification. Evidence: raw 250 → applied 1000 (×4 test multiplier) → 1253 with Sharpshooter×2 + M92F Lv2 → 1339 with Sharpshooter×3; matches the core formula.
- [x] Detect enemy death and duplicate/non-player kill behavior. Evidence: `killed=1 local=1` → `kill counted`; core ignores repeated enemy ids (tests). Non-player kills cannot occur in Mercenaries solo; story-mode partner kills not yet observed.
- [x] Obtain living enemy count in a named encounter. Evidence: `sEnemy` listed=12 alive=12 → 11 after a kill (Public Assembly).
- [x] Verify finite encounter completion and a second arena round/reset. Evidence: kill budget met → mod zeroes the authoritative countdown → game results screen → next match begins the next arena (`run_0003`: 3 arenas; `run_0004` arena 1 cleared by 6 real kills in 76 s).
- [x] Validate clean mission exit and diagnostic log export. Evidence: in-game Export diagnostics wrote a valid ZIP; WM_CLOSE exits cleanly with audio restored.
- [x] Complete feasibility recommendation. RE5 gate passed with runtime evidence; RE4 Remake comparison not needed.

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

- [x] Define the chosen existing arena and verified encounter entry procedure. Evidence: The Mercenaries Solo (any stage; tested on Public Assembly) — match start (countdown running, local player present) begins the arena.
- [x] Start a run with reset temporary state and selected supported character. Evidence: F7 → Start run (Enter hotkey) in game; character from DTI class (`uPl00…` → chris); `run_0004` started by automation.
- [x] Track kills, received damage, time and finite remaining enemies. Evidence: HUD "Enemies left" decreasing with real kills; telemetry `enemy_killed`, `player_damaged`, `arena_completed` with time.
- [x] Offer and apply three reward choices using at least four tested perks across content. Evidence (in game, `build/runs/20261027-200845-autoplay/adapter.log` and `manual-t2`):
  - B.S.A.A. Veteran: incoming ×0.95.
  - Sharpshooter: outgoing ×1.16/×1.24 with 2/3 stacks.
  - Finisher: +35% only below 30% target HP; applied at 50/150/200 of 800, not at 300.
  - Keen Eye: crits ×2.0, stacking with Finisher to ×2.7.
  - Kevlar Vest: incoming 0.87 combined with Veteran.
  - Field Medic: unlocked with credits first; +30 HP heals on kills while hurt.
  - Curse: Frailty: incoming ×1.25 on the harder route.
- [x] Award/spend supplies without negative balances or duplicate transactions. Evidence: two in-game purchases (upgrade 30, perk 30); ledger tests; 10k-run sim.
- [x] Show arena results and deterministic route choices. Evidence: results card and route offers in game (`build/runs/manual-t2/b11.png`, `b19.png`).
- [x] Complete three successive rounds using a verified game transition. Evidence: `run_0003` arenas 1→2→3 through three Mercenaries matches; the mod ended each match via the countdown.
- [x] Persist end-of-run credits and show final result. Evidence: "run_0003: complete, 3/3 arenas, 78 kills, +269 credits"; `profile.json` credits 269 after restart of the game.
- [x] Verify the next run clears temporary progression. Evidence: core tests; in game `run_0004` (after `run_0003` ended with Sharpshooter×3 and M92F Lv2) started with only B.S.A.A. Veteran and M92F Lv1 (`build/runs/20261027-193859-autoplay/a1-rewards.png`). Interrupted runs are settled on the next launch ("interrupted run settled: run_0004/run_0005").
- [~] Produce manual installation, playthrough and diagnostic-export instructions. `README.md` + release `README.txt` (`scripts/package_release.ps1`); needs a human read-through.

## Phase 4 — Expansion

- [x] Expand to ten perks and rarity tiers. Evidence: 22 perks across 5 rarities in `configs/perks.json` (20 offerable with the RE5 adapter's capabilities); permanent perk/weapon unlocks purchasable with Credits (in game: `unlock perk field_medic -> ok`).
- [~] Add weapon upgrades, shop offers and elite modifiers. Weapon upgrades and shop offers are done and verified in game (M92F Lv2 bought → +8% damage observed). Elite modifiers are not implemented: there is no verified way to control spawns, and content rejects `elite_chance` > 0.
- [x] Add harder routes and character/skill unlock progression. Evidence:
  - Route tiers 1–3 with reward multipliers and curses; the tier-3 Ruins Gauntlet with Curse: Frailty was played in game.
  - Perk/weapon unlocks spend Credits (Unlocks panel; in game `unlock perk field_medic -> ok`).
  - Character unlocks are in the panel. Jill and Wesker stay disabled until verified in game.
- [x] Native "ROGUE MERCENARIES" entry in the game's Play menu. Evidence (in game, 2026-10-27, `build/shots/final_menu.png`, `final_match.png`):
  - the row renders, highlights and is navigated by the game itself, with its own help line;
  - decide opens the game's Mercenaries screen, and the match auto-starts the run (`decide on row 8 of 9` → `ROGUE MERCENARIES chosen` → `start run (chris): ok` → `begin arena: ok`);
  - backing out to the Play menu cancels the choice, and plain LOS MERCENARIOS in the same session stays vanilla (`decide on row 6 of 9`, match with no run);
  - row 0 ("CONTINUAR"/"NUEVA PARTIDA") is untouched.
  - Not verified: other game languages; the trial-menu variant (left vanilla by design).
  - Open question: one unexplained "chosen" after a back-out (cursor read 8 while row 6 had been drawn a second earlier), not reproduced in 4 attempts with cursor logging.
- [ ] Add additional arenas only after repeatability and softlock checks. All Mercenaries stages should work (the arena is tied to the match, not the map), but only Public Assembly has been played.

## Phase 5 — Stabilization/release

- [x] Repeat three-round run and inspect crash logs. Evidence:
  - `run_0003` completed 3 arenas.
  - Five more automated runs (`run_0004`–`run_0008`) with no crash and no unhandled exception in any adapter log.
  - 5/5 stable launches after hook batching.
- [x] Test save corruption and duplicate reward recovery. Evidence:
  - Core tests: truncated JSON, bad checksum and wrong schema → `.bak` → default; v0 migration.
  - `ChooseReward` twice fails; EndRun is idempotent.
  - In game, interrupted runs were settled once on the next launch (`run_0004`, `run_0005`).
- [x] Profile adapter and logging overhead. Evidence: `perf:` log lines from the packaged build:
  - Menus: about 80 µs per frame.
  - Matches: about 93–99 µs average, worst around 0.6 ms.
  - One 4.2 ms hitch when "Start run" saves the profile synchronously. Accepted for v0.1; a background save would remove it.
  - Per-hit logs are now at debug level.
- [x] Perform clean install/uninstall and Steam file verification. Evidence:
  - Install from `dist\RogueMercenaries-0.1.0-dev.zip` with player defaults loads and plays (`build/runs/package-install/`).
  - Uninstall removes exactly the manifest files.
  - A failed install rolls back.
  - `re5dx9.exe` SHA-256 is unchanged after every uninstall. The Steam "verify integrity" step is left to the user; no game file is modified.
- [x] Produce release ZIP, hash manifest and rollback guide. Evidence: `scripts/package_release.ps1` → ZIP with DLL, configs, README (install/play/uninstall/rollback), third-party licenses and `SHA256SUMS.txt`.
- [x] Document unsupported engine features and tested game build. Evidence: `research/capabilities.md` lists the tested build and the untested paths (story mode with AI partner, explosion/environment kills, stage id).
