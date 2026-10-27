# RE5 Capability Feasibility Ledger

Target identity for all observations: Steam app 21690; build ID `11465250`; `re5dx9.exe` file version `1.0.0.129`; PE32 x86; SHA-256 `751726F0EC8BD01C00B037AF8AE64CDF0BC5CC5B9025F3F4D782EF6F66E04E26`; PE timestamp `0x63E9CE00`.

Statuses:
- **Verified (runtime)** — observed in the running game, with logs or screenshots.
- **Verified (static)** — the disassembly of the unpacked in-memory image shows the behavior, and the in-process signature check confirms those bytes at runtime. The gameplay effect itself has not been observed yet.
- **Partially verified** — some evidence, with gaps stated in the row.
- **Unverified** — no evidence yet.

Engine facts and their derivation are in [re5-engine-map.md](re5-engine-map.md). Run artifacts (`adapter.log`, window screenshots) are under `build/runs/<timestamp>-<label>/`. They are local and not committed.

| # | Capability | Status | Evidence | Remaining proof |
|---|---|---|---|---|
| 1 | Load a custom DLL without breaking startup | **Verified (runtime)** | `dinput8.dll` proxy (x86, 5 forwarded exports) loaded in 30+ launches on 2026-10-27; clean `process detach` on exit; exe hash unchanged after every uninstall | Coexistence with other proxies (RE5Patch/RE5VR) |
| 2 | Safe periodic callback / game-loop hook | **Verified (runtime)** | Present on the game main thread (~120 Hz) drives `TickGame`; hit callbacks arrive on a game worker thread (t56396/t48932) and are serialized by the mod mutex | Loading screens and cutscenes in story mode |
| 3 | Identify the local player | **Verified (runtime, Mercenaries solo)** | In match: `sPlayer` count=1, class `uPl00ChrisNormal`, number=0, control=0 (`build/runs/manual-session1-adapter.log`) | Story mode with the AI partner (the control-value hypothesis) |
| 4 | Read player health | **Verified (runtime)** | HP 1000/1000 → 814 → 628 … → 0 on successive hits; matches the HUD bar | — |
| 5 | Detect player damage | **Verified (runtime)** | `player_hit raw=196 applied=186` per enemy hit; death detected (HP 0 for 2 s → run failed, banked 0) | Damage from explosions/falls |
| 5b | Modify incoming damage | **Verified (runtime)** | Chris starting perk (−5%): raw 196 → applied 186 on every hit; developer invulnerability → applied 0 | — |
| 6 | Detect enemy death and attacker | **Verified (runtime)** | `enemy_hit class=uEm10 local=1 hp 800->0 killed=1` → `kill counted`; tougher `uEm21` (1400 HP) killed by the second hit | Kills by explosions and environment (no attacker) |
| 7 | Modify outgoing weapon damage | **Verified (runtime)** | Handgun hit raw 250: ×4 test multiplier → 1000; + Sharpshooter×2 and M92F Lv2 → 1253 (×1.2528); Sharpshooter×3 → 1339 (×1.3392). Matches the core formula exactly | Shotgun pellets, melee and grenade paths |
| 8 | Read gameplay stage / mission state | Partially verified | `mode` = 2 in Mercenaries solo (0 at title). `+0x574` is **not** a stage id (it is compared with `+0x578` as a sync counter); stage id unknown | Stage id if routes ever map to specific maps |
| 9 | Count living enemies | **Verified (runtime)** | `sEnemy` listed=12 alive=12 at match start, alive 11 after a kill | — |
| 10 | Control the encounter / end an arena | **Verified (runtime)** | Authoritative countdown is `[mercs+0x4DC]` (game work `+0x680` is a per-frame mirror). Hooking `0x00717850` holds it at 90 s during an arena and zeroes it on clear, and the game then shows its own results screen | — |
| 11 | Display an overlay | **Verified (runtime)** | HUD and run menu drawn in menus and matches; keyboard navigation, digit/Enter hotkeys | Mouse cursor fix (DirectInput-driven) needs a re-test |
| 12 | Cleanly return to the next round | **Verified (runtime)** | `run_0003`: 3 consecutive matches → arenas 1→2→3 (routes outskirts_patrol → market_sweep → ruins_gauntlet), run complete, 269 credits banked and persisted (`build/runs/manual-t2/`) | — |
| 12b | Native Play menu entry and mod menu | **Verified (runtime, Spanish text)** | Play menu (`uIdStartUp01`) extended to 9 rows; the row opens the mod's screens inside the same object, drawn by the game's own row/help/colour routines and navigated by its menu helper (texts are borrowed PS3/Xbox-only messages in the game's glyph encoding). START RUN hands the frame to the game's Mercenaries flow; the run starts with the match (`build/shots/m01.png`..`m05.png`, `p02.png`) | Other languages; the trial menu variant (count 3) stays vanilla; gamepad |
| 12c | Hide the Mercenaries countdown | **Not found** | `uIdMerce00` (HUD, vtable `0x00FA6C84`): its ten draw helpers were ret-patched one at a time and its two GUI page handles blanked; the digits stayed (`build/shots/timer_sheet.png`). `0x00A1D520` draws the score panel | Locate the digit sprites among the 97 `cSprite*` children at `+0x180` |
| 13 | Export logs/diagnostics | **Verified (runtime)** | In-game button wrote `rogue-mercenaries-diagnostics.zip` (summary, adapter logs, configs; `zipfile.testzip()` OK). The live JSONL was skipped (opened unshared) — fixed with `_wfsopen(_SH_DENYWR)` | Re-check that the JSONL is included |

## Static observations versus runtime facts

- The shipped executable is packed (SteamStub `.bind`, compressed sections), so every static fact comes from a runtime dump of the unpacked image. The packer unpacks in chunks, and the adapter waits until every signature site is written before it verifies anything.
- The 2021 trainer's addresses do not apply to this build. Its byte patterns were used only as search seeds.
- RE5VR (2026, same build: its site `0x440320` matches our dump byte for byte) also patches game code with MinHook. That is consistent with our runs, where no integrity check reacted to our hooks.

## Failure modes observed

| Date | Symptom | Cause | Resolution |
|---|---|---|---|
| 2026-10-27 | First process exits silently right after the DLL loads | SteamStub terminates directly started processes (no DLL detach) and asks Steam to relaunch | Launch through `steam://rungameid/21690` |
| 2026-10-27 | Steam refuses to launch (`AppError_16`) | The game's `Launcher.exe` lingers after `re5dx9.exe` exits, so Steam still considers the app running | `gamectl.ps1 -Action reap` after each run |
| 2026-10-27 | Three signatures read as all-zero | Verification started before the packer finished unpacking the higher code pages | Wait until every signature site is non-zero before comparing |
| 2026-10-27 | One full-hook run ended with exit code 0 about 20 s after start | Happened only on the double-process relaunch path; did not reproduce in 7 later Steam launches covering every component combination | Keep monitoring; crash diagnostics (VEH + detach log) now in place |
| 2026-10-27 | Probe ticks stop after boot while the window is unfocused | The game stops polling DirectInput without focus | Tick also from Present on the main thread |
| 2026-10-27 | Countdown "hold" had no effect | Game work `+0x680` is rewritten every frame from `[mercs+0x4DC]` | Hook the countdown update and write the authoritative field |
| 2026-10-27 | The key that closed the menu leaked into the game (Space confirmed character select) | Input blocking stopped while the key was still held | Keys/buttons pressed while the menu is open stay hidden until released |
| 2026-10-27 | Keyboard navigation lost its target when panel content changed | ImGui nav id disappears with the widget | Digit/Enter hotkeys handled by the mod, independent of ImGui navigation |

## Failure logging requirements

For every runtime attempt record the date, executable path and hash, Steam build ID, proxy file hash and architecture, signature results, log path, launch outcome, observed screen or stage, and a rollback check. `scripts/smoke.ps1` captures most of this automatically. If a signature does not match, the adapter disables that capability; it never falls back to guessed addresses.
