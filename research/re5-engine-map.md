# RE5 Engine Map (build 11465250)

Target: Steam app 21690, build `11465250`, `re5dx9.exe` 1.0.0.129, SHA-256 `751726F0EC8BD01C00B037AF8AE64CDF0BC5CC5B9025F3F4D782EF6F66E04E26`, PE timestamp `0x63E9CE00`, SizeOfImage `0x1929000`, fixed image base `0x400000` (no ASLR, `DllCharacteristics=0`). All addresses below are virtual addresses (VA); RVA = VA − 0x400000.

Status words: **verified-runtime** (observed in the running game with logs), **verified-static** (bytes/disassembly of the unpacked image, cross-checked by in-process signature match), **hypothesis** (inferred, not yet observed doing what we claim).

## How this was obtained

1. The shipped executable is packed: unnamed sections whose raw sizes are far below their virtual sizes, a `.bind` SteamStub section as entry point, and one import per DLL. Static analysis of the file on disk is impossible.
2. `scripts/dump_image.py` read the whole module (`0x1929000` bytes, 0 unreadable pages) from a running game at the title screen into `build/dump/re5dx9.flat` (local research data, never distributed). SHA-256 of the dump: `7ece2bfe9627428db6323460c02eef9637b9d97073085e1441ca65c23ea6841e`.
3. `scripts/re5dis.py` (capstone) disassembles the dump; `scripts/re5dti.py` walks MT Framework's runtime type info (DTI) tree.
4. Every address used by code is pinned by an expected-bytes signature in `adapters/re5/src/re5_layout.h`; the adapter re-checks them in-process after the packer finishes. All 12 matched in two consecutive runs on 2026-10-27 (`build/runs/*-sig1`, `*-sig2`).

The 2021 RE5 trainer (Puh00) addresses do **not** match this build (e.g. its god-mode site `exe+77CB40` holds different code). Its byte patterns were used only as search seeds: the HP subtraction `66 29 8E 64 13 00 00` occurs once, at `0x00B7C960`.

## Runtime type info (MT Framework DTI)

`MtDTI` layout: `+0x00 vtable`, `+0x04 char* name`, `+0x08 next sibling`, `+0x0C first child`, `+0x10 parent`, `+0x1C size|flags`, `+0x20 CRC`. Every class exposes `getDTI()` as vtable slot 4 (`+0x10`), a `mov eax, imm32; ret` stub, so the adapter reads the immediate instead of calling game code. `build/dti_all.txt` lists all 4,355 classes reachable from `MtObject` (`0x0124C3C0`).

Relevant hierarchy (verified-static):

```
MtObject > cUnit > uCoord > uModel > uBioModel > uWork > uPawn > uCharacter (0x011EA088, size 0x1B30)
uCharacter > uEnemy (0x011F4F4C, 0x1F50) > uEm0b ... uEmc6, uEmAnimal, uEmNpc (48 classes)
uCharacter > uPlayer (0x012031D8, 0x2F80) > uPl00Chris, uPl01Sheba, uPl02Jill, uPl03Wesker,
             uPl04Josh, uPl05Excella, uPl06Barry, uPl07Rebecca, uPl86Irving
```

Singleton instance pointers sit immediately before each class's DTI (verified-static via constructors writing `mov [global], esi`): `sPlayer*` at `0x011B27DC`, `sEnemy*` at `0x011B1FCC`, `sShell*` at `0x011E86AC`.

## Characters and HP

| Fact | Value | Status | Evidence |
|---|---|---|---|
| HP | `int16` at `uCharacter+0x1364` | verified-static | base `SetHp/AddHp/SubHp` at `0x0084F740/780/7C0` clamp `[+0x1364]` to `[0, [+0x1366]]` |
| Max HP | `int16` at `+0x1366` | verified-static | same functions |
| HP virtuals | vtable `+0x10C` SetHp, `+0x110` AddHp, `+0x114` SubHp | verified-static | uCharacter vtable `0x00F6D580`; 956 vtables inherit the base functions |
| Player SubHp override | `0x00B7C950` `uPlayer::SubHp(short)` | verified-static, hook installed at runtime | 32 vtables (player classes) reference it; sets damage/heal flags in `+0x2D8C` |
| Player list | `sPlayer+0x24 uPlayer*[4]`, count `+0x34` | verified-static | `sPlayer::GetPlayer(i)` `0x007E6220` returns `[this+i*4+0x24]`; loops compare `i < [this+0x34]` |
| Player number | `uint8` at `uPlayer+0x2D9C` | verified-static | `uEnemy::OnHit` copies it into `enemy+0x136C` when the attacker is a uPlayer |
| Control kind | `int32` at `uPlayer+0x2DA8`, values 0/1/2 seen in code | **hypothesis** (0 = local, 2 = AI) | player code branches on `== 2`; enemy death path skips work for `== 1` |

## Enemies and damage

| Fact | Value | Status |
|---|---|---|
| Enemy hit entry | `0x009E0840` `uEnemy::OnHit(cHit*)` (thiscall, `ret 4`), 2 direct callers, not virtual | verified-static, hook installed at runtime |
| Damage application | `0x009DEA50` `uEnemy::ApplyDamage(cHit*, int)` called only from OnHit; computes damage, honours instant-kill flag `hit+0x3C & 0x10000000` (damage = max HP), calls virtual SubHp (`+0x114`) | verified-static |
| Attacker | `0x00C49530` `cHit::GetAttacker()` (unwraps `hit+0x20`) | verified-static |
| Is player | `0x007E6370` `sPlayer::IsPlayer(unit)` scans `mPlayer[4]` | verified-static |
| Enemy list | `sEnemy+0x24` count, `sEnemy+0x30` `uEnemy**` | verified-static (loops at `0x0071E40E`) |

Hook design: `OnHit` records HP before/after and the attacker in a thread-local scope; the base `uCharacter::SubHp` hook scales the amount only when called inside that scope for the same enemy (the synchronous `OnHit → ApplyDamage → SubHp` chain). Kills = HP crossing from >0 to ≤0 inside `OnHit`.

## Game state / Mercenaries

| Fact | Value | Status |
|---|---|---|
| App object | `[0x012340A4]` (same pointer the 2021 trainer used as `base+E243A8`) | verified-static |
| Game work | `[[0x012340A4]+0x1042C]` (2,947 code references) | verified-static |
| Mode | `int32` game work `+0x58`; Mercenaries code checks values 2 and 5 | **hypothesis** for meaning; value 0 observed at title screen (runtime) |
| Stage | `int32` game work `+0x574` (444 references) | **hypothesis**; 0 at title screen |
| Mercenaries timer | `float` seconds at game work `+0x680` | verified-static: `0x00718050` adds bonus seconds to it (`AddTime(int)`), sets `+0x688` (bonus) and `+0x68C` (popup flag) |
| Mercenaries score | `int32` game work `+0x6B4` | hypothesis (trainer pointer path, not yet observed) |

## Menus and messages (verified-runtime, 2026-10-27)

Found with the developer menu probe (`dev.menu_probe`: hooks `move()`, vtable slot 6, of every `uId*` class under `uIdBase`), byte diffs while navigating, and static disassembly.

| Fact | Value | Status |
|---|---|---|
| Play menu | `uIdStartUp01` (vtable `0x00FA89B0`); `move` `0x00A396C0`, init `0x00A39560`, draw `0x00A373B0` (called from inside `move`) | verified-runtime |
| Play menu fields | `+0x14` state (u8, 2 = input; decide sets 3 → 90 → 91 → 92 within the same `move`), `+0x48` cursor, `+0x190` item count (8 Gold, 7 without Extra Content, 3 trial), `+0x194` enabled mask (copied from game work `+0x20` each `move`; a clear bit tints the row red), `+0x18C` row-0 variant (1/2 = "NUEVA PARTIDA") | verified-runtime |
| Row layout | label y = (item + 3) × 50 at x 640 on a 1280×720 canvas; help line `DrawHelp(msg, 570)` at `0x00A17CF0`; row text `DrawMenuText(msg, x, y, argb*, scale)` at `0x00A17660` (ignores `ecx`) | verified-runtime |
| Item → message | label table `0x01131D90` (item 8 → 936 "NUEVA PARTIDA"), help table `0x01131D78`. Row 0 reads label/help cell 8 when `+0x18C` = 1, so these cells are shared | verified-static |
| Decide | `[[0x01249C40]+0x5B4] & 0x009E3420(-1, -1)`; `move` checks decide before navigation; on decide the game writes the item to game work `+0x1C` and re-opens the menu on it | verified-runtime |
| Mouse | `sId` (`[0x011B20C4]`) hit-tests registered row areas (`0x007B5870`); only the game's own 8 rows have areas | verified-runtime |
| Messages | `sMessage` `[0x011B26F0]`; `+0x11534` → `rMessage` for `etc\message\mes_system_<lang>` (type CRC `0x10C460E6`, path at `+4`, per-message pointer table at `+0x60`, raw MSG2 file at `+0x64` with u16 count at `+0x1C`: 2612) | verified-runtime |
| MSG2 text | u16 pairs (glyph, width); end (0, 0x0401), line break (0, 0x0403). Latin font: `A`=0x38, `a`=0x6C (+2 per letter), digits `1`..`9` = 2..18 and `0` = 0 (width 61), space 0x1C, `,` 0x24, `.` 0x26, `(` 0x18, `)` 0x1A, `?` 0x16, `-` 0x2E. No `%`, `+`, `:` glyphs. Widths are identical across messages | verified-runtime (`-` static only) |
| uIdMenu helpers | `Navigate(cursor, count)` `0x009E4BD0` (pad up/down with repeat and wrap, returns the new cursor); `RowColor(argb*, selected, alpha, fade)` `0x00A17350` (white 1.0 / gray 0.7 times 255, alpha from `+0x78`); UI sounds `0x009E4A70` decide, `0x009E4A50` cancel, `0x009E4AC0` deny (ids 0x45/0x4E/0x47 through `[0x011B1D0C]`); `uIdTopMenu00::SetBallIndex(item)` `0x00A58170` writes `[[this+0x5C]+0xD58]`; cancel mask `[[0x01249C40]+0x5B8]` | verified-runtime (mod menu runs on them) |
| Mercenaries setup screens | `uIdStartUp20` (vtable `0x00FA9A54`, move `0x00A47CE0`): Iniciar/Marcadores in state 2 (cursor `+0x48` → gw `+0x5C8`), then the Solo/Duo page in state 10 (cursor `+0x18C` → gw `+0x5CC`, 4 entries); `uIdStartUp21` (`0x00FA9CA0`, move `0x00A49C40`): stage list, state 2, cursor `+0x48` → gw `+0x5D4` (0..7, horizontal); `uIdStartUp22` (`0x00FA9D04`, move `0x00A48260`): character/costume, state 2, `+0x194` → gw `+0x600+4*slot`, states 80..82 while loading. Decide: state 2 → 3 → 90/91/92 (20, 21) or 80/81/82 (22). Slot order seen: 0 `uPl00ChrisNormal`, 1 `ChrisCos1`, 2 `ChrisCos2`, 3 `uPl01ShebaNormal` | verified-runtime (menu probe field logging + snap/diff) |
| Title-flow script | Scripts are state machines over a shared context (`+7` parent state, `+0x24` sub-state, `+0x25` step state 0/1/2, `+0x18` current screen, `+0x28` result). `FUN_00BF9360` (Mercenaries menu): state 0 sets `gw+0x58` = 2 and requests scene 0xB; state 1 `Step20` (`0x00BF2BD0`, Iniciar/Marcadores + Solo/Duo page → `gw+0x5C8`, `+0x5CC`); state 2 dispatches on `gw+0x5CC` to `FUN_00BF78B0` (Solo), `FUN_00BF7AA0` (Duo split), `FUN_00BF8070` (Duo online). `FUN_00BF78B0`: state 0 inits (`+0x5D4 = 0`, `+0x630 = 3`, `+0x634 = 0`, `+0x620 = 1`, `+0x624 = 0`, `+0x5F0` online flag), state 10 `Step(ctx, 0)` = stage screen (`FUN_00A46150` → `uIdStartUp21`), state 0xB `Step(ctx, 1)` = character screen (`FUN_00A464B0` → `uIdStartUp22`), state 0x14 starts the match (`gw+0x620 = 2; FUN_00BF5050(); FUN_007F1610(0xD, 0)` = scene request, 0xF with `+0x79E`). `Step` (`0x00BF2CF0`) creates the screen when `[ctx+0x25] == 0`, waits for its `+0x3C`, returns +1 / −1 (`+0x3E`) / −2 (`+0x3F`). Fader `[0x011E8804]` vtable: `+0x2C` fade in, `+0x30` fade out `(float, frames)`, `+0x38` busy | verified-runtime (direct start) |
| Mercenaries HUD | `uIdMerce00` (vtable `0x00FA6C84`, move `0x00A1E650`, draw tail `0x00A1E580`): 97 `cSprite*` children at `+0x180`..`+0x300`; the draw tail renders two GUI pages (handles `+0x44`/`+0x48` = 0x51/0x52) through `[0x011B2334]` (`0x007C16B0` lookup, `0x007C1600` draw) and then ten overlay helpers. `0x00A1D520` draws the score panel (digit sprites `+0x1B0`..`+0x1C4`, 64-bit division by 1500). The countdown digits were **not** located: none of the ten helpers nor the two pages removes them | verified-runtime (ret-patch / handle-blank experiments) |

## Rendering and input (verified-runtime, 2026-10-27)

- `IDirect3DDevice9::Present` is called on the game's main thread, the same thread that polls DirectInput (`t31988` / `t57312` in run logs), at ~120 Hz on the title screen with VSync on.
- DirectInput keyboard polling (`GetDeviceState`, 256 bytes) stops a few seconds after boot when the window is not focused; the tick therefore comes from Present or input on the main thread.
- A throwaway HAL device on a hidden window exposes the d3d9 vtable; hooks on device `Present`/`Reset` and swap-chain `Present` coexist with the Steam overlay. ImGui renders on the title screen (`build/runs/20261027-183850-overlay/window.png`).
- The packer stub raises its own divide-by-zero / illegal-instruction exceptions at `0x01465A71`/`0x01465A9C` during startup; they are handled by the stub and filtered from crash logs.

## Launching

A direct `re5dx9.exe` start makes SteamStub terminate that process (no DLL detach) and ask Steam to start a new one via `Launcher.exe`. Steam keeps the app marked running until `Launcher.exe` exits, which can block relaunch for minutes (`AppError_16`). Test automation launches through `steam://rungameid/21690` and reaps the lingering `Launcher.exe` (`scripts/gamectl.ps1`).
