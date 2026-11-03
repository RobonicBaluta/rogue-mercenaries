# ROGUE MERCENARIES: road to a shippable game mode

This is the plan from the current prototype (a run layered on top of The Mercenaries) to a
real game mode that the mod owns end to end. It is the to-do list for every following
session: work top to bottom, one item at a time, each closed only by its "done when" test in
game. `TASKS.md` keeps the evidence; `research/re5-engine-map.md` keeps the addresses.

Written 2026-11-01 from the user's direction: *no extra time in the map, no time running
down, spawn exactly the enemies we want regardless of the arena, new arenas, bosses, custom
menus with icons, character and skin previews, a custom organize/buy screen, chests, a
smooth flow, gameplay-changing perks, skins with bonuses and builds. Mercenaries is only a
means; a fully homemade mode is fine. This has to ship.*

## 1. The product that ships

- **Title**: a ROGUE MERCENARIES entry in the Play menu opening the mod's own menus:
  characters and skins with a preview (model or portrait), perks with icons in three slots
  (style, supply, support), meta currency, unlocks, records.
- **A run**: a sequence of arenas (Mercenaries maps and campaign areas), chosen in the NEXT
  ROUTE menu, ending in a boss. No countdown, no score, no Mercenaries results.
- **An arena**: the mod's director spawns exactly the enemies the arena defines (types,
  counts, waves, positions), counts every death (any cause) and places chests. Health,
  inventory and layout carry over.
- **Between arenas**: the game's own Organize/Buy screen (or one indistinguishable from it):
  buy, sell, upgrade, rearrange; rewards in chests and in that screen.
- **Character identity**: perks change how a character plays (grab immunity with a cost,
  items from kills, low-health behaviour); skins carry small percentage bonuses and enable
  builds; supply perks set the starting kit.
- **Shipping**: one zip, install/uninstall that restores the game exactly, settings,
  a crash report, a tested build list.

## 2. Where the prototype stands

Works and verified in game (keep): native Play-menu entry and mod menus; direct start into a
match with any stage and costume; one run per match with the countdown held; stage change
between arenas without the results screen; inventory control (strip to one gun, create items
and loaded models only, discard with the model, carry-over with layout); native in-match
screens in the game's font (results, rewards, shop with rarity colours, organize, route, run
complete); chests placed by the mod at the map's chest positions; perks in three slots per character; all
characters and skins.

Hacked (replace): the Mercenaries countdown still runs underneath (held, off screen); the
Mercenaries spawner decides what spawns; kills are counted from hooks and a death detector;
weapons/enemies are limited to what the stage loaded; the arena ends through the game's
time-up; the shop and route are list screens.

Missing: campaign areas, bosses, custom icons and
previews, the native organize/buy screen. (All four have since been done, M5 to M7.)

## 3. Architecture: the Rogue mode layer

The mod becomes a mode that only borrows the engine's scene, player and rendering. Its
parts, each a module in `adapters/re5` driven by data in `configs/`:

| Part | Owns | Engine pieces it drives |
|---|---|---|
| **Flow** | title → run → arena → between → arena … → boss → end | title-flow hooks (done), scene requests, fader |
| **Host** | loading an arena's map and placing the player | Mercenaries setup script (done); campaign area jump (to research) |
| **Director** | which enemies exist, where, when; the kill count | `sEnemy::Create` + `cSetInfoEmXX` (found), death events |
| **Resources** | models and textures an arena, a weapon or a boss needs | `sResource` / `rArchive` / `sLoadMan`, path formats (found) |
| **Inventory** | kit, items, weapons, layout, carry-over | done (record API, create, clear) |
| **UI** | menus, HUD, between-arena screens, icons, previews | game text routine (done), game sprites/layouts (to research) |
| **Content** | arenas, waves, bosses, perks, skins, loot | JSON in `configs/`, validated by the core |

The portable core (`core/`) keeps the rules (run state, perks, economy, loot, arena and
wave definitions) and stays unit-tested; the adapter only translates them into engine calls.

**Host strategy.** Start every new system on the Mercenaries host (it already loads, starts
and changes stages reliably), then add campaign areas as a second host once area loading is
proven. A Mercenaries arena with the spawner disabled, the HUD replaced and the director in
charge *is* the homemade mode; the campaign host adds maps and bosses. If a host fights us
(scripts, HUD), the fix is to suppress that piece, not to give up the host.

## 4. Engine capabilities: what we know and how to get the rest

Each line is a research spike with a concrete starting point and a test that closes it.

| # | Capability | Known today | Approach | Done when |
|---|---|---|---|---|
| E1 ✔ | Spawn any enemy at a point (proven 2026-11-01: dev `spawn uEm10 70100 2 0` created two Majini at the player, who attacked at once; the game scored and counted them like its own) | `sEnemy::Create(dti, group, mode, flag)` `0x007AF550` → `0x007F4BA0` (DTI `newInstance`, unit group registration); descriptors `cSetInfoEmXX` (e.g. `cSetInfoEm10` vtable `0x01049C94`) are reflected objects (`0x00422C00` registers `mbArmed`, `mArmId` …) applied to a unit by vtable slot 10 (`0x00CC70E0` → base `0x00CCDD20`: position, rotation, weapon, AI flags) | dev command: create a uEm10 next to the player with a filled descriptor; then a list of types | a Majini of a chosen weapon type appears at a chosen point and fights normally; its death is reported |
| E2 | Turn off the native spawner | Mercenaries spawns from its stage set data | find the Mercenaries generator (callers of `sEnemy::Create`'s vtable slot, set-file processing, `cActEvEmSet` for campaign) and gate it | a Mercenaries map stays empty until the director spawns |
| E3 ✔ | Load models on demand (done 2026-11-02: the loader's area lists, `research/re5-engine-map.md` "Resources"; every gun of the content and the stage's roster load with each stage of a run) | formats `%s\em\em%02x\em%02x%02x`, `%s\wp\wp%02x\wp%02x%02x`, `%s\stage\s%03d\s%3d%02x`; classes `sResource`, `rArchive`, `rArchivePack`, `sLoadMan`; creating an unloaded weapon or screen freezes the game | find the function that formats the em/wp paths and the request it makes; call it for extra types during the arena's loading screen; wait for completion | the shop sells a gun outside the costume's loadout and it can be fired; an enemy type foreign to the map spawns |
| E4 ✔ | No timer, no score (done 2026-11-02: the clock is the cockpit HUD's timer draw `0x009F0390`, the score/combo panels the Mercenaries HUD's draw `0x00A1E580`; both skipped in a run, the 150-kill cap held off) | the countdown is held (`0x00717850`), bonuses suppressed (`0x0071CBF0`) | with E2 done, stop the HUD instead: kill `uIdMerce00` or keep it from being created; or use the campaign host (no countdown) | no clock or score on screen in a run; nothing ends the match except the mod |
| E5 | Count kills exactly | hooks + death detector + "all deaths count" | the director tracks its own units: a death = its unit dead or removed | counter equals spawned-and-dead across explosions, fire, falls, flash on plagas |
| E6 ✔ | Campaign areas as arenas (done 2026-11-02 for s503: `research/re5-engine-map.md` "Stages and areas") | `cScenarioArg_AreaJump` (vtable `0x010497E4`), `cStageInfo`, `sBioArea`; stage numbers s1xx–s6xx | trace the campaign's area jump and chapter start; load one area directly from the menu, skip scripts and cutscenes, place the player | a run's arena takes place in a campaign area with director spawns and no story script |
| E7 | The game's Organize/Buy screen | `uIdItemBox00` (vtable `0x00FA60A8`, `0x00A07980`) opens only in the chapter flow (`0x00BF22A0`); creating screens outside their context freezes | open it in its own context: between arenas as a chapter-style transition (campaign host) or by reproducing what the chapter flow loads first (its layout/texture set) before creating it; map supplies to the screen's gold | buy, sell, upgrade and rearrange in the game's own screen between two arenas |
| E8 ✔ | Custom icons and sprites (done 2026-11-03 by the fallback: PNGs in `RogueMercenaries\configs\icons` drawn by the overlay next to the rows; `TASKS.md` M7.2) | menus draw the game's text; `sLayoutSpr`, `rLayout`, `rLayoutSpr`, `cSprite` children in screens | load our own textures (an `.arc` in `nativePC` loaded through `sResource`) and draw them with the game's sprite routine; fallback: draw textured quads in the D3D9 present hook, styled like the game | perk icons visible next to the perk rows in the PERKS screen |
| E9 ✔ | Character/skin preview (done 2026-11-03: the game's own select busts, read from its archives at runtime and drawn by the overlay; `TASKS.md` M7.2) | the game's character select (`uIdStartUp22`) shows the model for a costume slot | host the preview in that screen with our selection driving it; fallback: portraits as E8 textures | choosing a skin shows its model or portrait |
| E10 ✔ | World objects: chests (done 2026-11-02; exits dropped, see M4.2: `research/re5-engine-map.md` "World objects") | pickups are `uOm08xx` (descriptors at `0x00EF0CA0`…), time-bonus action `0x00AD9130` | same path as E1 for objects (find the om manager's create and `cSetInfoOm…` descriptors, `cSetInfoOmArea`) | the director places a chest and an exit object where the arena says |
| E11 ✔ | Bosses (done 2026-11-02 for the Uroboros uEm40: `research/re5-engine-map.md` "Bosses") | boss classes and their setup descriptors exist (`cSetInfoEm4x/6x/8x…`) | per boss: spawn through E1 with E3 resources in a fitting arena; script dependencies listed per boss | one boss fight as the last arena of a run |
| E12 | Perk mechanics | kills, damage, heal, items done; missing grab, move speed, weapon category damage | grab: the `cEm…PlEvent_Catch`/`_PL` actions (refuse or shorten a grab); move speed: player motion rate field; weapon category: equipped record id → category (done) | each mechanic used by a perk works in game |

## 5. The to-do list

Milestones in order. Inside a milestone, items go top to bottom. Each item ends with a
"done when" check in game and an entry in `TASKS.md`.

### M1 — The director (enemies under our control)
1. ~~E1 spike: spawn one uEm10 with a descriptor next to the player (dev command `spawn`).~~ Done 2026-11-01. The Mercenaries spawner calls the same path from `0x007AF6E0` (callers `0x00C2A880`, `0x00C2AA80`, `0x00C2E070`): the gate for E2.
2. ~~Spawn table~~ First version done 2026-11-01 from the survey (`scripts/survey_spawns.sh`,
   `research/spawn_survey.json`): the classes and weapon ids each stage spawns itself, and
   the descriptor class for each (own `cSetInfoEm..`, or `cSetInfoEm10` for Majini variants).
3. ~~E2: switch off the Mercenaries spawner in runs.~~ Gate done 2026-11-01 (`director.cpp`, dev `nativespawns off`); on automatically for a run's whole match on a stage listed in `maps.json`.
4. ~~Core: `maps.json` and `rm::Director`~~ Done 2026-11-01: spawn points, roster with
   weights, weapon ids and minimum tier, max alive; keeps up to max alive, refills until the
   route's enemy budget is dead, prefers points away from the player, backs off from points
   that fail. Explicit scripted waves (counts and timing per wave) come with M6 for boss arenas.
5. ~~Adapter director~~ Done 2026-11-01: spawns what the core asks, gives each unit its own
   id, reports kills from both kill paths (E5).
6. ~~Arena end by the director~~ Done 2026-11-01: the arena clears when exactly its budget has died.
7. ~~Enemies as the game spawns them~~ Done 2026-11-01: native descriptor template (they hunt the
   player from the start), surveyed profiles (weapons, plagas), each stage's special enemies one at a time.

### M2 — No timer, no score
1. ~~E4: remove the countdown and score from the screen and every path that ends the match.~~ Done 2026-11-02:
   no clock, score or combo on screen for the whole run; the countdown stays held and the game's
   150-kill cap is held off, so only the mod (or the player's death) ends a run's match.
2. ~~Replace the status line with a small HUD in the game's style (arena, wave, kills left).~~ Done 2026-11-02:
   the clock's own big orange digits count the enemies left, with the arena where the score was and the
   supplies where the combo was. Waves come with M6 (the director refills continuously until then).

### M3 — Resources
1. ~~E3 spike: preload one enemy type and one weapon during an arena's loading.~~ Done 2026-11-02: queued
   into the loader's lists right before the game commits them; uEm11 spawned in the Village and the
   L.Hawk was created, equipped and fired there (Chris's loadout has neither).
2. ~~Preload list per arena (enemies of its waves, the run's weapons); the shop and chests may then
   offer any gun.~~ Done 2026-11-02: every gun of `weapons.json` (19, about 20 MB) and the armed stage's
   roster with its weapons go with every load of a run; rewards, the shop and chests offer any loaded
   gun, purchases carry over and fire on the next stage; the director never spawns a class whose
   archive is not loaded.
3. Run-planned rosters (user, 2026-11-03): enemies of every kind across a run, not only the
   zone's own, but planned so a stage never loads every type. Today an arena's classes are its
   map's fixed roster (`maps.json`; campaign areas borrow a similar Mercenaries stage's), preloaded
   with that stage. Plan: when the run map is generated, each node gets its roster drawn from a
   run-wide pool (variety across the run, harder classes at higher tiers, a cap on classes per
   arena); every stage load preloads exactly its node's classes and their weapons (E3, proven for
   uEm11 in the Village). Each pool class is vetted on foreign stages first: classes that need their
   own stage's setup (special plaga types, the big special enemies, dogs and birds that need open
   ground) stay with their stage or are left out. Done when a run's arenas each show their planned,
   partly foreign classes, with no fatal error and no class spawned that the stage did not load.
   Coded 2026-11-03, not yet played (`TASKS.md` M3.3): `economy.json` `run_rosters`, `rm::PlanRunRosters`.
   From the second arena on (the first arena's stage loads before the run exists) each node of the
   run map gets one normal guest and, where its threat level lets specials in, one special, from the
   14 classes of the Mercenaries rosters, within 5 classes per arena, preferring classes not yet
   fielded on the paths into the node. Off their home stage only some were seen in game (uEm11 in
   the Village, the chainsaw Majini uEm22 on s503; the campaign areas borrow these rosters, but no
   per-class record of them exists), so the user's playtest is the vetting: a class that misbehaves
   leaves `guests`.
4. Enemy threat (user, 2026-11-03: "easy at start and harder as it goes"; the first arena had stun-rod
   soldiers and a Licker). Coded 2026-11-03, not yet played: every spawn variant has a threat level, the
   higher of its class's and its weapon's (1 bare hands and small blades, 2 big blades, fire, bows and
   handguns, 3 explosives, stun rods, shotguns and the big specials, 4 machine guns, rockets, gatlings,
   the chainsaw). Weapons are graded from their archives' hit effects (`scripts/write_threats.py`,
   `research/re5-engine-map.md`). The first arena is level 1, later arenas their route's tier plus one
   (so 1, then 2 or 3, then 3 or 4); the director fields only variants at or under the level, keeps
   6/8/10/12 alive at once, and lets specials in from level 3 (one more per level above); the run map
   only puts a map on a node when it can field enemies at that level (the soldier maps, Experimental
   Facility and Underground Facility, wait for level 3). Tuning in `economy.json` `enemy_threat`.

### M4 — World objects
1. ~~E10: chests placed by the arena data (no more reliance on time bonuses).~~ Done 2026-11-02: the
   mod creates the time-bonus statue itself (uOm0100 through sObjModel, as the stage's set spawn does);
   in a run the stage's own statues stay off and each arena places `chests.per_arena` (3) of the map's
   chest positions (`maps.json` `chests`, where the game puts its statues).
2. ~~Exits as objects: walking into one picks the next arena (in-world route choice).~~ Dropped
   2026-11-02 by the user: tried as larger exit statues to break after the shop (one per route,
   the HUD naming the nearest), which was awkward to reach and aim at and not fun; the route
   stays the NEXT ROUTE menu. The attempt is not in the code.

### M5 — Arenas beyond Mercenaries
1. ~~E6 spike: load one campaign area directly with the player placed and scripts off.~~ Done 2026-11-02:
   the Mercenaries host loads any stage number in place of its own (`host.cpp`, dev `stage <n>`); the
   area starts at its own player start with no story script; s503 (ship deck) ran a director arena from
   its AI waypoints (`scripts/campaign_maps.py`) and cleared at its budget.
2. ~~Arena catalogue: Mercenaries maps + selected campaign areas with spawn points and chest positions.~~
   Done 2026-11-02: 17 campaign areas vetted in game and listed in `maps.json`, 16 of them in the run
   rotation (AI waypoints around the area's start, chests, the roster of a similar Mercenaries stage
   and everything that stage and the area load for their enemies); runs pick from the 8 Mercenaries
   stages and those areas for every arena (`"campaign_arenas": false` to keep to Mercenaries). The
   fatal error seen on campaign areas (`Failed open file ... wp6f00.mod`) was a weapon model the area
   never loaded; fixed by those preloads.
3. ~~Run map: branching arenas with difficulty tiers.~~ Done 2026-11-02: a run's arenas are a map
   generated when it starts (`rm::GenerateRunMap`): the starting arena, then three per layer, each a
   real arena with a route; every node leads to two neighbours whose tiers differ, tiers rise with
   depth. NEXT ROUTE offers the next nodes by arena name and tier, with the route, rewards, curse
   and the arenas each leads to; the stage change loads the chosen node's arena.

### M6 — Bosses
1. ~~E11 spike on one boss; boss arena as the last step of a run.~~ Done 2026-11-02: eleven boss
   candidates spawned in their home areas (`scripts/test_boss_spawn.sh`); the Uroboros (uEm40) in
   s103 hunts the player, takes gunfire and dies with the game's own death sequence. `maps.json`
   boss maps (`"boss": {class, name, hp}`) end every run: the director spawns the boss alone, the
   arena clears when it dies, the HUD's digits show its health. Runs are three arenas and the boss.
2. More bosses as content allows. Second boss 2026-11-02: the giant Uroboros (uEm43) in s304's capsule
   dome; runs end on one of the two (the last NEXT ROUTE offers both). Campaign arenas start where
   their map says (the game picks an area's entrance at random; the adapter moves the player). Open:
   Wesker and Uroboros Wesker (alive at zero health: their deaths are scripted), uEm44 and uEm60 (need
   their story setup), the uEm46 and uEm52 oddities.

### M7 — Between arenas and menus that look native
1. E7: the game's Organize/Buy screen between arenas (fallback: our screens with E8 sprites).
   Spike 2026-11-02: the screen opens and closes in a Mercenaries match once its archive
   (`ShopResource`) loads with the stage (dev `uipreload ShopResource`, `itembox`); it shows the
   campaign save's inventories, storage and treasures, so the run's state must stand in for them.
   Done for Chris (2026-11-02): the SHOP's ORGANIZE row opens it with the run swapped into the
   save data (grid, supplies as money, a run stash as the storage) and the campaign data written
   back on close; moves, sells, buys and gun upgrades work, and upgrades are the game's own (slot
   levels, kept on the gun for the run). Every character checked (2026-11-02, one costume each):
   their run in the first grid, labelled with their name; a supply perk's first aid spray no longer
   ends the next stage (every item the content hands out preloads). Open: treasures; upgrades
   checked on the M92F only.
2. ~~E8: perk icons in the PERKS screen; E9: character and skin previews.~~ Done 2026-11-03.
3. ~~Title menus with icons and previews; results of a run.~~ Done 2026-11-03: icons on the main screen
   and CREDITS; at a run's end (boss beaten or death) the game's Mercenaries results, its game-over
   choice and their save writes are skipped, the game returns to its Play menu and the mod menu opens
   on RUN RESULTS (also CREDITS → LAST RUN).

### M8 — Builds
Direction (user, 2026-11-03): every skin is its own build, a strong rule with a cost, replacing the
small percentage traits. Requested builds:

| Skin | Build | Needs |
|---|---|---|
| Chris Safari | every drop is a mine (proximity bomb), mines stack to 10 | E13 stack size, E14 drops, item id |
| Chris S.T.A.R.S. | a first aid spray on every arena clear | done in content (`item_on_arena_clear`) |
| Chris Warrior | rewards only shotguns, pistols and magnums; more damage with them | B1 offer filter, B2 category damage |
| Chris Heavy Metal | can unlock the Gatling gun; ammo stacks far past the normal size | E13, item and model of the Gatling |
| Sheva Clubbin' | extra supplies; one free shop reroll per stage | B3 supplies, B4 shop reroll |
| Sheva Tribal | her bow | the costume's longbow kept at the run's start, arrows in the loot |
| Sheva Business | better loot rarity and shop offers; SMGs stronger, past the normal maximum | B5 rarity, B2, B6 uncapped levels |
| Sheva Fairy Tale | starts with a legendary weapon that upgrades without limit | B6, B7 starting weapon |
| Jill BSAA | opens chests without keys; everyone else needs a key from a Majini kill | B8 keys, E15 chest lock |
| Jill Battle Suit | a button combo fires a dual SMG attack, with a cooldown | E16 input combo, E17 attack |
| Wesker Midnight | his special movement (the boss dash), with a big drawback | E16, E18 dash |
| Wesker S.T.A.R.S. | very strong melee; can only use his pistol, magnums and the Hydra (others offered but cannot be bought) | E19 melee detection, B1 |
| Barry | starts with a magnum that upgrades past the maximum; more health | B6, B7, E20 max health |
| Rebecca | heals work better; sprays and mixed herbs add invulnerability and short buffs | E21 heal detection, B9 timed buffs |
| Josh | a button combo calls a computer ally for the stage, once every two stages | E16, E22 AI partner |
| Excella | every melee kill adds a random stat for the rest of the run | E19, B10 run stats |

Core work (B, unit-tested): B1 weapon filters per skin (offered at all, or offered but not
buyable), B2 damage by weapon category and by melee, B3 supplies multiplier, B4 free shop rerolls
per stage, B5 rarity shift, B6 weapon instances without a level cap, B7 a skin's starting weapon
(rarity, cap), B8 keys (drop chance per kill of a Majini class, a key per chest, an exempt skin),
B9 timed buffs (invulnerability, damage), B10 run stats (damage, defence, health, speed, crit).

Engine work (E, each closed in game): E12 the original M8 mechanics (grab immunity with a cost,
move speed); E13 a stack's maximum per item; E14 the items enemies drop, replaced at creation;
E15 a chest that stays shut without a key; E16 button combos from the game's input; E17 the dual
SMG attack (the boss Jill's gun actions `cEm80ActAtkGun*` are not player actions: emulated with
the game's damage call and the SMG's fire unless a player action can be borrowed); E18 the dash
(the boss classes' `cEm84ActDash` likewise; emulated with the player warp, a short invulnerability
and the cost); E19 melee hits (the player's current `cPlAction_pl_act_attack_*` action at the hit);
E20 max health (`engine::SetHealth` writes it); E21 which healing item was used (the inventory count
that dropped with the health rise); E22 a computer ally (`cPlIntelNpcThink` is the campaign
partner's brain: research spike, may not be possible in a Mercenaries match).

Progress 2026-11-03: B1 to B7 and E20 done; Warrior, S.T.A.R.S. Chris, Clubbin', Business, Fairy
Tale, S.T.A.R.S. Wesker (weapons; his melee waits for E19) and Barry play their builds (`TASKS.md`).
E13's stack sizes are coded, not yet used.

Second batch 2026-11-03, coded and not yet run in game (from here on the user playtests and the
fixes come from the logs and their reports): B8 to B10 and every remaining build. E13 used by
Safari (mines to 10) and Heavy Metal (ammo x3). E14 done at the pickup instead of at creation
(`uPlayer` pick-up: an ammo, recovery or grenade item becomes the build's item). E15: a chest opened
without a key is placed again where it stood. E16: the combo read from the hooked keyboard, XInput
and DirectInput (the game's `[JOYPAD]` names). E17 emulated (damage pulses in a cone through
`uCharacter::SubHp`), E18 emulated (the player warp over 0.18 s with core invulnerability). E19:
the action at `uPlayer+0x10F8` at the hit. E21 by the amount `uPlayer::AddHp` gets (60 percent of
the maximum or more is a big heal), not by the item. E22 lead (2026-11-03): the Port stage (s204) made
Josh (`uPl04JoshCos1`, player slot 2, control 1) in a Mercenaries match, so the engine can run a
computer player there; nothing in s204's set data creates him and the area load's player flags
(game work `+0x478`) are not set by the stage, so the source is still unknown, and the `partner:` log
lines will say whether he follows and fights. Until then Josh's backup is a stand-in
(covering fire on the nearest enemy for the arena) until a real ally can be spawned. The special's
cooldown HUD is an overlay ring at the bottom left.

First playtest (2026-11-03): Midnight's dash dealt no damage and sometimes passed through walls; the user asked for his
long-range melee and more attacks. From the game's own code (`research/re5-engine-map.md`, "Character actions and
collision"): every move is now checked with the collision query the game's dodges use, and enemies take the game's
own reactions through their ChangeAction. Wesker's special picks a burst, a lunge (his long-range melee: the Majini
is left in the head-hit state the melee prompt follows) or a dash. Next for E17/E18: starting the player's own
actions (attack_*, avoid_*) the same way, once a playtest shows the enemy reactions work. Port (s204) started the
player on the story's boat, whose climb to the ledge is a two-person move: the start is now the ledge. A run quit
from the pause menu wrote the game's save: the match's quit state now finds the Mercenaries phase cleared.

Second playtest (2026-11-03, TASKS.md "round 2"): the collision query needs its world in ECX (a thiscall); the special's buttons are the mod's while a build with specials fights; Wesker's burst has its own button (a build's special_2); his dash and lunge play the game's forward dodge (E18 partly: an animation over the emulated move); stuck enemies are moved to reachable points; the boss waits 8 s with its own health; the Organize screen keeps Buy and Upgrade out of a run; the SHOP sells NEW STOCK.

Fourth playtest (2026-11-03, TASKS.md "Playtest 4"): E17 and E18 take the bosses' own animations. The engine plays a motion list from another unit's archive on the player (the Uroboros' archive carries Chris's grab motions the same way), and every player model shares one skeleton: the boss Jill's WideFire plays on Jill Battle Suit (her readying and the sweep, her shots landing during the sweep), the boss Wesker's dash-strike, knee and kick on Midnight Wesker's dash, lunge and burst (research/re5-engine-map.md "Motions"). Open: her second gun in the left hand (the boss holds two weapon units; spawning and attaching one to the player is the next step if the motions work).

Order: B1 to B7 with the builds that need no engine work (Warrior, Clubbin', Business, Fairy Tale,
S.T.A.R.S. Chris, Barry), then E19/E20/E21 (Wesker S.T.A.R.S., Excella, Rebecca), E13/E14 and item
ids (Safari, Heavy Metal, Tribal), B8/E15 (keys, Jill BSAA), then E16 with E17/E18/E22 (Battle
Suit, Midnight, Josh). E12 joins where a build uses it (speed for Excella's stats). Balance pass
with `rm_sim` at the end.

### M9 — Ship
1. Install/uninstall and settings checked on a clean copy; crash report export.
2. Test matrix: every character × host type × a full run, soak runs, save corruption.
3. Release notes, version, zip.

## 6. Risks and how they are handled

- **Context-bound engine objects** (screens and models created outside their context
  freeze the game). Rule: never create an engine object without first reproducing what its
  owner loads; spikes start in a dev command and a disposable test run.
- **Scripts in campaign areas.** Suppress per area (event list, triggers); start with areas
  that are open combat spaces.
- **Co-op and network code paths.** Solo only; flows that wait on partner sync are entered
  past the sync or not at all.
- **Game updates.** Every address is checked by signature at start; a mismatch disables the
  feature and says so.

## 7. Working rules

- Verify in game before marking anything done; screenshots and log lines go to `TASKS.md`.
- No overwritten game files: everything lives in the deployed DLL and `RogueMercenaries\`;
  uninstall restores the folder and the executable hash is checked.
- No pirated or leaked material; only the user's own copy, Ghidra on it, and our code.
- Dangerous experiments (object creation, scene changes) only in dev commands, logged.
