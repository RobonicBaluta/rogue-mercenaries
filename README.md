# Rogue Mercenaries

A single-player roguelite mod for **Resident Evil 5 Gold Edition (Steam, build 11465250, `re5dx9.exe` 1.0.0.129)**. Each run is three arenas played as Mercenaries matches. Between arenas you pick perks and weapons, shop with temporary Supplies and choose a route; a run banks permanent Credits for unlocks.

## Status (2026-10-27)

| Area | State |
|---|---|
| Portable core (rules, economy, saves, telemetry) | Built and tested: 63 test cases on x64 and Win32; 10,000-run simulation passes all invariants ([research/sim](research/sim/summary-2026-10-27.md)) |
| RE5 loader (`dinput8.dll` proxy) | Verified in game: loads, verifies 12 engine signatures, exits cleanly |
| Overlay (HUD + run menu) | Verified in game, including the full results → reward → shop → route flow (scripted demo) |
| Combat hooks (kills, damage in/out, heals) | Installed and statically verified; **not yet observed in a real fight** — see test T1 |
| Arena loop on Mercenaries matches | Implemented; **not yet played** — see test T2 |

Details: [research/capabilities.md](research/capabilities.md) (what is verified and how), [research/re5-engine-map.md](research/re5-engine-map.md) (engine addresses), [TASKS.md](TASKS.md) (checklist with evidence).

## Build

Requires Visual Studio 2022 Build Tools (C++ x86/x64, Windows SDK) and Python 3 for one test.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_core.ps1
```

This builds and tests x64 and Win32 and produces `build\win32\adapters\re5\Release\dinput8.dll`.

## Install / uninstall

The installer refuses to overwrite a `dinput8.dll` it did not install and records every file it creates. Close the game first.

```powershell
# install (adapter.test.json = developer menu + detailed logs, audio on)
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Mode install -AdapterConfig configs\adapter.test.json
# remove exactly what was installed; also checks re5dx9.exe is unchanged
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Mode uninstall
```

Installed files: `<game>\dinput8.dll` and `<game>\RogueMercenaries\` (`configs\*.json` and `adapter.json`). Your mod save and logs live in `%LOCALAPPDATA%\RogueMercenaries\`. The game's own save is never touched.

## Playing

1. Launch RE5 from Steam. Top right shows *Rogue Mercenaries – F7 to start a run*.
2. Go to **The Mercenaries → Solo**. Pick Chris or Sheva and a stage.
3. Press **F7**, select the same character and press **Start run**. Press F7 again to close the menu.
4. Start the match. The HUD shows *Enemies left*. The Mercenaries countdown is held at 90 s, so the kill target decides the arena.
5. When the target is reached the match ends. Results and rewards open automatically: take a reward, shop, choose the next route.
6. Press **Retry** (or start another match) to begin the next arena. After arena 3 the run ends and its Credits are banked.

While the menu is open the game receives no input. Use the arrow keys and Enter, or the mouse.

## Tests that need a human (a few minutes each)

These can't be automated, because the game window needs keyboard focus. Install with `adapter.test.json` (above) first.

**T1 — combat hooks (≈5 min).** In a Mercenaries match after **Start run**:
1. Kill about 5 enemies with your handgun.
2. Let enemies hit you 2–3 times.
3. Press F7, then **Developer → Export diagnostics**, and quit.

If The Mercenaries is locked, start a story chapter and use F7 → **Begin arena now (dev, any mode)** instead.
- **Expected:** *Enemies left* drops by one per kill; perk toasts appear.
- **Evidence:** `%LOCALAPPDATA%\RogueMercenaries\logs\adapter.log` contains `enemy_hit ... local=1 ... killed=1` and `player_hit` lines, and `rm_events.jsonl` contains `enemy_killed` and `player_damaged`. The diagnostics ZIP is `%LOCALAPPDATA%\RogueMercenaries\rogue-mercenaries-diagnostics.zip`.

**T2 — three-arena loop (≈15 min).** Play one run through all three arenas: Retry after each reward/shop/route selection.
- **Expected:** after each kill target the match ends by itself, and the next match begins arena 2 and then 3. At the end, the F7 menu shows banked Credits.
- **Evidence:** `rm_events.jsonl` has three `arena_completed` events and one `run_ended`.

Send back the diagnostics ZIP, or the `logs` folder, plus one sentence on what you saw.

## Developer automation

`scripts\smoke.ps1` runs an unattended check. It installs the mod, launches the game through Steam (muted, in the background), captures frames from inside the game, then closes and uninstalls. `scripts\game_config.ps1 -Mode test|restore` switches the game to a small windowed mode and back. See [TESTING.md](TESTING.md).

## Project layout

```
core/        portable roguelite rules (no game code)      tests/       Catch2 tests
configs/     perks, weapons, characters, routes, economy   simulation/  rm_sim (10k-run balance)
adapters/re5 dinput8.dll: hooks, overlay, mod glue        scripts/     build/deploy/smoke/RE tools
research/    capability ledger, engine map, sim reports    third_party/ MinHook, Dear ImGui, nlohmann/json, Catch2
```
