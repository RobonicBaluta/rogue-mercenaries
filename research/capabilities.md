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
| 1 | Load a custom DLL without breaking startup | **Verified (runtime)** | `dinput8.dll` proxy (x86, 5 forwarded exports) loaded in 9 launches on 2026-10-27; game alive 20–45 s; clean `process detach (process exit)` on WM_CLOSE; executable hash unchanged after uninstall (`scripts/deploy.ps1`) | Long session; coexistence with other proxies (RE5Patch/RE5VR) |
| 2 | Safe periodic callback / game-loop hook | **Verified (runtime)** | Present (device) runs on the game's main thread at ~120 Hz; `TickGame` drives core updates from Present/DirectInput on that thread (`*-overlay` run: ticks 2047, presents 3828) | Behaviour during loading screens and cutscenes |
| 3 | Identify the local player | Partially verified | `sPlayer` list/count (static); `IsKindOf(uPlayer)` via DTI. At the title screen the runtime read shows count=0 | In-match read of count, class (`uPl00Chris`…) and `+0x2DA8` control value; the "control == 0 means local" hypothesis is untested |
| 4 | Read player health | Verified (static) | HP `int16 +0x1364` / max `+0x1366` from the base and player SetHp/SubHp code | Observe HP in a match and compare it with the HUD bar |
| 5 | Detect player damage | Verified (static); hook installed | `uPlayer::SubHp` at `0x00B7C950` hooked in every run since `*-sig1` | One controlled hit logs `player_hit` with the matching HP change |
| 6 | Detect enemy death and attacker | Verified (static); hook installed | `uEnemy::OnHit` at `0x009E0840` hooked; attacker from `cHit::GetAttacker` `0x00C49530`; kill = HP crossing to ≤0 inside OnHit | In-match kill logs `killed=1 local=1`; check explosions, burning and partner kills |
| 7 | Modify outgoing weapon damage | Verified (static); hook installed | Base `uCharacter::SubHp` `0x0084F7C0` scaled only inside the OnHit scope for the same enemy; `uEnemy::ApplyDamage` calls it through vtable `+0x114` | Same weapon and enemy with modifier off/on; compare `raw`/`applied` against shots-to-kill |
| 8 | Read gameplay stage / mission state | Partially verified | Game work `[[0x012340A4]+0x1042C]`, read at runtime (mode=0, stage=0 at the title); Mercenaries timer `+0x680` from `AddTime` code | Values in a Mercenaries match and in story mode |
| 9 | Count living enemies | Verified (static) | `sEnemy+0x24` count / `+0x30` array (loops in game code) | Compare the listed/alive count with a known group in a match |
| 10 | Control the spawn sequence / reset the arena | Partially verified (design) | No runtime spawn API. Plan: one Mercenaries match = one arena. The kill budget is enforced by the mod, and the match is ended by writing the countdown (`+0x680` = 0). Retry restarts the stage | Confirm that a timer write of 0 ends the match and that Retry starts a new match detected by the mod |
| 11 | Display an overlay | **Verified (runtime)** | Dear ImGui panel rendered over the title screen (`build/runs/20261027-183850-overlay/window.png`); Present/Reset/SwapChain Present hooked with the Steam overlay active | Behaviour in exclusive fullscreen and on device reset (alt-tab) |
| 12 | Cleanly return to the next round | Unverified | Depends on #10 | Three Retry cycles without a softlock |
| 13 | Export logs/diagnostics | Partially verified | Core `ExportDiagnostics` writes a ZIP (unit-tested); the in-game button is implemented | Press the button in game and inspect the ZIP |

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

## Failure logging requirements

For every runtime attempt record the date, executable path and hash, Steam build ID, proxy file hash and architecture, signature results, log path, launch outcome, observed screen or stage, and a rollback check. `scripts/smoke.ps1` captures most of this automatically. If a signature does not match, the adapter disables that capability; it never falls back to guessed addresses.
