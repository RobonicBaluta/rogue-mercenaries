# Testing Strategy

## Current test status

The portable core (`rm_core`) has an automated Catch2 suite (`rm_tests`, 63 test cases) and a headless simulator (`rm_sim`); both build and pass on x64 and Win32 (see Commands). These results say nothing about in-game behaviour on their own. In-game behaviour is verified separately with adapter logs, JSONL telemetry and in-process frame captures from the installed game (`research/capabilities.md` lists the evidence per capability; runs are stored under `build/runs/`).

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

### M8 playtest log lines

From M8's second batch the user plays and the fixes come from `%LOCALAPPDATA%\RogueMercenaries\logs\adapter.log`
(and the JSONL telemetry next to it). What each build writes there:

| Build | Lines |
|---|---|
| any special | `special: N controller buttons named by the game's config.ini` at start; `special: <kind> used at x y z facing fx fz`; `special: <kind> over (done / arena over): N pulses, N hits` |
| Mine Layer | `world: picked up XXXX, given as 010A (the build's drops)`; `build: item 010A stacks to 10` |
| Heavy Gunner | `build: item 02xx stacks to N (was M)` for the ammo |
| Field Surgeon | `build heal: A -> B` and `(big heal: buffs)` |
| Ambition, Iron Fist | `kill counted: ... (melee)`; `build: melee kill, <stat> +0.0N (N gains)`; with a debug log, `outgoing ... (<weapon>, melee)` |
| keys | `keys: a key dropped, N held`; `keys: chest locked (no key), it comes back`; `keys: locked chest back at x y z` |
| Locksmith | `chests: 6 of 6 placed for arena N` (three more than the others' `3 of 3`) |
| ORGANIZE result | after the screen closes: `organize: XXXX cell N xA -> xB` (a stack as the screen left it) and `organize: XXXX xN left cell N (not there in the layout)` (moved to the storage); the next `itembox: the run is in the screen (... grid ...)` must match the last `the run's grid from the screen` |
| enemies moved | `director: <class> stuck N s ... moved to ...` only out of the player's sight; one in sight: `director: <class> still N s at x y z, N m from the player, in his sight; left there` |
| the Uroboros | `boss drive: uEm40 actions walk N, near grab N (of N), far grab N`; `boss drive: uEm40 had no target; the player is its target now`; `boss drive: uEm40 near grab (action N) at N m` / `far grab`; `boss drive: uEm40 cEm40ActWalk -> cEm40ActNearCatch, N m from the player, N degrees off` (its actions besides walking and standing); the 5 s `director: boss uEm40 at ...` lines should show the distance falling |
| Twin Fury hits | `special: dual_smg over (done): N pulses, N hits, N staggered, N twin gun shots` (`over (the sweep is over)` when the borrowed sweep played) |
| Twin Fury's two SMGs (Jill Battle Suit) | `twin guns: XXXXXXXX YYYYYYYY in her hands (Twin Fury)` once the hold runs; `twin guns: put away (the sweep is over)` at the end; none when `uWp76` is missing (the capability `twinGuns`). Developer mode, the first shots: `twin guns: gun N shot from X Y Z (her feet at X Y Z)`, about 1 to 1.5 m above her feet and a hand's reach to her side (her bones 11 and 6, capability `jointPosition`) |
| A run goes on after a quit or a crash | at each stage change `resume point: ok`; a match left: `run: arena N's match was left without the mod (...); the run is suspended (CONTINUE RUN)`; at launch `interrupted run run_NNNN kept: CONTINUE RUN goes on with it`; CONTINUE RUN: `resume: N carried records, N in the storage, health N`, `resume: run run_NNNN goes on at arena N on stage N`, then `stage change: arena N started on stage N` and the carried items (`scripts/test_resume.sh`) |
| The ladder (run lengths) | at a run's start `ladder: short run, 4 arenas; bosses beaten: none` (later `boss:uEm40, short:easy`, ...), then one `run map: arena N:` line per arena, the last one the boss alone (`103 ...`) until its length's easy boss is beaten, then `103 ... | 304 ...`; the 8-arena run from its fifth arena: the arena line's budget 3 more each arena; in the JSONL telemetry a boss win adds `"source":"ladder","trigger":"boss_beaten","key":"short:easy"` |
| A boss arena (Jill, Popokarimu, Wesker) | `director: boss uEm84 (Jill) spawned, health 6000` then `director: boss health 2000/2000 -> 6000`; every change of the boss's and the player's action: `watch: uEm84 hp N/N action N cEm84Act...` and `watch: player action N cPlAction_...` (a black screen or a stuck move: the last lines before it); its death: `enemy ... died (...): counted`, then `boss fall: uEm81 cEm80ActShrink (action 104) ...` (Wesker, or one killed without a hit) or `boss fall: uEm84 fell by itself (cEm84ActDieNormal)`, and `phase arena_active -> arena_results` 3 s later |
| What does not fit goes to the storage | `shop: item 0108 x2 -> 0 stacked, 0 new slot(s), 2 did not fit` then `inventory: 0108 x2 did not fit; kept in the run's storage (ORGANIZE)` (a gun: `shop: weapon NNNN created with N rounds -> no slot` then the same with its rounds); no refund; the shop's header ends `FLASH GRENADE IN STORAGE`; the next ORGANIZE shows it in the storage (`itembox: stash (N): 0108 x2 ...`) |
| No save from a run | at start `autopilot: save file write hook queued (no save while a run is on)`; in a run's match `save: the game's save file write held while a run is on`, 30 s after it `... allowed again`; a write the game tried meanwhile `save: the game's save write skipped (a run is on); savedata.bin stays as it was`. The file's hash must not change |
| A run's match quit or restarted from the pause menu | `run: arena N's match was left without the mod (the pause menu's quit?); the run is abandoned` (or `started over ... retry?`), then `run ended (abandoned)` and the run's results at the Play menu; the next START RUN starts a new run |
| Dead ends learned (any map, the Port above all) | `director: uEmNN stuck at X Y Z; no enemy goes within 4 m of it this match (N such spots)` before its move; fewer `stuck ... moved to` lines from the same spots, and the moves land near the player's height |
| An enemy that cannot be moved | `director: uEmNN is back at X Y Z after its move (N)` when a move did not take; with three or fewer left, after two: `director: uEmNN cannot be moved (back at X Y Z after 2 moves), N m from the player, N left; finished so the arena can end`, then its death line |
| Run start, the character's guns | `inventory: stripped to the pistol PPPP (in hand HHHH, N record(s) removed)`; with a knife in hand `(in hand 0123, no gun, ...)` and the pistol kept or `, pistol created` (S.T.A.R.S. Wesker keeps his Samurai Edge `0129`) |
| Wesker's moves | `special: boss motion pawn\em\em80\motion\em80 20` (the dash's arrival), `... 24` (the lunge's knee), `... C9` (the burst's kick), each `N of N body tracks ...` then `boss motion NN done: N frames in N s` |
| last arena | `phase arena_results -> run_complete` (no `reward_selection` after the boss) |
| bosses | the Uroboros: `boss drive: uEm40 actions walk N, near grab N (of N), far grab N (lookup N, N actions)` with ids found (not -1), then `near grab (action N)` lines; U-8 (uEm43): no `director: uEm43 stuck ... moved to` lines (only its own `cEm43ActShift`) |
| Heavy Metal | every shop lists the Gatling gun (`0115`) at 100 supplies until it is bought |
| quitting a run | from the pause menu: `autopilot: the match's quit state begins (run match 1, ..., Mercenaries phase N)` then `a run's match quits; Mercenaries phase N cleared`; `savedata.bin` must not change (a quit on 2026-11-04 wrote it) |
| Iron Grip (Chris) | `iron grip: cEm10ActNeckHang_PL by uEm10 thrown off (cEm10ActDmgFlyoff)`; another grabber: `... kept its grab (no known reaction)` or `no grabber found` |
| Scavenger, Chemist | at the arena's start `world: drops: the game's own, ammo and recovery items as ammo for the gun in hand` / `, swaps 0301->0309 0305->0309`; on a pickup `world: picked up 0305, given as 0203 (ammo for the gun in hand)` / `given as 0309 (a perk's swap)` |
| stack perks | `build: item 0204 stacks to N (was M)` (Sharpshooter rifle, Weapon Expert handgun, Point Man machine gun, Magnum Specialist magnum rounds) |
| new weapons | from a reward or the shop `shop: weapon 011E created ...` (M93R), `0122` stun rod, `0125` grenade launcher, `010D` rocket launcher, `0121` flamethrower; in ORGANIZE `itembox pictures: 0121 gets item_135` (the flamethrower takes the Gatling's picture); a missing model would end the game with `Failed open file ... wpNN00.mod` |
| Twin Fury, Backup | `enemy XXXXXXXX died (health reached zero): counted, N remain (by the special)` |
| health | `build: maximum health A -> B (xM)` |
| specials refused | `special: slot N pressed while <action or paused / the item screen / a mod screen / down>; not used` |
| enemy stagger states | `enemy states: uEm10 cEm10ActDmgHead after a hit (a lunge's stagger)` (once per class and state) |
| item screen | `hud: item screen flag 0 -> 1` when the in-match inventory opens (if the hypothesis holds) |
| carried items | `carried: item XXXX was used up, removed` before `carried: item XXXX xN -> ...`; `inventory: XXXX xN did not fit after the stage change; kept in the run's storage (ORGANIZE)` |
| Organize pictures | `itembox pictures: 011F gets item_13e and its model`; `... the run's guns in the screen's item table: ok`; `... the screen's item table restored: ok` after it closes |
| Wesker's turn | `special: turn to A done; facing B now (on target)`; `special: lunge reached uEm10 (cEm10ActDmg...), facing it: the game's melee finishes it` |
| computer partner | `partner: slot 2 uPl04JoshCos1 control 1, N m from the player, action ..., hp ..., items: 0101 x1 ...` (any second player, every 5 s) |
| Josh's backup (E22) | `extras queued: ... uPl01ShebaNormal` on every stage of a Josh run; `partner: uPl01ShebaNormal (character 1, costume 0) added in slot 1 ... (backup)`; `special: backup used: uPl01ShebaNormal joins for the arena`; `partner: the save's player entries put back: ok`; `partner: N m away ...; moved beside the player`; every 3 s `partner: N m from the player (player at ..., hp N, action), her action, hp`; hits `enemy_hit ... player=1 local=0 ... by=uPl01ShebaNormal`; at the stage change `partner: match over; game work's slot 1 put back`; when she cannot be added: `partner: not added (backup): ...` and the covering fire |
| code fixes | `code fix: a Majini's bomb death takes Reunion's effect (Excella's finisher): applied` at start |
| missing model | `game fatal error (the game shows it and exits): Failed open file. ...\pawn\wp\wpNN\model\wpNN00.mod`: an archive no list loaded; add its item to the run's preloads |
| specials input | `special: lunge ... [XInput pad]` / `[keyboard]` / `[DirectInput pad]`: the device that triggered it |
| Wesker's dodge | `special: dodge running as cPlAction_pl_act_avoid_front`; `special: the dodge never started (...)`; `special: dodge still running after 1.5 s (...); back to relax` |
| burst | `special: burst: reacts killed ... [device]` |
| stuck enemies | `director: uEm10 stuck 12 s at x y z, N m from the player; moved to x y z (copies)`; `one of the last, quiet` |
| Organize guard | `itembox guard: on (no Buy, no Upgrade)`; `itembox guard: Buy refused` / `Upgrade refused` |
| shop | `shop: new stock (N supplies)` |
| Wesker's moves | `special: dash X of 5.0 m (clear (flags F) / wall at Y m (flags F)[, floor at N percent])[, hits: killed reacts ...]`; `special: lunge X m (uEm10 at N m, ...)` then `special: lunge strikes uEm10: reacts`; `special: burst (N enemies on him): reacts ...`; `special: yaw ... does not match his facing` if the facing write is skipped |
| quitting a run | `autopilot: a run's match quits; Mercenaries phase N cleared (no save screen, no save)` |
| enemy threat | `arena N: <MAP>   <ROUTE> (stage S, enemy threat L)`; the director's spawn lines show each class and weapon (`arm 70150`: the weapon class uWp50, graded in `economy.json` `enemy_threat` `weapons`) |
| kills not by the player | `enemy_hit ... killed=1 by=<class>` |
| borrowed boss moves | `resources: list 4 (enemies) extras queued: ... uEm84` (Jill) or `uEm80` (Wesker); `special: boss motion pawn\em\em84\motion\em84atk 0A: N of M body tracks and N of M in all reach uPl02JillCos2's joints, 91 frames (slot 15)`; while it plays the player's action is `cPlAction_pl_act_test`; `special: boss motion 0A done: 91 frames in X s at speed 2.0`; `special: boss move dual_smg over (done) after X s, step 2 of 2, N m from where it began; back to relax`; `special: the hold ran after N requests, X s` when it took more than one; a hit with no reaction during it: `special: a no-reaction hit (cPlAction_pl_act_setEventAct_wait) during dual_smg step N; the hold is asked for again`, then `special: the hold is back after a no-reaction hit, X s`; when not: `special: no boss move for <move> (<why>)`, `special: boss move <move> over (the hold never started: <action>, N requests)` |
| elements (rarity remake) | `elements: uEm10 reactions: DmgFire=134 DmgIce=135 DmgSulfate=136 DmgFlash=137 DmgFlyoff=115 DmgYoroke=124 DeadHeadExplosion=149` (a class's reactions, once); `elements: burn (Incendiary) on uEm10 <unit>: 0 damage, 50 per second for 3.0 s, 0 range, 1 reached, its own reaction` (the first 12 of each kind per match: burn, poison, freeze, shock, arc, ignite_around, execute, explode, shatter, overkill, refill); the kills they cause: `enemy <unit> died (health reached zero): counted, N remain (by the special)`; with `probe_hits` the first 40 hits: `hit: m92f, magazine N left, x1.00, N procs[, frozen]` |
| refines | `chest N: CHEST   ...   M92F REFINED TO EPIC`; telemetry `weapon_refined` (source reward, shop, chest, dev) |
| mod skins | `mods: <game>\RogueMercenaries\mods: sheva_hound_wolf missing uPl01ShebaCos4.arc; excella_experimenter installed;` (at start and on SKINS); `mods: excella_experimenter wears its mod (1 files read from the mods folder)`; `files: ...\uPl05ExcellaNormal.arc read from ...\mods\...`; `mods: opening https://www.nexusmods.com/... (ok)`; with dev `openlog on`, `files: the game opens ...` |
| boss arena | `director: boss uEm40 at x y z, N m from the player (N m above), cEm40Act...` every 5 s; `director: uEm40 stuck 8 s at ...; moved to ...` when it stops closing in |
| specials while dying | `special: slot N pressed while dying; not used` |
| start move | `match start: the player is not placed yet (0 0 0); moving to the map's start later`; `warp: N places hold the unit's position ..., more than a unit has; not moved` |
| run-planned guests (M3.3) | `run map: arena N: 204 dockside_push +uEm14 +uEm32 -> 0 1` (each node's guests); at a stage change `resources: stage S brings the run's guests: uEm14 uEm32` and `resources: list 4 (enemies) extras queued: ... uEm14 uEm32 uEm0d ...`; `arena N: <MAP>   <ROUTE> (stage S, enemy threat L, guests uEm14 from 4, uEm32 from 4)`; `director: uEm14 (arm 7016D, ...)` spawn lines for them; a guest whose archive did not load: `director: uEm14 is not loaded on this stage; not spawned, dropped for this arena`; telemetry `arena_started` `guests` |

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
- Content validation only: any `rm_tests "[content]"` run loads `configs/` and fails with the loader's error list if the data is invalid
  (it also checks that every perk the menus list names an icon whose `configs/icons/<icon>.png` exists).
- Menu icons: `python scripts/make_icons.py [--sheet]` redraws `configs/icons/*.png` (`--sheet`: `build/icons_sheet.png` for review).
- Runtime (RE5, game must be closed first; all scripts restore what they change):
  - Switch the game to 1280x720 windowed + VSync for tests / restore the player's config:
    `scripts\game_config.ps1 -Mode test` / `-Mode restore` (original kept in `build\backup\config.ini.original`).
  - Install / uninstall with a manifest (never overwrites foreign files, checks the exe hash):
    `scripts\deploy.ps1 -Mode install -AdapterConfig configs\adapter.test.json [-ConfigDir <content>] [-FromZip dist\RogueMercenaries-<version>.zip]`, `-Mode uninstall`, `-Mode status`.
  - Unattended smoke run (Steam launch, muted, background; logs + in-process frame captures to `build\runs\<stamp>-<label>\`):
    `scripts\smoke.ps1 -Dll build\win32\adapters\re5\Release\dinput8.dll -AdapterConfig configs\adapter.dev.json -Seconds 30 -CaptureAt "10,20"`.
    With `dev.ui_demo: true` the mod walks a scripted run (sandbox profile) through every panel for UI captures.
  - Interactive control (needs an otherwise idle PC: sends input to the game): `scripts\gamectl.ps1 -Action launch|keys|capture|close|reap|status`;
    `-Keys` accepts key names, `down=`/`up=` holds, mouse buttons and `move=dx/dy`.
  - Full automated run with real kills (Mercenaries Solo, Public Assembly, Chris; small-budget test content):
    `bash scripts/autoplay_run.sh build/testcontent configs/adapter.autoplay.json 3 [rounds]`
    (`adapter.autoplay.json`: invulnerable, x4 outgoing damage; never ship it). Optional `PRE_COMMAND="unlock perk <id>"`.
    Safety gate: the script aborts without further input if the game does not enter The Mercenaries (mode 0 -> 2).
  - Director and HUD checks from the Play menu (game started with `bash build/restart_to_play_menu.sh configs/adapter.director.json`;
    each saves and restores the profile and closes the game): `STAGE=<n> bash scripts/test_director.sh` (enemies come, arena clears at
    its budget), `STAGE=<n> bash scripts/test_hud.sh` (no clock or score in arena 1, the screens between and arena 2; the kill cap
    no longer ends the match; captures in `build/runs/<stamp>-hud/`), `bash scripts/test_preload.sh` and `bash scripts/test_weapon.sh`
    (a foreign enemy and a gun outside the loadout, preloaded, spawned, created, equipped and fired), `STAGE=<n> bash scripts/test_shopgun.sh`
    (a gun bought in the shop is carried to the next stage and fired there), `bash scripts/test_chest.sh` (a placed chest opens when shot),
    `STAGE=<n> bash scripts/test_chests.sh` (a run blocks the stage's statues and places its own chests in arenas 1 and 2);
    `bash scripts/test_twinguns.sh` (E16 spike, Jill Battle Suit: dev `twinguns`, `twinfire N`, `twinguns off`; the log's
    `dev twinguns: gun N ... attached`, `dev twinfire N: N shots fired`, and `enemy_hit ...` lines for the shots;
    `SPECIAL=1 [DEVSPECIAL=1] [USES=n] [NOGUNS=1]`: Twin Fury itself n times, with or without the guns);
    surveys: `STAGE=<n> bash scripts/survey_objects.sh`, `bash scripts/survey_chests.sh` (all stages, writes `research/chest_survey.json`).
  - Campaign areas (M5): `NUMBER=<n> bash scripts/test_stage.sh` (does stage n load and keep running), `bash scripts/survey_campaign.sh`
    (several stages: captures, own spawns, objects), `NUMBER=<n> bash scripts/dump_stage_sets.sh` (set records), `python scripts/campaign_maps.py <n>...`
    (director maps from the AI waypoints), `NUMBER=<n> bash scripts/test_campaign_arena.sh` (a director arena there); `python scripts/arc_extract.py <arc> [type...]`.
    `STAGES=".." bash scripts/vet_campaign.sh` (start views and player positions of candidate stages), `bash scripts/check_campaign_arenas.sh`
    (a director arena on every campaign map), `bash scripts/test_rotation.sh` (a run from a campaign area to a Mercenaries stage), `NUMBER=<n> CLASS=uEmNN [EXTRAS=..] [FIRE=n SETHP=n] bash scripts/test_boss_spawn.sh` (a boss candidate in an
    area: spawn, health, gunfire, death; `configs/adapter.boss.json` logs every hit), `bash scripts/test_run_map.sh` (a whole run
    through the run map: each stage change must load the chosen node's arena), `bash scripts/test_native_shop.sh` (M7.1: the game's
    Organize/Buy screen from the SHOP with the run in it, a reload upgrade on the M92F, its slot levels after the screen and in
    arena 2; backs up `savedata.bin` to `build/backups/` and prints its hash before and after, which must match),
    `[COSTUMES="3 6 .."] [NEXTMAP=<key>] bash scripts/test_native_shop_roster.sh` (the same screen with each character: restarts the game
    per costume slot, opens and closes the screen untouched, prints the grid before, after and in arena 2 and whether the first aid
    spray's archive is loaded there; same save backup and hash check),
    `[SKINS="chris_warrior .."] bash scripts/test_builds.sh` (M8: each skin's build in a run of its own, with
    `configs/adapter.builds.json` (debug log with every hit's breakdown, invulnerable): the inventory at the start, three shots
    at an enemy of a class the stage loaded, the rewards and the shop, Clubbin's free NEW STOCK; same save backup and hash check),
    `MODE=die|finish bash scripts/test_run_results.sh` (M7.3: the end of a run; `die` with `configs/adapter.mortal.json` lets the
    director's enemies kill the player in arena 1, `finish` clears every arena; the game must go back to its Play menu by itself
    with the mod menu open on RUN RESULTS, which is captured with the main screen and CREDITS; same save backup and hash check),
    `NUMBER=<stage> CLASS=uEmNN [LETHAL=1] [TARGET=1] bash scripts/test_boss_hits.sh` (a unit pinned in the line of fire at
    seven sides and shot: hits per side, with health 1 its death; a Majini, `CLASS=uEm10 HOLDHP=30000`, is the control),
    `STAGE=<boss map> CLASS=uEmNN bash scripts/test_boss_arena.sh` (a boss map's arena end to end: spawn, health, hits, a
    killing shot, its fall, the screens), `STAGE=<boss map> CLASS=uEmNN [CONFIG=configs/adapter.mortal.json] bash
    scripts/test_boss_idle.sh` (the boss left to fight the player, captures every 5 s),
    `[STAGE=<n>] [CLASSES="..."] [FIRE_MS=3500] [GUN=0121] [EFFECTS=none] bash scripts/test_flamethrower.sh` (a gun held on one
    class after another: hits, each class's health and actions, the gun's state; `gamectl` holds a button with `lclick:<ms>`,
    `lclick+<ms>` is a tap and a wait; a few classes at a time, or some are not loaded and their spawns refused),
    `COSTUME=<n> MODDIR=<a mod's nativePC_MT> FILES="<archives>" [CONTROL=1] bash scripts/test_skin_mod.sh` (a costume read
    from a mod's folder in a plain match, captured; CONTROL=1 without it), `[OPEN=1] bash scripts/test_mod_skins.sh` (supported
    mods as a player meets them: one mod unzipped into RogueMercenaries\mods, SKINS with the other one missing, a run in the
    original outfit and then in the mod's, the game's files hash-checked),
    `[STAGE=<n>] [CLASS=uEmNN] [REACTIONS="DmgFire ..."] bash scripts/test_reactions.sh` (an enemy class's own reactions
    started by the mod, each captured: which play, which the class lacks; never list `DeadArmyBurn`, it ends the game),
    `[STAGE=503] [CASES="burn freeze shock toxic blast exotic"] bash scripts/test_elements.sh` (the rarity remake: the gun in
    hand made each case's gun with dev `gun`, a Majini brought into the line of fire and shot; the elements' lines counted,
    frames captured; then dev `refine`),
    `[PART=1|2] bash scripts/test_ladder.sh` (the ladder: part 1, no boss beaten, a short run
    straight from START RUN through its Uroboros to RUN COMPLETE, the profile must then hold `short:easy`; part 2, the short hard boss
    beaten (`scripts/profile_edit.py` rewrites the profile with a valid checksum), START RUN must open RUN LENGTH and its second row
    start an 8-arena run; the profile is restored, the save checked), `bash scripts/test_resume.sh` (a run goes on after a quit and a crash),
    `NUMBER=<n> bash scripts/test_grab_hang.sh` (enemies left to grab the player), `bash scripts/check_campaign_starts.sh` (each campaign map
    as a later arena: where the player starts, do the enemies close in), `bash scripts/survey_stage_preloads.sh` (what each
    Mercenaries stage loads for its enemies: `research/stage_preloads.json`, read by `campaign_maps.py`). `DLL=<path>` makes
    `build/restart_to_play_menu.sh` deploy a fixed build.
  - Diagnostics: the log records the game's own fatal errors with the game code on the stack (`game fatal error (the game shows it and exits): ...`,
    `game fatal error: stack code: ...`); with `developer_mode` a hang watchdog logs where the main thread is when frames stop for 4 s.
  - Developer test aids in `adapter.json` -> `dev`: `invulnerable`, `outgoing_multiplier`, `incoming_multiplier`, `grant_perks`, `probe_hits`, `mute_audio`, `ui_demo`;
    `<data>\dev.command` accepts `force_complete`, `kill <n>` (synthetic player kills), `grant <perk id>` (adds a
    perk to the active run), `unlock perk|weapon <id>`, `dumpinv`, `give <hex item> <count>`, `arm <stage> <costume>`,
    `spawn <class> [weapon hex] [count] [distance]` (an enemy next to the player; refused when the stage did not load
    the class, which would stop the game with "Failed open file ... emXXXX_low.mod"), `rogue` (the mod menu opens by itself at
    the next Play menu: tests press Enter only after "opened on screen 0 by itself", never blind keys on the Play menu, whose
    row 0 starts the campaign), `select <skin id>` (the character and skin the next START RUN plays), `plactions` (research:
    pointers in the player object to player action objects), `nativespawns off|on`, `enemies` (live enemy list), `enemyhp <n>` (every live enemy's health down to n), `warp x y z` (moves the player), `uipreload <archive>...` (screen archives load with the next stages), `itembox [run] [poke <hex offset> <value> ...]` (opens the game's Organize/Buy screen with the save data copied and restored on close; `run` puts the run in it as the SHOP's ORGANIZE row does; needs `ShopResource`, which a run loads with every stage, or `uipreload ShopResource` and a new match), `dumpinv` (the inventory slots with their addresses and raw words, upgrade levels at `+0x20`), `savedump [hex bytes]` (the in-memory save data to `savedump.bin`), `where` (the player's position and its copies in the player object), `killall` (every live enemy dies through the game's own damage call), `abandon` (ends the run; the match goes on as plain Mercenaries), `survey on|off` (logs every native spawn for `scripts/survey_spawns.py`), `chest` (one chest, as a pickup would), `die`, `timeup`, `merchud on|off` (the game's clock and score during a run), `merckills <n>` (sets the game's kill counter; 150 used to end a match), `preload <classes, hex player item ids, hex enemy weapon ids>|clear` (extra archives with the next loads), `loadlists` (the loader's lists), `archive <name>` (loaded or not), `quiet on|off` (no director refills, native spawns blocked), `supplies <n>`, `cell <from> <to>` (moves a grid cell like ORGANIZE), `placechest [distance] [seconds]` (a chest statue in front of the player), `stage <n>` (the next matches load stage number n, e.g. a campaign area; -1 off), `nextmap <key>` (the run's next arena: Mercenaries index or campaign stage number), `fataltest` (the game's fatal error handler: its message must reach the log), `setdump` (every set record of the stage and the player position), `survey on|off` also logs every object and item the stage creates, `buy <kind>`, `itemnames`, `sethp <hp>`, `pause 1|0` (the world-pause request bit), `gomenu` (research: the results screen's return-to-menu call, no effect mid-match), `fadein [frames]`, `scene <hex id>` and the
    memory-probe commands listed in `adapters/re5/src/mod.cpp` (developer mode only, logged).
- Diagnostic export: in game F7 -> Developer -> Export diagnostics writes `%LOCALAPPDATA%\RogueMercenaries\rogue-mercenaries-diagnostics.zip`
  (verified in game 2026-10-27; core API `rm::ExportDiagnostics`).
- Release package: `scripts\package_release.ps1` → `dist\RogueMercenaries-<version>.zip` (DLL, configs, README, licenses, SHA256SUMS).
