# Rogue Mercenaries Development Plan

## Objective

Deliver a single-player Resident Evil 5 roguelite prototype while keeping gameplay rules independent of engine memory and only enabling engine features after testing them against the installed executable. The first proof target is a safely loaded Win32 plugin plus one verified gameplay attribute modification. A playable three-round loop is the Version 0.1 target; research or compilation alone does not satisfy it.

## Environment baseline (2026-10-27)

- Workspace: `G:\Projects\re5mod`; was empty before this plan and reference checkouts.
- Target: Steam app 21690, `G:\SteamLibrary\steamapps\common\Resident Evil 5`.
- Installed Steam build ID: `11465250` (from `appmanifest_21690.acf`).
- Executable: `re5dx9.exe`, PE32 x86, file version `1.0.0.129`, SHA-256 `751726F0EC8BD01C00B037AF8AE64CDF0BC5CC5B9025F3F4D782EF6F66E04E26`.
- Windows 11 Pro x64, Git, Python 3.11, .NET SDK 10, Visual Studio Build Tools 2022/MSVC 14.44 with Win32 tools, Windows SDK 10.0.26100 and bundled CMake 3.31.6 are present. CMake/MSVC are not on the shell PATH by default.
- GitHub CLI is installed. Ghidra, x32dbg and 7-Zip were not found on PATH or in the first installation checks; verify before installing anything.
- Reference checkouts are under `research/references/` and are kept as upstream clones, not product dependencies.

This is an inventory, not proof that a game session launches or that any hook works. The executable is not modified by this plan.

## Phases and gates

### Phase 0 — Baseline and reproducible build

1. Preserve the executable identity above and inspect the game's imports and installed layout.
2. Record build tools, reference commit IDs, relevant source behavior, and missing tools.
3. Create project architecture, research, testing, and task documents (this phase).
4. Initialize Git and establish ignore rules for build products and upstream checkout metadata.
5. Build the RE5Patch reference and a project-owned x86 loader in isolated build folders.
6. Before touching the game folder, confirm the intended proxy name is absent. Keep a manifest of any staged test files and remove only files created by the test.

**Gate 0 proof:** repeatable x86 build logs, PE machine type for produced DLLs, proxy exports/import forwarding inspected, and an unchanged target executable hash.

### Phase 1 — RE5 adapter feasibility

1. Load a diagnostic proxy beside the game executable with all gameplay patches disabled. Verify startup, forwarded input imports, mod initialization log and clean exit.
2. Verify a harmless render callback/overlay path on the D3D9 device. Only then consider ImGui; begin with a simple diagnostic indicator.
3. Find candidate player, health, damage, enemy death and encounter state through source research and targeted runtime inspection. Record signatures with module/build identity, match count, expected instructions, validation bounds and failure behavior.
4. Implement one reversible attribute change in a controlled test and confirm its in-game effect. Do not write to unknown addresses or ship guessed signatures.
5. Test kill and received-damage events independently, including non-player kills and non-gameplay screens. Determine whether enemy counts and responsible attacker are actually available.
6. Determine if an authored finite arena can repeat and whether an existing round can restart without corrupting mission state. Prefer data-driven authored encounters over runtime spawning until a runtime spawn path is proven.

**Gate 1 proof:** tested executable hash/build, diagnostic log, clear visual or gameplay confirmation, and a successful rollback/clean exit. See `research/capabilities.md` for per-capability status and exact evidence.

**Fallback decision:** if the first verified damage modification or reliable event path remains unavailable after testing source-backed candidates and one alternate, compare measured RE5 results against REFramework on RE4 Remake. Preserve the portable core and publish a recommendation before changing target.

### Phase 2 — Portable roguelite core

1. Implement deterministic run state and seeded RNG.
2. Add data-defined characters, perks, weapon instances and rarity/affix rules.
3. Add transactional temporary supplies and persistent credits, with versioned atomic saves and corruption recovery.
4. Add arena outcomes, reward generation, route choices and reset semantics.
5. Add JSONL telemetry with run identifiers and a diagnostic ZIP command.
6. Add headless simulation and automated tests for each rule. Run at least 10,000 simulated runs and report balance and unreachable states.

**Gate 2 proof:** core builds independently of game headers/addresses; automated test output and simulator summary are reproducible from documented commands.

### Phase 3 — First playable vertical slice

1. Use one verified existing RE5 arena with a finite authored enemy set.
2. Start a run and track only engine events already proven in Phase 1.
3. Offer three data-driven perk choices, of which at least four perks have independent core tests and supported adapter effects.
4. Award and spend temporary supplies transactionally; attach a verified damage modifier and one other supported statistic.
5. Present arena results and route choices. Repeat for three rounds using only a verified transition/restart path.
6. Save permanent credits at run end and reset all temporary state on the next run.

**Gate 3 proof:** a human can follow an installation sheet, play all three rounds and end a run; game log proves tracked events and credits/save; no feature is called playable before that run succeeds.

### Phase 4 — Content and balance

Add perks, rarity, upgrades, shops, elite variants, branching route modifiers, unlocks and additional supported arenas. Keep cooperative perks reserved and do not build networking in Version 0.1.

### Phase 5 — Release and support

Repeat runs, test save recovery and duplicate reward prevention, profile overhead, test a clean install, prepare a reversible ZIP installation and uninstall guide, and publish known compatibility limits with the tested executable identity.

## Minimum subsystem proof before investment

| Subsystem | Minimum proof before expanding |
|---|---|
| DLL/proxy | x86 PE, exports resolve to system DLL, game reaches its main menu, init/exit log present |
| Frame callback / UI | one stable callback over a short controlled session; no render-state corruption or crash |
| Player state | repeated bounded reads across gameplay, menus, death and stage transition; no guessed pointer dereference |
| Damage event | health/damage change correlated with a controlled hit and logged once; distinguish healing and scripted changes |
| Outgoing damage | controlled same-weapon comparison with modifier off/on and target/build recorded |
| Enemy kill | controlled kill and non-player kill cases; duplicate/death-animation behavior documented |
| Enemy count | compare displayed count against known groups and delayed spawns in a named encounter |
| Round reuse | complete/reset/restart the same arena twice without leaving softlocks or duplicate state |
| Save/economy | interruption-safe write, corrupt-file recovery, idempotent reward and negative-balance rejection |
| Overlay | repeatable visual evidence plus D3D9 state restoration and clean shutdown |

## Acceptance tests for Version 0.1

- [ ] Clean install on the tested Steam build and rollback without replacing original game files.
- [ ] Begin a run; temporary currency, perks and weapon modifiers begin from defaults.
- [ ] Complete three finite rounds with counts reaching zero and results shown after each.
- [ ] At least four perks are independently tested and visibly/effectively influence supported gameplay.
- [ ] Rewards and purchases are deterministic for a fixed run seed and never create negative balances or duplicates.
- [ ] A route/reward choice changes the following encounter or a verified modifier.
- [ ] Run-end credits persist; save/load succeeds; corrupt save recovers without changing the original game save.
- [ ] Starting another run clears temporary state.
- [ ] Core automated tests and a 10,000-run headless simulation pass.
- [ ] JSONL evidence includes initialization, arena transitions, relevant events, purchases, result, and errors.
- [ ] Manual three-round instructions, uninstall steps and diagnostic export are included.

## Known feasibility risks

- RE5Patch demonstrates a working style of proxy loader and x86 pattern hooks but its current patch set is presentation-focused; its source does not verify the gameplay capabilities this mod needs.
- Enemy-randomizer source edits authored enemy and item definitions inside extracted ARC data. That supports static encounter-data research, not a proven runtime spawn/round-reset API.
- A Steam install and x86 toolchain are present, but launch behavior, matching signatures, player/enemy structures, D3D9 callback, and round transitions are still untested.
- The target game's retail executable and installation files remain user-owned; project code may inspect local files but must not redistribute them.
