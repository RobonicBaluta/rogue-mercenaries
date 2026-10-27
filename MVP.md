# MVP direction (from the user, 2026-10-27)

Requirements, in the user's priority order, and what each needs from the engine. This is the
plan the next sessions work from; TASKS.md tracks the evidence.

## 0. Finish the start flow (in progress)
START RUN → match, with no game screens. The stage and costume are plain game-work fields
(`+0x5D4`, `+0x600`), but the match is started by the title-flow script after the character
screen reports its result, so the clean version calls that script's "start match" step after
writing the fields. Today the screens are driven behind the game's fader (black → loading).
Tool: Ghidra (`research/ghidra.md`), flow script around `0x00BF7xxx`.

## 1. Perks are playstyle traits, per character, from the menu only
- No "+X% damage" / "-X% damage taken" identities. A perk changes *how the character plays*:
  what melee does, what kills give back, how items behave, what the character is allowed to do.
- The first set stays straightforward; one or two traits per character to begin with.
- Perks are unlocked only in the mod menu (credits and challenges); the in-run shop and the
  arena rewards never offer perks.
- Engine work: melee detection (the core already has an `after_melee` condition), kill rewards
  (heal/supplies exist), ammo/item refunds (needs the inventory write below), movement/reload
  changes (needs new hooks; research first).

## 2. The inventory must actually work
- The rolled loadout (weapon, grenades, herbs, sprays, ammo) goes into the game's inventory at
  match start, and run rewards/shop purchases change it in game.
- Engine work: find `uPlayer`'s item table (slot → item id, count; ammo per weapon), verify a
  single write in game (one grenade), then build the apply/sync layer. Everything in the shop,
  rewards and chests is defined in terms of these items.

## 3. Stay in the game between arenas
- Avoid the Mercenaries results screens ("you finished the level"). Preferred: when the kill
  target is met, go straight to the next stage (the direct-start machinery, same run) or, if
  spawn control turns out feasible, continue in the same map.
- The shop looks like the game's own item screen (the campaign "Organize/buy" UI), opened by
  the mod between arenas, or a native list screen in the same style as the Play menu; items,
  weapon upgrades and ammo only.

## 4. Chests instead of time-based challenges
- Chests in the arena (where the game places time-bonus pickups, or at fixed points) hold the
  run's rewards and challenges; opening one is an in-world event, not a menu.
- Engine work: the pickup objects (`uOm08xx` units opened at match start) and their pickup
  handler; replace or reskin the time-bonus pickups.

## 5. Organic next-stage selection
- The route choice happens in the world (which chest/exit the player takes), not in a list.

## Order of work
1. Flow script → true direct start (0).
2. Inventory table + verified write (2); apply the loadout.
3. Perk set rewritten as traits per character; remove perks from shop/rewards (1).
4. Arena-to-arena transition without the results screens (3).
5. Chests and in-world route choice (4, 5); shop UI (3).
