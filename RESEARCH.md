# Research Notes

Last updated: 2026-10-27. This records local source inspection and environment evidence. See `research/capabilities.md` for runtime feature status. No game memory patch has been applied by this research.

## Target executable and install

- Steam app 21690, display name Resident Evil 5; installed depots listed in `G:\SteamLibrary\steamapps\appmanifest_21690.acf`; app build ID `11465250`.
- `re5dx9.exe`: 8,195,416 bytes, PE32 x86 (`machine 14C`), image base `0x00400000`, file version `1.0.0.129`, SHA-256 `751726F0EC8BD01C00B037AF8AE64CDF0BC5CC5B9025F3F4D782EF6F66E04E26`.
- Imports include `XINPUT1_3.dll`, `DINPUT8.dll` and `d3d9.dll`. The game folder did not contain a local `xinput1_3.dll` when inspected.
- The game's `nativePC_MT` directory contains unpacked game data folders, while ARC containers are present elsewhere in the install and must be enumerated before extraction. No file has been modified.

## Environment

- Windows 11 Pro x64, OS build `10.0.26200`.
- Git, Python 3.11, .NET SDK 10.0.301, MSVC 14.44.35207 x86/x64, Windows SDK 10.0.26100, VS-bundled CMake 3.31.6 and MSBuild are present.
- CMake/MSVC are not in PATH; use the discovered absolute paths or `vcvarsall.bat` in build scripts.
- Ghidra, x32dbg and 7-Zip were not found in initial checks. No dependency installation is justified until a task requires one.
- Steam also lists RE2 and RE4 installations, but no RE4 Remake feasibility investigation has started.

## RE5Patch source inspection

Repository: [chuckstation/RE5Patch](https://github.com/chuckstation/RE5Patch), local clone commit `a82342321202a117fdaec9826b1d28d4b0babb36`, MIT license with vendored BSD-licensed inih. It identifies itself as a 32-bit proxy for `re5dx9.exe` and exposes xinput/dinput/dxgi-style forwarding. `DllMain` disables thread callbacks and starts a worker; the worker initializes logging, reads `RE5Patch.ini`, delays, then runs patches. `helper.cpp` parses byte patterns and scans the host module; patch helpers change protection and flush the instruction cache.

The patch set in this checkout is presentation-focused (FOV, FPS, resolution, colour grading, shadow quality, UI scaling and movie/cutscene fixes). Its code is useful for proxy and signature-scanner examples. It does **not** identify player health, incoming/outgoing damage, enemy death, living enemy count or encounter reset. The README says pattern failure is logged and recommends reporting the game build; therefore, source presence alone is not validation against this installed build.

## RE5 enemy randomizer inspection

Repository: [missehsjune/resident-evil-5-randomizer](https://github.com/missehsjune/resident-evil-5-randomizer), local clone commit `81a256e229567d9871b4d006200b5d92faf83120`. It is a .NET Framework 4.7.2 WinForms tool. Its README instructs backing up level ARC files and using FluffyQuack's ARCTool. The source invokes `ARCtool.exe` with `-alwayscomp -xfs -re5 -texRE5 -v 7 -pc`, then edits extracted `_em` enemy and item XML/TXT data in authored level folders and repacks the files. Enemy definitions contain class IDs, game IDs, life ranges, serialized properties and XML templates.

This is concrete evidence that static authored encounter files can be altered and that data edits can have compatibility/softlock risks. It does not expose a runtime spawn function or prove a repeatable in-session round reset. It also warns that some enemies can crash and some DLC data can softlock. Any ARC research here must use a copied file and preserve hashes.

## ARCTool and formats

The [FluffyQuack tools page](https://www.fluffyquack.com/) describes ARCTool as an MT Framework ARC extractor/repacker with conversions for supported contained formats. The randomizer's exact command confirms RE5, XFS, TEX and PC options in its chosen workflow. Actual file variants and round-trip integrity are not yet verified against this install; `TASKS.md` tracks a copied-file test. Do not infer support for all LOT/GMD/TEX/XFS variants based only on a general tool description.

## References consulted

- [RE5Patch README and source](https://github.com/chuckstation/RE5Patch): proxy loader, x86 restriction, build methods, signatures/logging and patch scope.
- [RE5 enemy randomizer README and source](https://github.com/missehsjune/resident-evil-5-randomizer): ARC workflow, authored spawn-definition edits and stated failure modes.
- [FluffyQuack tools page](https://www.fluffyquack.com/): ARCTool role and official download page.

## Next research steps

1. Build the upstream proxy as an isolated reference build and inspect its x86 exports.
2. Download ARCTool only from FluffyQuack's official tools page; validate a copied ARC extraction/repack and contained file variants without writing into the game folder.
3. Use the installed executable's actual imports and verified RE5Patch signatures as constrained entry points for the first runtime session.
4. Keep player, enemy and event capabilities unknown until a controlled game test records evidence.
