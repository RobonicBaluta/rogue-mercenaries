ROGUE MERCENARIES {VERSION}
A roguelite mod for Resident Evil 5 (Steam). This is an ALPHA: expect bugs and crashes,
and please send your logs (see "When something breaks").

WHAT IT IS
  Each run is three arenas and a boss. Arenas are Mercenaries stages and campaign areas,
  picked on a branching run map. The mod spawns the enemies and there is no clock; from
  the second arena on, each arena also brings enemies from another stage. After
  each arena you pick a reward, shop with the run's supplies, and choose the next arena.
  A run banks Credits, which unlock perks and weapons in the mod's menu. You start with
  every character and skin, but most perks and some weapons are locked until you buy them.

REQUIREMENTS
  - Resident Evil 5 on Steam, the current build (Steam build 11465250, re5dx9.exe 1.0.0.129).
    On any other build the mod switches itself off and the game runs as usual.
  - Windows 10 or 11.
  - A keyboard or a controller. Mouse hover does not work in the game's menus.

INSTALL
  1. Close Resident Evil 5.
  2. Extract this whole ZIP to a folder (do not run it from inside the ZIP).
  3. Double-click Install.bat. It finds the game through Steam and asks before doing
     anything unusual. If Windows warns about an unknown file, choose "More info" then
     "Run anyway". The scripts are plain text in the "installer" folder if you want to
     check them first.
  The installer also copies your RE5 save to %LOCALAPPDATA%\RogueMercenaries\save-backups
  before anything else. The mod is designed never to write your game save during a run.

PLAY
  Start Resident Evil 5 from Steam. "Rogue Mercenaries" appears at the top right.
  In the Play menu choose ROGUE MERCENARIES: it is the last row (press Up from the first
  row). Then:
    START RUN   - starts a run with the selected character.
    CHARACTERS  - pick a character and a skin. Most skins change how the run plays
                  (the line under each skin explains it).
    PERKS       - three slots: Style, Supply, Support. Locked perks are red; buy them
                  with Credits.
    CREDITS     - your Credits, records, the last run and weapon unlocks.
  Arrow keys move, Enter confirms, Esc goes back (on a controller: the game's own menu
  buttons). F7 opens the mod's overlay at any time.

  In a run:
    - The big orange number at the top is how many enemies are left in the arena.
    - Chests (clock statues) give supplies and items. Most need a key; Majini
      sometimes drop keys.
    - Keys 1 to 4 pick the first four inventory cells.
    - Characters with a special (Jill Battle Suit, Josh, Wesker Midnight):
      Q on the keyboard, or LB+RB together on a controller. Wesker's second special:
      X, or both sticks pressed together.
    - Between arenas: ARENA CLEARED, then CHOOSE A REWARD, then SHOP (ORGANIZE INVENTORY
      opens the game's own inventory screen), then NEXT ROUTE.
    - Dying or finishing the boss ends the run and banks your Credits.

KNOWN ISSUES (ALPHA)
  - Only tested on one game save so far. The mod starts its matches itself, so it should
    not need The Mercenaries or its stages unlocked in your save; if START RUN does
    nothing on your save, please say so.
  - Some campaign areas may still have spots where enemies get stuck; the mod moves
    them after a while.
  - The Play menu row only shows with Latin-alphabet game languages. With Japanese,
    Chinese or Russian text, go to The Mercenaries > Solo, press F7 and start the run there.

WHEN SOMETHING BREAKS
  Please send:
    - what you were doing, the character and skin, and the stage;
    - the folder %LOCALAPPDATA%\RogueMercenaries\logs (paste that path into Explorer's
      address bar, then zip the folder). adapter.log has the details; after a crash also
      send adapter.prev.log.

START OVER
  Your progress is in %LOCALAPPDATA%\RogueMercenaries\profile.json. Delete that file
  (with the game closed) to start again with everything locked.

UNINSTALL
  Double-click Uninstall.bat (in this folder, or in the game folder's RogueMercenaries
  folder). It removes exactly what was installed. If another mod's dinput8.dll was there
  before, the uninstaller puts it back. It asks before deleting your progress and logs.
  Optional: Steam > Resident Evil 5 > Properties > Installed Files > Verify integrity.

LICENSES
  The mod uses MinHook, Dear ImGui and nlohmann/json; their licenses are in "licenses".
  No game files are included in this package.
