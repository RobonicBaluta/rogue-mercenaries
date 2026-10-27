#!/usr/bin/env python3
"""Ghidra MCP server for the RE5 research project (stdio transport).

Opens the analyzed program from the Ghidra project through PyGhidra and exposes
decompiler, disassembly and cross-reference queries as MCP tools.

Environment: GHIDRA_INSTALL_DIR, JAVA_HOME, GHIDRA_PROJECT (directory),
GHIDRA_PROJECT_NAME (default re5), GHIDRA_BINARY (the imported file; used only
when the program is not in the project yet).
"""
import os
import sys

import pyghidra
from mcp.server.fastmcp import FastMCP

PROJECT_DIR = os.environ.get("GHIDRA_PROJECT", r"G:\tools\ghidra-proj")
PROJECT_NAME = os.environ.get("GHIDRA_PROJECT_NAME", "re5")
BINARY = os.environ.get("GHIDRA_BINARY", r"G:\Projects\re5mod\build\dump\re5dx9_mem.exe")

pyghidra.start()
_ctx = pyghidra.open_program(BINARY, project_location=PROJECT_DIR, project_name=PROJECT_NAME, analyze=False)
flat = _ctx.__enter__()
program = flat.getCurrentProgram()

from ghidra.app.decompiler import DecompInterface  # noqa: E402  (needs the JVM)
from ghidra.util.task import ConsoleTaskMonitor  # noqa: E402

monitor = ConsoleTaskMonitor()
decomp = DecompInterface()
decomp.openProgram(program)
listing = program.getListing()
refs = program.getReferenceManager()
fm = program.getFunctionManager()

mcp = FastMCP("ghidra")


def addr(text: str):
    a = program.getAddressFactory().getAddress(text if text.lower().startswith("0x") or ":" in text else "0x" + text)
    if a is None:
        raise ValueError(f"bad address {text!r}")
    return a


def func_name(a) -> str:
    f = fm.getFunctionContaining(a)
    return f"{f.getName()}@{f.getEntryPoint()}" if f else "?"


@mcp.tool()
def decompile(address: str, timeout_seconds: int = 60) -> str:
    """Decompiled C of the function containing `address` (hex)."""
    f = fm.getFunctionContaining(addr(address))
    if f is None:
        return f"no function at {address}"
    r = decomp.decompileFunction(f, timeout_seconds, monitor)
    if not r.decompileCompleted():
        return f"decompile failed: {r.getErrorMessage()}"
    return f"// {f.getName()} @ {f.getEntryPoint()}\n" + r.getDecompiledFunction().getC()


@mcp.tool()
def disassemble(address: str, count: int = 40) -> str:
    """`count` instructions starting at `address`."""
    out = []
    ins = listing.getInstructionAt(addr(address)) or listing.getInstructionAfter(addr(address))
    while ins is not None and len(out) < count:
        out.append(f"{ins.getAddress()}  {ins}")
        ins = ins.getNext()
    return "\n".join(out) or "no code"


@mcp.tool()
def xrefs_to(address: str, limit: int = 60) -> str:
    """References to `address`, with the referencing function."""
    out = []
    for r in refs.getReferencesTo(addr(address)):
        out.append(f"{r.getFromAddress()} ({func_name(r.getFromAddress())}) {r.getReferenceType()}")
        if len(out) >= limit:
            break
    return "\n".join(out) or "no references"


@mcp.tool()
def callers(address: str, limit: int = 60) -> str:
    """Functions calling the function containing `address`."""
    f = fm.getFunctionContaining(addr(address))
    if f is None:
        return f"no function at {address}"
    names = sorted({f"{c.getName()}@{c.getEntryPoint()}" for c in f.getCallingFunctions(monitor)})
    return "\n".join(names[:limit]) or "no callers"


@mcp.tool()
def callees(address: str, limit: int = 80) -> str:
    """Functions called by the function containing `address`."""
    f = fm.getFunctionContaining(addr(address))
    if f is None:
        return f"no function at {address}"
    names = sorted({f"{c.getName()}@{c.getEntryPoint()}" for c in f.getCalledFunctions(monitor)})
    return "\n".join(names[:limit]) or "no callees"


@mcp.tool()
def function_info(address: str) -> str:
    """Name, entry, size and signature of the function containing `address`."""
    f = fm.getFunctionContaining(addr(address))
    if f is None:
        return f"no function at {address}"
    body = f.getBody()
    return f"{f.getName()} @ {f.getEntryPoint()} size={body.getNumAddresses()} sig={f.getPrototypeString(True, True)}"


@mcp.tool()
def rename_function(address: str, name: str) -> str:
    """Names the function containing `address` (persists when the project is saved)."""
    f = fm.getFunctionContaining(addr(address))
    if f is None:
        return f"no function at {address}"
    tx = program.startTransaction("rename")
    try:
        f.setName(name, program.getSymbolTable().getSymbol(f.getEntryPoint().toString()).getSource() if False else __import__("ghidra.program.model.symbol", fromlist=["SourceType"]).SourceType.USER_DEFINED)
    finally:
        program.endTransaction(tx, True)
    return f"renamed {f.getEntryPoint()} -> {name}"


@mcp.tool()
def read_bytes(address: str, count: int = 64) -> str:
    """Hex bytes at `address`."""
    data = flat.getBytes(addr(address), min(count, 4096))
    return " ".join(f"{b & 0xFF:02X}" for b in data)


@mcp.tool()
def search_functions(name_contains: str, limit: int = 50) -> str:
    """Functions whose name contains the text."""
    out = []
    for f in fm.getFunctions(True):
        if name_contains.lower() in f.getName().lower():
            out.append(f"{f.getName()}@{f.getEntryPoint()}")
            if len(out) >= limit:
                break
    return "\n".join(out) or "no match"


if __name__ == "__main__":
    print(f"ghidra mcp: {program.getName()} ({fm.getFunctionCount()} functions)", file=sys.stderr)
    mcp.run()
