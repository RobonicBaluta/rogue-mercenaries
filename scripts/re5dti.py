#!/usr/bin/env python3
"""List MT Framework DTI (runtime type info) classes from the unpacked dump.

DTI layout observed in this build: +0 vtable, +4 name, +8 next sibling,
+0xC first child, +0x10 parent, +0x1C size/flags, +0x20 CRC.
"""
import argparse, struct
from pathlib import Path

B = 0x400000
DUMP = Path(__file__).resolve().parent.parent / "build" / "dump" / "re5dx9.flat"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root_va", help="DTI VA to start from (hex)")
    ap.add_argument("--depth", type=int, default=99)
    ap.add_argument("--up", action="store_true", help="print the parent chain instead")
    a = ap.parse_args()
    data = DUMP.read_bytes()
    u32 = lambda va: struct.unpack_from("<I", data, va - B)[0]
    def name(dti):
        r = u32(dti + 4) - B
        return data[r:data.find(b"\0", r, r + 96)].decode("latin1")
    root = int(a.root_va, 16)
    if a.up:
        d = root
        while d and B <= d < B + len(data):
            print(f"{d:08X} {name(d)} size=0x{u32(d + 0x1C) & 0xFFFFFF:X}")
            d = u32(d + 0x10)
        return
    def walk(dti, depth):
        child = u32(dti + 0xC)
        while child and depth < a.depth:
            print(f"{'  ' * depth}{child:08X} {name(child)} size=0x{u32(child + 0x1C) & 0xFFFFFF:X} crc={u32(child + 0x20):08X}")
            walk(child, depth + 1)
            child = u32(child + 8)
    print(f"{root:08X} {name(root)}")
    walk(root, 1)

main()
