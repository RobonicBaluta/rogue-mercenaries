#!/usr/bin/env python3
"""Read-only dump of the running re5dx9.exe image as a flat RVA-indexed file.

The shipped executable is packed (code sections decompress at runtime), so
static analysis has to work from the unpacked in-memory image. Unreadable
pages are zero-filled. Output is local research data and must not be shared.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import sys
from pathlib import Path

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
LIST_MODULES_32BIT = 0x01

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wt.DWORD), ("EntryPoint", ctypes.c_void_p)]


def find_pid(name: str) -> int:
    pids = (wt.DWORD * 4096)()
    needed = wt.DWORD()
    psapi.EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(needed))
    for pid in pids[: needed.value // 4]:
        handle = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not handle:
            continue
        try:
            buf = ctypes.create_unicode_buffer(260)
            if psapi.GetModuleBaseNameW(handle, None, buf, 260) and buf.value.lower() == name.lower():
                return pid
        finally:
            k32.CloseHandle(handle)
    raise SystemExit(f"{name} is not running")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parent.parent / "build" / "dump" / "re5dx9.flat")
    args = parser.parse_args()

    pid = find_pid("re5dx9.exe")
    handle = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    modules = (ctypes.c_void_p * 1024)()
    needed = wt.DWORD()
    if not psapi.EnumProcessModulesEx(handle, modules, ctypes.sizeof(modules), ctypes.byref(needed), LIST_MODULES_32BIT):
        raise SystemExit(f"EnumProcessModulesEx failed {ctypes.get_last_error()}")
    base = None
    size = 0
    for module in modules[: needed.value // ctypes.sizeof(ctypes.c_void_p)]:
        buf = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(handle, ctypes.c_void_p(module), buf, 260)
        if buf.value.lower() == "re5dx9.exe":
            info = MODULEINFO()
            psapi.GetModuleInformation(handle, ctypes.c_void_p(module), ctypes.byref(info), ctypes.sizeof(info))
            base, size = info.lpBaseOfDll, info.SizeOfImage
    if base is None:
        raise SystemExit("re5dx9.exe module not found")

    image = bytearray(size)
    page = 0x1000
    unreadable = 0
    read = ctypes.c_size_t()
    chunk = (ctypes.c_char * page)()
    for offset in range(0, size, page):
        if k32.ReadProcessMemory(handle, ctypes.c_void_p(base + offset), chunk, page, ctypes.byref(read)) and read.value == page:
            image[offset : offset + page] = bytes(chunk)
        else:
            unreadable += 1
    k32.CloseHandle(handle)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(image)
    print(f"pid={pid} base=0x{base:08X} size=0x{size:X} unreadable_pages={unreadable}")
    print(f"out={args.out} sha256={hashlib.sha256(image).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
