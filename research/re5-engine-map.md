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

### Native spawns (survey 2026-11-01, `scripts/survey_spawns.sh`, data in `research/spawn_survey.json`)

| Fact | Value | Status |
|---|---|---|
| Set spawn result | `0x007AF5F0` returns the created unit (0 when refused, already alive or no record) | verified-static and runtime |
| Opening wave | Mercenaries places set 0 (ids 0..11 on Public Assembly: 12 units) when the match starts; refills come from other sets (1, 3, ..) as units die | verified-runtime |
| Classes per stage | 0 Public Assembly uEm10/uEm11 (+uEm20, uEm21 rare); 1 The Mines uEm10 (+uEm13, uEm34); 2 Village uEm12 (+uEm24, uEm0b); 3 Ancient Ruins uEm12 (+uEm24); 4 Experimental Facility uEm14 (+uEm32); 5 Missile Area uEm10 (+uEm23); 6 Ship Deck uEm10 (+uEm22); 7 Prison uEm10 (+uEm13, uEm20, uEm26). uEm62 appears once on every stage: spawning it next to the player showed chickens (1 HP; `build/shots/cap-20261101-230237.png`). uEm0b (Village) is a 1000 HP tentacled creature that grabs the player, the stage's special enemy (`cap-20261101-230446.png`). The classes besides a stage's main Majini are its specials: the game keeps one at a time and respawns it when killed. Stage names follow the menu order; 2 and 6 recognised in play | verified-runtime |
| Weapon ids | unit `+0x1BE4` after spawning: `0x70100` (default) and `0x7014F..0x70175`, a different set per stage | verified-runtime |
| Descriptor classes | Own `cSetInfoEm..` exists for 0b, 0c, 10, 22, 27, 30..32, 34, 3401, 40, 44, 45, 52, 61..63, 65, 6c, 6d, 6f, 70, 71, 80..84. The Majini variants without one (uEm11, uEm12, uEm24 checked; uEm13, uEm14, uEm2x by the same rule) derive from uEm10 and are placed with `cSetInfoEm10` (`director::Spawn` walks the DTI parents). uEm20 (from uEnemy) and uEm26 (from uEm20) have neither and are placed with the generic `cSetInfoEnemy`: 18000 HP each, uEm26 a big hooded axe-wielder (`build/shots/cap-20261101-233347.png`) | verified-runtime (uEm11, uEm12, uEm24, uEm20, uEm26) |
| Unloaded enemy model | Spawning uEm11 (a Public Assembly class) in the Village hung the game, as creating an unloaded weapon does; uEm0b, uEm24 and uEm62 (all Village classes) spawned fine in the same state. The director only uses classes the stage spawns itself until M3 | verified-runtime |
| cSetInfoEnemy fields | property list `FUN_00CCE070`: `+0x70` mInAir, `+0x71` mNoLimitCheck, `+0x74` mModelType, `+0x7C` mMontageNo, `+0x84` mIsForNet, `+0x85` mIsFindPL, `+0x86` mIsGoto, `+0x87` mIsGotoAng, `+0x90` mGotoPos, `+0xA0` mGotoAng, `+0xB0` mSearchType, `+0xB4` mSearchForward, `+0xB8` mSearchCircle, `+0xBC` mSearchHitNo, `+0xC0` mAreaKeepType, `+0xC4` mAreaKeepDist, `+0xC8` mAreaKeepHitNo, `+0xCC`/`+0xD0` mSArg00/01, `+0xD4` mIsScrHitOff, `+0xD5` mbForceMoveFSM. Placement: `+0x20` position, `+0x34` angle about Y (radians, forward = sin, cos) | verified-static |
| cSetInfoEm10 fields | property list `FUN_00CC6C20` (vtable slot 5): `+0xE0` mIsExtra, `+0xE4` mExtraNo, `+0xE8` mIsActing, `+0xE9` mbArmed, `+0xEC` mArmNum, `+0xF0` mArmId, `+0xF4` mArmSetJoint, `+0xF8` mAttribute, `+0xFC` mPlagaType, `+0x100` mSubBomb, `+0x104` mAttackType, `+0x108` mMaskType, `+0x10C` mbLefty, `+0x10D` mbNeckEvent | verified-static |
| Native Majini descriptors | dump of every `cSetInfoEm10` apply (`0x00CC70E0`, survey only), 187 on eight stages: mIsForNet = mIsFindPL = 1 in all, mSearchType 1, mSearchCircle 1000 (156) / 2000 / 500, mbArmed 1 (179), mMaskType -1 (all), mPlagaType 0 / 1 / 4, mArmNum 0..3, mAttribute 0 / 0x80 / 0x8000 / 0x8080 / 0x400 .. Per-class profiles in `research/spawn_survey.json` | verified-runtime |
| Fresh descriptor | the DTI's new instance only sets constructor fields: mIsFindPL 0, mbArmed 0, mMaskType 0, and leftover heap memory at `+0x0C..+0x1C`, `+0x2C..+0x3C`, `+0x4C`, `+0x54..+0x5C`, `+0x6C`, `+0x8C`, `+0xD4..+0xDC` (one had mIsScrHitOff = 3). Spawned like that, enemies idled or wandered off. With those ranges cleared, mIsFindPL/mIsForNet set and a surveyed profile applied, 12 Majini closed in from a mean 2576 to 230 units in 48 s and grabbed the player (`build/shots/cap-20261101-230005.png`) | verified-runtime |
| Kill helper | `uCharacter::SubHp(hp)` (`0x0084F7C0`) from the game thread kills a live enemy; the game then refills as after a real kill (dev `killall`) | verified-runtime |
| Stage names | stage index = Mercenaries stage select order: 0 Public Assembly, 1 The Mines, 2 Village, 3 Ancient Ruins, 4 Experimental Facility, 5 Missile Area, 6 Ship Deck, 7 Prison | 2 and 6 verified-runtime, rest inferred |

On The Mines the player was caught by a large insect's grab twice; with dev invulnerability the grab never resolved and the match stayed in a state where the countdown hook (state 0 only) could not end it. Without invulnerability the same stage stopped refilling after the opening wave and showed no results screen within 45 s (cause not seen). Surveys therefore run one game launch per stage.

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
| Inventory | `uPlayer+0x21A0`: 24 slots of 0x30 from `+4` (`+4` item id, `+8` count, `+0xC` max, `+0x29` valid). `uPlayer::PickUpItem` `0x007E9DF0` dispatches on the id's category (`id >> 8`): 1 weapons/grenades → `Create` `0x00C4B480(inv, id, count, -1, -1, -1, 0xD, attr[9], player, pos, 0)` (→ `0x007BD6A0` → `0x00C4E4F0`), 2 ammo → `AddAmmo` `0x00C4DA90(inv, id, count)`, others → `AddToStacks` `0x00C4BEE0(inv, id, count)` (returns what did not fit). Item records: `[0x010EACF0 + cat*4] + idx*0x48` (`+8` pickup amount, `+0xC` stack max). Names: `mes_item` message = weapon index + 1 (`0x0106` Granada de mano, `0x0107` incendiaria, `0x0108` cegadora, `0x0304` spray). `aGame::NotifyItemGet` `0x00719B60(id, count)` shows the pickup notice (crashes on ids without a resource) Creator internals (Ghidra, 2026-10-28): `0x00C4E4F0` rejects id 0xFFFF or an id without a record, allocates a 0x18-byte record (`0x00C4B500`), and with flags 0xD resolves the slot through `0x00C4D590(id, partner_flag)` (records with `+0x34 == 3` go to the fixed position 10); it returns 0 when that search yields 0xD, i.e. no free grid position. In game the character's own loadout plus three mod-created entries filled the grid: a fourth `Create` (a grenade for a full stack, a spray next to the existing one) returned 0 with 11 of the 24 records in use, so the inventory capacity is the game's item grid, not the record table; the mod tops up existing stacks first and the loadout roll caps distinct items (`max_items`). Record layout: `+0x18` grid position, `+0x1C` == 1 for the equipped weapon, `+0x2C` the weapon object. Ids (dev `itemnames`, from `mes_item`, name message = record dword 16): weapons 0x0102 M92F, 0x0103 VZ61, 0x0104 Ithaca M37, 0x0105 S75, 0x0109 SIG 556, 0x0110 H&K P8, 0x0111 SIG P226, 0x0113 H&K MP5, 0x0116 M3, 0x011A L.Hawk; ammo 0x0201 handgun, 0x0202 machine gun, 0x0203 shells, 0x0204 rifle, 0x0209 magnum; recovery 0x0301 green herb, 0x0302 red herb, 0x0304 first aid spray. | verified-runtime |
| Mercenaries HUD | `uIdMerce00` (vtable `0x00FA6C84`, move `0x00A1E650`, draw tail `0x00A1E580`): 97 `cSprite*` children at `+0x180`..`+0x300`; the draw tail renders two GUI pages (handles `+0x44`/`+0x48` = 0x51/0x52) through `[0x011B2334]` (`0x007C16B0` lookup, `0x007C1600` draw) and then ten overlay helpers. `0x00A1D520` draws the score panel (digit sprites `+0x1B0`..`+0x1C4`, 64-bit division by 1500). The draw tail first clears the draw byte of every element of both pages (`sLayoutSpr::SetElementDraw(this = [0x011B2334], draw, page, element)` `0x007C1600`, elements of a page `0x007C16B0`, count at `+0xC`), then (unless `[app+0x10438] & 0x20000E00`) the helpers switch panels back on: `0x00A1D520` score (shown when `+0xC1`), `0x00A1DC30` combo (`+0xD1`, combo count game work `+0x69C`), `0x00A1E0A0`/`0x00A1E1D0`/`0x00A1E300` bonus popups (`+0xDD`/`+0xE8`/`+0xF1`); numbers through `0x00A1C3A0(value, digits, first sprite, template)` | verified-runtime (2026-11-02: clearing only, the panels stay off) |
| Countdown display | The clock belongs to the cockpit HUD `uIdCockpit00` (vtable `0x00F9E480`, move `0x009F8FC0` = slot 6, ctor `0x009EB480`): the move clears the draw byte of its 0x291 child sprites (`+0x12D0`), copies game work `+0x680` (capped at 5999.99) into `+0x34C` and sets the show flag `+0x344` in modes 1/2/5 (setter `0x009EC370`, hide `0x009EC3A0`); `0x009F0390` draws MM'SS"cc from digit templates (tables `0x01122BF8`/`0x01122C20`, separators via `0x009EC870(0x01122C38)`). Skipping that call removes the clock | verified-runtime (2026-11-02) |
| Match end | `cMercs::UpdateCountdown` `0x00717850`: state `+0x4D4` 0 → 1 at countdown zero or when `[gw+0x58C]` (kills, incremented by `0x007243A0`) reaches 150 (400 with `gw+0x79E`); state 1 → 2 sends `0x00792A70` → `0x00792830` (phase `gw+0x67C` = 2, then 3: end sequence `0x00722780` from `0x007245D0`) | verified-static; kill cap held off in game |

| Pause screen | `uIdPause00` (vtable `0x00FA821C`, move `0x00A308C0`): created with the match and updated every frame; state 0 waits for the pause button (`0x00A2D0C0`), `0x00A2F4B0` opens it and sets the application's pause request `[app]+0x10438 \|= 0x800` under the app lock (`app+0x4`, taken when `app+0x1C` or `[0x01249C2E]` is set), `0x00A2DDE0` closes it and clears `0x1000`. Its rows are drawn by sub-objects, not by the Play menu's text routine; the mod hosts its in-match screens in this move instead and draws with `0x00A17660` (needs the screen object as `this`: a null `this` crashes at `0x009E5896`) | verified-runtime (2026-10-28) |

| Shop and item box screens | `uIdShop00` (vtable `0x00FA8878`): the Bonus Features purchase screen (list entries of 0x48 bytes from `+0x1A4`, price at `+0xC` compared with the save's points `[0x011B2158]+slot*0x8C74+0x1C4`); the title flow step `0x00BF24D0` creates it (`0x00A35C10` with `ECX = 0x011F5608`, `obj+4 \|= 0x2000`, `0x00428B80(this = [0x012340B0], 0x1A, obj)`, done when `obj+0x3C`, deleted through vtable `+0x30`). Creating it in a match froze the game. `uIdItemBox00` (vtable `0x00FA60A8`, 0x5B88 bytes, `0x00A07980`) is the campaign Organize/Buy screen, opened by the chapter flow `0x00BF22A0` and by the match object's phase 3 `0x007189F0` (dispatched by `0x00722990`, aGame vtable slot 5, phases 0..4); a Mercenaries match does not run that dispatcher for phase 3 (phase write → loading sequence → crash) | verified-runtime (negative) |
| Inventory record clear | `0x00C4C4E0(record, 1)`: `0x00C4B3A0` deletes the record's weapon object (`+0x2C`) through the unit manager `[0x011B2200]` vtable `+0x30`, then the record is zeroed with cell `0xD`. Wiping the record by hand leaves the holstered model. Cells: `+0x18` 0..8 grid, 9 knife, `0xB` unarmed, `0xD` none | verified-runtime |
| Time bonus and countdown add | `cMercs::AddTime(this = aGame, seconds)` `0x0071CBF0` (combo +5 s from `0x0071D340`/`0x007243A0`, pickups from the pickup action `0x00AD9130`: state `+0x14` 0 delay → 1 grant → 2 fade, object `+0x18`, seconds int16 at `object+0x1364`) | verified-runtime |

## Rendering and input (verified-runtime, 2026-10-27)

- `IDirect3DDevice9::Present` is called on the game's main thread, the same thread that polls DirectInput (`t31988` / `t57312` in run logs), at ~120 Hz on the title screen with VSync on.
- DirectInput keyboard polling (`GetDeviceState`, 256 bytes) stops a few seconds after boot when the window is not focused; the tick therefore comes from Present or input on the main thread.
- A throwaway HAL device on a hidden window exposes the d3d9 vtable; hooks on device `Present`/`Reset` and swap-chain `Present` coexist with the Steam overlay. ImGui renders on the title screen (`build/runs/20261027-183850-overlay/window.png`).
- The packer stub raises its own divide-by-zero / illegal-instruction exceptions at `0x01465A71`/`0x01465A9C` during startup; they are handled by the stub and filtered from crash logs.

## Launching

A direct `re5dx9.exe` start makes SteamStub terminate that process (no DLL detach) and ask Steam to start a new one via `Launcher.exe`. Steam keeps the app marked running until `Launcher.exe` exits, which can block relaunch for minutes (`AppError_16`). Test automation launches through `steam://rungameid/21690` and reaps the lingering `Launcher.exe` (`scripts/gamectl.ps1`).
