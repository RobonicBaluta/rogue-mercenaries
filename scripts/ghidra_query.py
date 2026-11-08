#!/usr/bin/env python3
"""Command-line queries against the Ghidra project (same backend as ghidra_mcp.py).

  python scripts/ghidra_query.py count
  python scripts/ghidra_query.py decompile 0xbf2cf0 [max_lines]
  python scripts/ghidra_query.py callers 0x... | callees 0x... | xrefs 0x... | disasm 0x... [count] | words 0x... [count]
  python scripts/ghidra_query.py seed FILE      (one hex function start per line; creates missing functions)
"""
import os
import sys

os.environ.setdefault("GHIDRA_INSTALL_DIR", r"G:\tools\ghidra-dl\ghidra_12.2_PUBLIC")
os.environ.setdefault("JAVA_HOME", r"C:\Program Files\Eclipse Adoptium\jdk-25.0.5.8-hotspot")
PROJECT_DIR = os.environ.get("GHIDRA_PROJECT", r"G:\tools\ghidra-proj")
PROJECT_NAME = os.environ.get("GHIDRA_PROJECT_NAME", "re5")
BINARY = os.environ.get("GHIDRA_BINARY", r"G:\Projects\re5mod\build\dump\re5dx9_mem.exe")

import pyghidra  # noqa: E402

pyghidra.start()
cmd = sys.argv[1] if len(sys.argv) > 1 else "count"
args = sys.argv[2:]

with pyghidra.open_program(BINARY, project_location=PROJECT_DIR, project_name=PROJECT_NAME, analyze=False,
                           nested_project_location=False) as flat:
    program = flat.getCurrentProgram()
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import ConsoleTaskMonitor

    monitor = ConsoleTaskMonitor()
    fm = program.getFunctionManager()
    listing = program.getListing()
    refs = program.getReferenceManager()

    def addr(text):
        return program.getAddressFactory().getAddress(text)

    def fname(a):
        f = fm.getFunctionContaining(a)
        return f"{f.getName()}@{f.getEntryPoint()}" if f else "?"

    if cmd == "count":
        print("program", program.getName(), "functions", fm.getFunctionCount(), "instructions", listing.getNumInstructions())
    elif cmd == "decompile":
        # decompile ADDR... [max_lines]  (several functions in one JVM session)
        limit = 400
        targets = list(args)
        if len(targets) > 1 and targets[-1].isdigit():
            limit = int(targets.pop())
        d = DecompInterface()
        d.openProgram(program)
        for target in targets:
            f = fm.getFunctionContaining(addr(target))
            if f is None:
                print("// no function at", target)
                continue
            r = d.decompileFunction(f, 120, monitor)
            text = r.getDecompiledFunction().getC() if r.decompileCompleted() else "FAILED " + str(r.getErrorMessage())
            lines = text.splitlines()
            print(f"// {f.getName()} @ {f.getEntryPoint()} ({len(lines)} lines)")
            print("\n".join(lines[:limit]))
    elif cmd in ("callers", "callees"):
        f = fm.getFunctionContaining(addr(args[0]))
        fs = f.getCallingFunctions(monitor) if cmd == "callers" else f.getCalledFunctions(monitor)
        print("\n".join(sorted(f"{c.getName()}@{c.getEntryPoint()}" for c in fs)))
    elif cmd == "xrefs":
        for r in refs.getReferencesTo(addr(args[0])):
            print(r.getFromAddress(), fname(r.getFromAddress()), r.getReferenceType())
    elif cmd == "words":
        # "words 0x... [count]": dwords from that address (a vtable: each slot and the function it names).
        mem = program.getMemory()
        a = addr(args[0])
        n = int(args[1]) if len(args) > 1 else 16
        for i in range(n):
            at = a.add(i * 4)
            v = mem.getInt(at) & 0xFFFFFFFF
            target = program.getAddressFactory().getDefaultAddressSpace().getAddress(v)
            f = fm.getFunctionAt(target)
            print(f"{at} +{i * 4:03X} {v:08X} {f.getName() if f else ''}")
    elif cmd == "disasm":
        ins = listing.getInstructionAt(addr(args[0])) or listing.getInstructionAfter(addr(args[0]))
        n = int(args[1]) if len(args) > 1 else 40
        while ins is not None and n > 0:
            print(ins.getAddress(), ins)
            ins = ins.getNext()
            n -= 1
    elif cmd == "seed":
        from ghidra.app.cmd.function import CreateFunctionCmd

        made = 0
        tx = program.startTransaction("seed functions")
        try:
            for line in open(args[0]):
                line = line.strip()
                if not line:
                    continue
                a = addr(line)
                if fm.getFunctionAt(a) is None and CreateFunctionCmd(a).applyTo(program, monitor):
                    made += 1
        finally:
            program.endTransaction(tx, True)
        print("created", made, "functions; total", fm.getFunctionCount())
        # pyghidra's context saves the program on exit.
