ROGUE MERCENARIES {VERSION}
A roguelite mod for Resident Evil 5 (Steam). This is an ALPHA: expect bugs and crashes,
and please send your logs (see "When something breaks").

WHAT IT IS
  A run is a chain of arenas: Mercenaries stages and campaign areas, picked on a
  branching run map, with a boss every fourth arena. The mod spawns the enemies and
  there is no clock; from the second arena on, enemies from other stages join in, and
  later arenas get harder. After each arena you pick a reward, shop with the run's
  supplies, and choose the next arena. A run banks Credits, which unlock perks and
  weapons in the mod's menu. You start with Chris and Sheva; every other character and
  skin is earned, and its locked row says how (some are secret).

THE LADDER
  Your first runs are 4 arenas long and end on a boss. Beating bosses opens harder
  alternate bosses and longer runs (8 and 12 arenas, with a boss on every fourth arena).
  A 12-arena run that takes the alternate boss on both arena 4 and arena 8 gets a 13th
  arena. Beating an 8-arena run also opens ENDLESS: no last arena, the run ends when you
  fall, with a boss every 4 arenas, stronger each time round. START RUN asks for the run
  length once a longer run is open, and the results screen after a run says what a boss
  win opened. The bosses and campaign areas carry the game's own names (until this
  version two Uroboros had each other's names, and U-8 was called a giant Uroboros).

REQUIREMENTS
  - Resident Evil 5 on Steam, the current build (Steam build 11465250, re5dx9.exe 1.0.0.129).
    On any other build the mod switches itself off and the game runs as usual.
  - Windows 10 or 11.
  - A keyboard, a controller or a mouse (the mouse works on the mod's menus and screens).

INSTALL
  1. Close Resident Evil 5.
  2. Extract this whole ZIP to a folder (do not run it from inside the ZIP).
  3. Double-click Install.bat. It finds the game through Steam and asks before doing
     anything unusual. If Windows warns about an unknown file, choose "More info" then
     "Run anyway". The scripts are plain text in the "installer" folder if you want to
     check them first.
  The installer also copies your RE5 save to %LOCALAPPDATA%\RogueMercenaries\save-backups
  before anything else. The mod is designed never to write your game save during a run.
  Installing over an earlier version is fine, and your progress (Credits, unlocks) carries over.
  If the game folder already has a dinput8.dll from another mod (input fixes, Enhanced
  Movement, a plugin loader), Install.bat moves it into the plugins folder as
  previous-dinput8.dll: Rogue Mercenaries loads it from there, so that mod keeps running
  (see OTHER RE5 MODS below). Uninstall.bat puts it back.

OPTIONAL: COSTUME MODS
  A few skins wear a costume mod made by someone else (Excella Experimenter, Jill
  Classic, Sheva Hound Wolf Squad, Rebecca Winter, Wesker RE4), and Leon S. Kennedy and
  Ada Wong are characters from mods. None
  of them come with this package, and the mod plays fine without them: SKINS lists such
  a skin in red. Press it for its MOD screen: OPEN ITS PAGE (download it there, and
  endorse its creator), OPEN THE MODS FOLDER, and CHECK AGAIN. New in 0.2.71: a skin
  or character still locked opens it too, so its mod is ready when you unlock it. The
  mods folder is
    Resident Evil 5\RogueMercenaries\mods
  Put the download in there as it is (new in 0.2.71): a .zip (or .7z, .rar) is unzipped
  for you into a folder of its name, and any folders inside it are fine. Only the costume
  is used, only while you wear that skin; nothing in the game's own folders changes. For
  Leon, put his Main Files there too; for Wesker RE4 and Ada, all of their files (their
  voice lines). A costume mod you already installed into the game (with Fluffy Mod
  Manager, or by hand into nativePC_MT) is found too, but then the original outfit wears
  it everywhere.
  New in 0.2.72: on a Steam Deck a .zip is unzipped too (unzip a .7z or .rar there
  yourself); an updated version of a mod is found even when its files changed size; and
  once a mod is found, SKINS and CHARACTERS show its own picture, even while that skin
  or character is still locked. Ada's pictures come with this download (her mod has
  none).

OTHER RE5 MODS (THE PLUGINS FOLDER)
  Only one dinput8.dll can be in the game folder, and many RE5 mods (input fixes, Enhanced
  Movement, plugin loaders) come as one. Rogue Mercenaries' dinput8.dll is a plugin loader
  too: when the game starts it loads every .dll and .asi file in the plugins folder next to
  re5dx9.exe (and .asi files next to re5dx9.exe), as the RE5 plugin loaders do.
    - Another mod's dinput8.dll: move it into plugins, under its own name (dinput8.dll)
      or any other, for example InputFix.dll. Or put that mod's whole folder into plugins
      as it comes (new in 0.2.71): its dinput8.dll and .asi files load from there, with its
      own files beside them. If it came with a plugins folder of its own, those files go
      in plugins. When it handles the controls itself, the game's input goes through it;
      with several such mods, through each in turn.
    - A plugin (.dll) or an .asi file: put it into plugins.
    - Enhanced Movement: its dinput8.dll is only a plugin loader, which this one already
      is: leave it out. The mod itself is its plugins\RE5EnhancedMovement.dll (and
      RE5EnhancedMovement.ini): put both into plugins. Its nativePC_MT folder goes into
      the game folder, next to re5dx9.exe, as its own install does.
    - New in 0.3.0: would you rather keep another mod's dinput8.dll (the QoL fix, an input
      fix) in the game folder? Download the Plugin version of Rogue Mercenaries instead: it
      goes into that mod's plugins folder as RogueMercenaries.dll, and an update is the
      same files copied over again. With this package's dinput8.dll, a
      plugins\RogueMercenaries.dll from the Plugin version is left out; you can delete it.
  The log (adapter.log, see WHEN SOMETHING BREAKS) lists every file loaded from plugins,
  or why one was not.

PLAY
  Start Resident Evil 5 from Steam. "Rogue Mercenaries" appears at the top right.
  In the Play menu choose ROGUE MERCENARIES: it is the last row (press Up from the first
  row). Then:
    CONTINUE RUN - only after a run was interrupted (a quit or a crash): the arena you
                   left starts again.
    START RUN    - starts a run with the selected character.
    PLAYERS      - 1, or 2 on a split screen (see TWO PLAYERS below).
    PLAYER 1     - with 2 players, in place of CHARACTERS: each player picks a character
    PLAYER 2       and a skin on the same screens. PERKS then asks which player first.
    CHARACTERS   - pick a character and a skin. Most skins change how the run plays
                   (the line under each skin explains it).
    PERKS        - three slots: Style, Supply, Support. Locked perks are red and show
                   their price in Credits; press one to buy it.
    CREDITS      - your Credits, records, the last run and weapon unlocks.
  Arrow keys move, Enter confirms, Esc goes back (on a controller: the game's own menu
  buttons). F7 opens the mod's overlay at any time.

  In a run:
    - The big orange number at the top is how many enemies are left in the arena (in a
      boss arena: the boss's health).
    - New in 0.2.6, solo: the ARENA INTEL panel at the top right shows the time, kills,
      damage dealt and taken, supplies, keys, credits, your kill chain, the boss's health
      and the rating so far. "info_panel": false in adapter.json turns it off.
    - Chests (clock statues) give supplies and items. Most need a key; Majini
      sometimes drop keys.
    - Keys 1 to 4 pick the first four inventory cells.
    - Sniper rifles aim over the shoulder like any gun. While aiming, press Z (the right
      stick's click on a controller) to look through the scope, and again to leave it.
    - Skins with a special: Q on the keyboard, or LB+RB together on a controller. A
      second special: X, or both sticks pressed together.
    - Between arenas: ARENA CLEARED, then CHOOSE A REWARD, then SHOP (ORGANIZE INVENTORY
      opens the game's own inventory screen), then NEXT ROUTE.
    - A weapon upgrade adds damage, and more rounds and a faster reload as far as that gun
      has levels left (a few guns, like the Px4 and the launchers, have none). Upgrades are
      only offered for guns you still have.
    - New in 0.2.7: the guns the game gives no levels get their own with each upgrade (the
      SHOP's upgrade line says what): the Px4 and Samurai Edge more crits, a headshot now
      and then giving its round back, supplies a kill and more rounds; the Gatling more
      damage the longer it fires, staggers and crits; the longbow hits the one behind the
      target and every few hits an arrow explodes; the stun rod's stun lasts longer, jumps
      to those around and stunned enemies take more; the grenade launcher a free round now
      and then, a wider blast, burns or poison and more rounds; the rocket launcher a wider
      blast and a second rocket an arena at level 3, a third at level 5; the flamethrower
      more fuel, a faster refill and a longer flame (10 m at level 5).
    - New in 0.2.7: the flamethrower burns everything in its flame, bosses and big enemies
      too, whatever its element, and its damage shows on the panel. Incendiary sets a fast
      burn, Toxic a poison that stacks once a second and lasts until the enemy dies, and
      Plague's victims let out a green cloud. Burns and poison hurt bosses as well (they
      end when a boss goes down for its next phase), and Jill and Wesker flinch when the
      flame takes a share of their health in one go.
    - New in 0.3.0a: a gun with an element shows it. Its picture in the inventory takes
      the element's colour and badge (flame, bolt, snowflake, blast, drop), and its model
      takes the colour. "gun_looks" in adapter.json turns each part off or makes it
      stronger (README.md).
    - New in 0.3.0b: an enemy shows its status by colour: orange while it burns, icy blue
      while frozen, green while poisoned, back to normal when it ends; an elite keeps a
      faint tint of its element. The effects that played on them are off
      ("element_lights": true in adapter.json brings them back).
    - New in 0.3.0b: charms, items that take a cell of your grid and work while you carry
      them. Clubbin' Sheva's Fidelity Card (every third chest a rare weapon or better),
      Wesker's Perfection, the Uroboros Virus (kills make him stronger, a hit clears it),
      Ada's Grapple Gun (no grab holds her, the special button flips her clear).
    - New in 0.3.0b: Squad Tactics' hand grenades burst into four bomblets that hurt
      enemies only; the Grenade Pouch's grenades hit harder and wider, the Flash Pouch's
      stun the masked Majini too and for longer, the Fire Pouch's burn like the element;
      the Ammo Belt's stacks are 50 percent larger, the first ones too; Scavenger's guns
      refill a little by themselves when everything is empty.
    - New in 0.3.0b: a Supply perk's gun of your starting gun's kind takes its place,
      better, so no run starts with two guns of one kind.
    - New in 0.3.0c: dogs (the Adjule) join the enemies: on The Bridge and the Ship Deck
      from the second threat level, in every arena from the third, and more of them the
      higher the tier.
    - New in 0.3.0c: Perfection is epic; the Uroboros Virus stacks to 30 (the same total
      as before, slower), and the info panel counts its stacks like ammo. At 30 stacks
      Midnight Wesker's burst button turns him into Uroboros: everyone freezes while the
      camera turns around him, then 12 seconds with no damage, no flinch and no grab,
      his other button lashing everyone ahead. It comes back 120 seconds after it ends;
      burst works all the while.
    - New in 0.3.0c: a Majini that grows a Plaga out of its neck fights on again and
      counts when the Plaga dies (0.3.0b killed it as the Plaga came out).
    - New in 0.3.0c: the Cryo, Toxic and Shock flamethrowers keep the flame's sound; a
      Cryo freeze now holds every enemy still, not only the Majini.
    - New in 0.3.0c: a charm shows its picture in the match's grid as in ORGANIZE.
    - New in 0.3.0c: the Drilling Facilities refuse the jump down at the boat's mooring,
      and a jump down that holds you for 6 seconds puts you back at the arena's start.
    - A rocket launcher, once bought, comes back at the start of every arena (into the
      storage if your grid is full). Selling it gives it up for good.
    - What does not fit in your nine cells waits in the storage of ORGANIZE INVENTORY.
      A gun that went there stays named at the top of the SHOP until you open ORGANIZE,
      and a gun offered while your grid is full says TO STORAGE.
    - New in 0.2.7: every arena plays another of the game's battle tracks; the Mercenaries
      theme comes round now and then. "music_rotation": false in adapter.json keeps the
      theme in every arena. The arena's time no longer runs while the game is paused.
    - New in 0.2.71: the "recharge flamethrower" prompt (in Public Assembly's three-storey
      house) no longer takes your flamethrower away. The fat Majini (Mines, Prison), the
      tribal big Majini (Village, Ruins), the Gatling Majini and the megaphone Majini
      come with their health from the game's own Mercenaries, not a Majini's.
    - New in 0.2.72: Ada has no bow any more. Her Red Dress starts with a rare PSG-1
      (rifles hit 40 percent harder), and her supply perk, now Machine Pistol, gives a
      VZ61 and 60 rounds.
    - New in 0.3.0, Safari Chris (Mine Layer): only grenades become proximity bombs, from
      drops, chests, the shop, rewards and perks alike; ammo and herbs stay. Bombs stack to
      10, one more comes every 8 kills, they hit 50 percent harder with a wider blast, and
      a bomb's kill is his. His guns deal 25 percent less, and explosions hurt him half.
    - New in 0.3.0: Shock and Cryo go off more often on the slow guns (the longbow, the
      magnums, the bolt rifles), Shock arcs further and harder, and a kill with a Cryo gun
      may freeze each enemy around it (more often with Absolute Zero).
    - New in 0.3.0: after Uroboros Wesker, the next stage comes (it never loaded).
    - New in 0.3.0a: Battle Suit Jill's Twin Fury fires real bullets from her two guns,
      each hitting the first enemy in its line.
    - New in 0.3.0a: the SHOP's upgrades also make a gun fire faster, and the H&K P8, MP5,
      Lightning Hawk, M29 and M500 shoot through more enemies, as far as the game's own
      levels go.
    - New in 0.3.0a: elite enemies, once you have beaten Uroboros Wesker. Now and then an
      enemy carries an element, glows with it and has a trick of its own to watch for.
      An elite's kill pays twice the supplies.
    - New in 0.3.0a: Chris's Iron Grip and Ada's Grapple Gun throw off every grab at once,
      the chainsaw Majini's and the big axe men's included.
    - New in 0.3.0a: move speed bonuses (Excella's Ambition) make you walk and run faster
      for real, and a Cryo elite's chill slows you.
    - New in 0.3.0a: Tribal Sheva's Quiver heals on kills only (the longbow takes no ammo
      item, so the arrows it handed out did nothing).
    - New in 0.3.0a: your progress is saved without the short pause each save made.
    - New in 0.3.0a: the town's women Majini (Public Assembly) burn, freeze, stagger and
      fly from the elements and the flamethrower like the men.
    - New in 0.3.0a: a head your shot bursts is always a kill: the Majini no longer grows
      a Plaga from it and fights on.
    - Dying or beating the run's last boss ends the run and banks your Credits.

TWO PLAYERS (SPLIT SCREEN) - NEW IN ALPHA.28, MADE PLAYABLE IN ALPHA.29 AND ALPHA.30
  Two players on one PC, the screen split in two: the game's own split screen. Player 2
  needs a controller; player 1 can use the keyboard or another controller.
  In the mod's menu set PLAYERS to 2. PLAYER 1 and PLAYER 2 each open the CHARACTERS
  and SKINS screens for that player (an outfit in red is the other player's costume
  slot, which a mod outfit cannot share), and PERKS asks whose perks first. Then
  START RUN. With two controllers connected the match starts right away (new in 0.2.6):
  player 2 is the controller connected last. Otherwise the game's split-screen lobby
  appears: player 2 presses START on their controller to join, then both press Ready.
  Player 1 plays on the top half, player 2 on the bottom half.
    - Each player has their own character, skin build, perks, weapons, inventory and
      health. The supplies are shared.
    - An arena has half again as many enemies, more of them at once, and a boss has more
      health.
    - Each player's skin special is on their own controller (LB+RB, both sticks), with
      its rings on their half. The arena, supplies and keys show once, at the top as in
      solo; each half shows how many enemies are left.
    - A player who is dying (crawling) can still be helped up by the other, the game's
      way. A player who dies stays down until the next arena, which they start with half
      their health; the other plays on (moving, aiming, weapon rings and inventory). The
      run ends when both have fallen.
    - A chest is the nearest player's. The enemies are shared evenly between the two of
      you (new in 0.2.6): each comes for the player who has fewer, from near them.
    - Leon and Ada have their own names on the weapon rings and in the inventory.
    - New in 0.2.7: each player's rewards and SHOP offer other guns, perks and items than
      the other player's (the heal, the reroll and an upgrade of your own gun aside).
    - Between arenas each player has their own screens at once (new in 0.2.6): player 1's
      in the top half, player 2's in the bottom half, each on their own controller (the
      keyboard works player 1's). Without a controller for player 2 the screens take turns
      in the top half. NEXT ROUTE is a vote: the same pick goes, different picks one of
      the two at random.
    - ORGANIZE INVENTORY shows your nine cells as a grid: pick one, then another, to swap
      them. A picked item can also go TO THE STORAGE or be SOLD
      (half its SHOP price, in supplies). The storage is the run's, both players': pick
      an item there, then a cell, to take it into your grid. A gun keeps its rounds.
    - Each player's flamethrower shows its element's colours, and each player's sniper
      rifle has its scope on their own controller (right stick click while aiming).
    - The lobby, when it appears, only does so before the first arena. After the run the
      game goes back to the Play menu with the run's results.
    - An interrupted two-player run (the game closed or a match quit) is saved like a
      solo one: CONTINUE RUN shows the lobby again, player 2 presses START to join, both
      press Ready, and the arena that was left starts again with each player's items.

KNOWN ISSUES (ALPHA)
  - The mod does not need the Extra Content DLC or The Mercenaries unlocked, but it has
    only been played on one full save, with those simulated for the rest. If "Rogue
    Mercenaries" shows at the top right but something is missing, send your logs.
  - New in 0.2.1: ENDLESS (it has had one short test run: please report what goes wrong
    deep into one) and Popokarimu as an alternate boss of arena 4.
  - Two players (split screen) is new and has had one short session with two real
    controllers: please report what goes wrong.
  - Uroboros Aheri (Excella, on the Bridge Deck), one of the alternate bosses of arena 8,
    may not be beatable yet: pick another boss there.
  - New in 0.2.6, harder bosses: Jill (more health, attacks from any distance, kicks up
    close, dodges your aim, flinches only from explosions), Wesker (gets back up twice,
    calling in enemies each time) and Uroboros Wesker (more health, rage, his second form
    when he first falls). Please report a fight that cannot be won or never ends.
  - The campaign areas' ladders can be climbed since this version (before, they were
    only scenery, and Factory Floor was left out of the runs for it). If a ladder still
    ignores you, please report the arena.
  - An enemy that stands far away out of sight while nothing dies is moved nearer
    after a while (never where you can see it). The last enemy of an arena is pulled
    nearer twice at most; if it is still not found, it is finished and the arena ends.
  - The Play menu row only shows with Latin-alphabet game languages. With Japanese,
    Chinese or Russian text, go to The Mercenaries > Solo, press F7 and start the run there.
  - Trainers: cheats that freeze or change the character, the stage, the Mercenaries
    timer, kills or score, health or the inventory fight the mod, which sets those itself
    during a run. Turn them off before a run (a trainer's saved settings come back when it
    starts), and use the mod's skins instead of a character swap.

WHEN SOMETHING BREAKS
  If "Rogue Mercenaries" never shows at the top right of the game, check that dinput8.dll
  is still in the game folder: an antivirus may have removed it (it hooks the game, which
  looks suspicious). If it is gone, allow it and run Install.bat again.
  If the game stops with "inflate: incorrect data check", one of the game's files is
  damaged: in Steam, right-click Resident Evil 5, Properties, Installed Files, "Verify
  integrity of game files". If it happens again, send your logs: they list the archives
  the game was reading and check each one.
  Please send:
    - what you were doing, the character and skin, and the stage;
    - the folder %LOCALAPPDATA%\RogueMercenaries\logs (paste that path into Explorer's
      address bar, then zip the folder). adapter.log has the details; after a crash also
      send adapter.prev.log.

START OVER
  Your progress is in %LOCALAPPDATA%\RogueMercenaries\profile.json. Delete that file
  (with the game closed) to start again from the first 4-arena run, with everything
  locked.

UNINSTALL
  Double-click Uninstall.bat (in this folder, or in the game folder's RogueMercenaries
  folder). It removes exactly what was installed. If another mod's dinput8.dll was there
  before, the uninstaller puts it back. It asks before deleting your progress and logs.
  Costume mods you put in RogueMercenaries\mods stay; delete that folder yourself.
  Optional: Steam > Resident Evil 5 > Properties > Installed Files > Verify integrity.

LICENSES
  The mod uses MinHook, Dear ImGui and nlohmann/json; their licenses are in "licenses".
  No game files are included in this package.
