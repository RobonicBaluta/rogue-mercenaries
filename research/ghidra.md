# Ghidra setup for the RE5 research

Static analysis of the unpacked image with Ghidra 12.2 (needs JDK 25). Installed on
2026-10-27 at the user's request so the menu-flow script, the Mercenaries HUD and the
inventory can be mapped with a decompiler instead of capstone sweeps.

| Piece | Where |
|---|---|
| Ghidra 12.2 | `G:\tools\ghidra-dl\ghidra_12.2_PUBLIC` (official GitHub release zip) |
| JDK 25 (Temurin, winget) | `C:\Program Files\Eclipse Adoptium\jdk-25.0.5.8-hotspot` |
| Project | `G:\tools\ghidra-proj\re5` with program `re5dx9_mem.exe` |
| Program file | `build/dump/re5dx9_mem.exe`: the runtime dump (`build/dump/re5dx9.flat`) rewritten as a 1:1 mapped PE by `scripts/dump_to_pe.py` (section raw pointers = virtual addresses), so every address matches the live game |
| Analysis | `GHIDRA_HEADLESS_MAXMEM=12G support/analyzeHeadless.bat G:/tools/ghidra-proj re5 -import build/dump/re5dx9_mem.exe -analysisTimeoutPerFile 7200` (log in `G:\tools\ghidra-proj\headless.log`). The default 2 GB heap dies with `OutOfMemoryError` in the operand-reference analyzer on this 25 MB image |
| Queries | `python scripts/ghidra_query.py count | decompile 0x... | callers | callees | xrefs | disasm | seed FILE` (PyGhidra from PyPI, `pip install pyghidra`) |
| MCP server | `scripts/ghidra_mcp.py`, registered in `.mcp.json` as `ghidra` (tools: decompile, disassemble, xrefs_to, callers, callees, function_info, read_bytes, search_functions, rename_function). Claude Code loads it on the next session start |

Notes:
- The imports table is already resolved in the dump, so API calls show as calls through the
  IAT addresses; names can be recovered later from the original PE's import directory.
- The packer's sections are unnamed; the code lives in the first section (`0x401000`..`0xF4A000`).
- Open questions this tooling is for: the title-flow script that creates `uIdStartUp20..22`
  and starts the match (`0x00BF7773`, `0x00BF78B0`, `0x00BF7AA0`, `0x00BF808E`, `0x00BF9360`,
  step functions `0x00BF29C0`/`0x00BF2BD0`/`0x00BF2CF0`), the countdown digits' draw, and the
  player inventory table.
