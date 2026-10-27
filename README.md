# Rogue Mercenaries

A single-player roguelite mod for **Resident Evil 5 Gold Edition (Steam, build 11465250, `re5dx9.exe` 1.0.0.129)**. Each run is three arenas played as Mercenaries matches. Between arenas you pick perks and weapons, shop with temporary Supplies and choose a route; a run banks permanent Credits for unlocks.

## Status (2026-10-27)

| Area | State |
|---|---|
| Portable core (rules, economy, saves, telemetry) | 63 test cases pass on x64 and Win32; the 10,000-run simulation passes all invariants ([research/sim](research/sim/summary-2026-10-27.md)) |
| RE5 loader (`dinput8.dll` proxy) | Verified in game: loads, checks 13 engine signatures, exits cleanly |
| Combat hooks | Verified in game in The Mercenaries:<br>• real kills are attributed to the player<br>• incoming damage is modified (perk −5%)<br>• outgoing damage is modified (Sharpshooter stacks + weapon upgrade)<br>• death ends the run |
| Arena loop | Verified in game:<br>• each match is an arena<br>• the mod ends the match through the game's countdown when the kill target is met<br>• three arenas, then credits are banked and still there after a game restart |
| Overlay | HUD and run menu; digit/Enter hotkeys and mouse |
| Not yet tested | Story mode with the AI partner; Sheva and other characters in game; balance with real players |

Details:
- [research/capabilities.md](research/capabilities.md): what is verified and how.
- [research/re5-engine-map.md](research/re5-engine-map.md): engine addresses.
- [TASKS.md](TASKS.md): checklist with evidence.

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
3. Press **F7**, select the same character and press **Enter** (Start run). The menu closes by itself.
4. Start the match. The HUD shows *Enemies left*. The Mercenaries countdown is held at 90 s, so the kill target decides the arena.
5. When the target is reached the match ends. The menu opens on the results screen:
   - **Enter** — continue;
   - **1–3** — take a reward;
   - digits buy in the shop, **Enter** leaves it;
   - a digit picks the next route.
6. Accept the game's results and start the next match (pick a stage and character again) to begin the next arena. After arena 3 the run ends and its Credits are banked.

While the menu is open the game receives no input. The mouse works too, and so do the arrow keys with Space.

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
