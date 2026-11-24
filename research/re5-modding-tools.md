# RE5 modding tools (the residentevilmodding.boards.net board), 2026-11-24

The user's pointer: https://residentevilmodding.boards.net/board/15/re5-modding-tools ("here's a bunch of tools that we can
use to animate, add or change models, basically a gold mine for us, no?"). Read on 2026-11-24: both pages of the board
(42 threads, 18 of them spam about email converters) and the first posts of every RE5 tool thread. Nothing downloaded,
nothing run. Every download is off-site (Mediafire, Mega, Google Drive, Patreon, Nexus, GitHub).

## What there is

| Tool | Author, date | What it does | Formats | Needs | Notes |
|---|---|---|---|---|---|
| RE5 LMT Tool v2.0 ([474](https://residentevilmodding.boards.net/thread/474/resident-evil-lmt-tool-wilsonso)) | Wilsonso, 2014 | Splits an .lmt into its parts and packs it back; PS3 to PC LMT converter; a HIT tool (v2.0) | .lmt -> .lmtheader, .motindex, .motptr, .mot, .atkindex, .atk; .hit | nothing named | Mediafire rar. Its animation list (which motion is which) was on the dead invisionfree forum; a Wayback copy exists (`web.archive.org/web/20140627221336/z6.invisionfree.com/Resident_Evil_4_PC/index.php?showtopic=15445`) but web.archive.org is blocked from here |
| Resident Evil 5 Workbench ([21372](https://residentevilmodding.boards.net/thread/21372/resident-workbench-animation-editor-converter)) | bigcoagulatedgravydg, Oct 2026 | Blender add-on: converts RE6 and Tekken 8 animations into RE5, previews and edits hitboxes and attack data, combines animations, edits melee sounds and melee cameras | not named | Blender | Free, on Patreon (patreon.com/LaheyGamingArchive). Other animations need their skeleton too; sounds need hex work |
| Grim's LMT File ([16887](https://residentevilmodding.boards.net/thread/16887/grims-lmt-file)) | grimrose, 2022 | One pl00action.lmt with every Mercenaries and Reunion melee move; a PDF names the slots | .lmt | none | Experimental; changed moves need .hit edits. The slot PDF is the useful part for us |
| Mercenaries 3D animations converted ([21053](https://residentevilmodding.boards.net/thread/21053/mercenaries-3d-animations-converted-re5)) | bigcoagulatedgravydg, Sept 2026 | Krauser's knife and bow, Garrador, Zealot and player damage animations from Mercenaries 3D as RE5 files | .lmt | an LMT template | Another game's assets |
| RE5 mod tool ([1504](https://residentevilmodding.boards.net/thread/1504/re5-mod-tool)) | aman, 2014 (to v0.33a, 2020) | 3ds Max MaxScript: imports a base-game character .mod, exports a .NEWMOD; RE5TriStrip.exe turns that into a game .mod | .mod | 3ds Max (2012 or 2015, the post contradicts itself) | Base game only: Gold Edition .mod files have more than 32 bones in their bone map and do not import (ours is Gold Edition). 4 bones a vertex, one texture a mesh |
| .mod Blender plugin ([17123](https://residentevilmodding.boards.net/thread/17123/plugin-model-blender-version-requirement)) | gabriel6065br, 2022 | One line about Blender .mod import/export | .mod | Blender | No link in the post: unverifiable |
| MT Framework Converter Tool ([20989](https://residentevilmodding.boards.net/thread/20989/mt-framework-converter-tool)) | Nute1cio, Aug 2026 | Makes mod textures, converts normal maps to RE5's | textures | not stated | Patreon |
| Universal Modding Tool ([21086](https://residentevilmodding.boards.net/thread/21086/universal-modding-tool-urheim)) | URHEIM, Sept 2026 | The first post did not show | ? | ? | Unknown |
| MT Framework Sound Tool ([15557](https://residentevilmodding.boards.net/thread/15557/mt-framework-sound-tool)) | Wesky, 2021 (to Oct 2025) | Extracts, replaces and converts sounds in .spc banks; edits .stq/.stqr metadata; .fwse and .xsew to .wav and back | .spc, .stq, .stqr, .fwse, .xsew, .mca | .NET 4.8; 16-bit mono 48 kHz WAV | Open source, GPL: github.com/LuBuCake/MTF.SoundTool. Supersedes Wesky's FWSE/SPC tool and STQ editor |
| grimoire ([19947](https://residentevilmodding.boards.net/thread/19947/grimoire)) | grimrose, 2025 | Searchable index of what each .arc holds, bulk extraction through Fluffy's ARCTool | .arc | Fluffy's ARCTool | Nexus mod 580 (an antivirus flag the author calls false) |
| Old ARC TOOL ([2620](https://residentevilmodding.boards.net/thread/2620/old-arc-tool)) | JoJo, 2014 | An older .arc tool | .arc | none | Some arcs fail; FluffyQuack's Arc Unpacker/Repacker v0.31 is the usual one (thread 481) |
| Grim's Scripts v1.0 ([18584](https://residentevilmodding.boards.net/thread/18584/grims-scripts-v1)) | grimrose, 2023 | grim-path.py builds a file's folder path; .sngw to .ogg and back | .sngw, .ogg | Python | No redistribution |
| RE5 Plugin Loader ([15830](https://residentevilmodding.boards.net/thread/15830/resident-evil-5-plugin-loader)) | Wesky, 2021 | dinput8.dll that loads plugins\*.dll | DLL | VC++ redist, the QoL fixes | The mod's dinput8.dll already does this (plugin_host.cpp) |
| PS4 and PS3 file dumps ([8941](https://residentevilmodding.boards.net/thread/8941/resident-evil-5-ps4-resources), [1386](https://residentevilmodding.boards.net/thread/1386/re5-gold-edition-pack-extracted)) | 2018, 2014 | Other platforms' game files | .arc, .tex, .mod, .stq | | Game files: not for us (the project's rule against pirated game files) |

## What it means for the mod

The mod changes the game at run time from the player's own files and ships none of the game's. That decides what helps:

- **Knowledge, not files.** The most useful things are lists and format notes. Motion ids by name (Wilsonso's list, Grim's
  slot PDF) would let a special pick the right motion instead of trying one and seeing it in a playtest (Uroboros' "weird
  dash" was the boss Wesker's 0x20 picked by guess). The LMT tool's split (.motindex, .mot, .atkindex, .atk) and the
  Workbench's attack data and hitboxes say where a motion's hit data lives, which the mod's borrowed boss moves do not use yet.
- **Sound.** The MTF.SoundTool source (GPL, readable) documents .spc banks and .stq stream tables: what the mod would need
  for its own voice lines or tracks ("boss music, or your own tracks" on the backlog's Later line).
- **Models.** No tool here gives a player Uroboros Wesker. The 3ds Max .mod tool reads base-game models only (Gold Edition's
  bone maps break it), and a boss's skeleton is its own. A playable Uroboros would be a costume mod someone makes (as Leon
  and Ada are), worn the way the mod already wears costume mods.
- **Other games' animations** (RE6, Tekken 8, Mercenaries 3D through the Workbench) could be tried locally, never shipped.
- **Already covered by the mod:** .arc reading and writing (scripts/arc_extract.py, rm/effects WriteArchive), .tex decoding
  (scripts/re5tex.py), the plugin loader, set data (scripts/xfs_setinfo.py), effect lists (rm/effects).

## Rules for using them

- Their executables are the user's to run (downloaded files from these hosts are not run by the assistant). Text files
  (MaxScript, Python, a PDF, GitHub source) can be read as documentation.
- Nothing from another game, and no game file, goes into a release.

## Worth getting (the user offered to download)

1. Grim's LMT File's zip: its slot PDF names pl00action's melee slots.
2. Wilsonso's animation list from the Wayback link above (blocked from here): the page saved from a browser.
3. The MTF.SoundTool source (GitHub, GPL) if the mod is to play its own sounds.
4. The Workbench's readme (Patreon) for its attack and hitbox data, before any melee work.
