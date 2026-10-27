#!/usr/bin/env python3
"""Read-only static helpers for the installed re5dx9.exe (x86).

Subcommands:
  dis  <rva> [-n COUNT] [-b BACK]   disassemble COUNT instructions at RVA
  func <rva> [-n MAX]               disassemble from the guessed function start
  xref <rva>                        call/jmp rel32 and absolute-VA references
  disp <value> [--write]            instructions using memory displacement VALUE
  vt   <rva> [-n COUNT]             dump COUNT dwords at RVA as code pointers

Addresses are RVAs relative to image base 0x400000 unless prefixed with
"va:". The executable is only read; nothing is patched.
"""

from __future__ import annotations

import argparse
import pickle
import struct
import sys
from pathlib import Path

import capstone
import pefile

DEFAULT_EXE = Path(r"G:\SteamLibrary\steamapps\common\Resident Evil 5\re5dx9.exe")
# The shipped exe is packed; analysis uses the unpacked image written by
# scripts/dump_image.py (flat, file offset == RVA).
DEFAULT_DUMP = Path(__file__).resolve().parent.parent / "build" / "dump" / "re5dx9.flat"
CACHE = Path(__file__).resolve().parent.parent / "build" / "re5dis-cache.pkl"


class Image:
    def __init__(self, path: Path, dump: Path | None) -> None:
        self.pe = pefile.PE(str(path), fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        if dump is not None and dump.exists():
            self.mapped = dump.read_bytes()
        else:
            self.mapped = self.pe.get_memory_mapped_image()
        # The first section holds the code (section names are stripped).
        text = self.pe.sections[0]
        self.text_rva = text.VirtualAddress
        self.text_end = text.VirtualAddress + text.Misc_VirtualSize
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True

    def bytes_at(self, rva: int, size: int) -> bytes:
        return self.mapped[rva : rva + size]

    def disasm(self, rva: int, count: int):
        code = self.bytes_at(rva, count * 15)
        return list(self.md.disasm(code, self.base + rva, count))

    def u32(self, rva: int) -> int:
        return struct.unpack_from("<I", self.mapped, rva)[0]


def parse_addr(text: str, image: Image) -> int:
    if text.lower().startswith("va:"):
        return int(text[3:], 16) - image.base
    return int(text, 16)


def fmt(image: Image, insn, marker: int | None = None) -> str:
    rva = insn.address - image.base
    tag = ">>" if marker is not None and rva == marker else "  "
    raw = insn.bytes.hex(" ").upper()
    return f"{tag} {rva:08X}  {raw:<30} {insn.mnemonic} {insn.op_str}"


def guess_function_start(image: Image, rva: int) -> int:
    """Walk back to the byte after CC/90 padding that precedes a prologue."""
    cursor = rva
    limit = max(image.text_rva, rva - 0x4000)
    while cursor > limit:
        prev = image.mapped[cursor - 1]
        if prev in (0xCC,) and image.mapped[cursor - 2] in (0xCC, 0xC3, 0xC2):
            return cursor
        if prev == 0xCC and image.mapped[cursor] in (0x55, 0x83, 0x81, 0x53, 0x56, 0x57, 0x8B, 0x6A, 0x51, 0xA1, 0x64):
            return cursor
        cursor -= 1
    return rva


def cmd_dis(image: Image, args) -> None:
    rva = parse_addr(args.rva, image)
    start = rva
    if args.back:
        # Try back offsets until the disassembly resynchronises on rva.
        for back in range(args.back, 0, -1):
            insns = image.disasm(rva - back, args.back + 4)
            if any(i.address - image.base == rva for i in insns):
                start = rva - back
                break
    for insn in image.disasm(start, args.n + (args.back // 2 if args.back else 0)):
        print(fmt(image, insn, rva))


def cmd_func(image: Image, args) -> None:
    rva = parse_addr(args.rva, image)
    start = guess_function_start(image, rva)
    print(f"; guessed function start {start:08X} (VA {image.base + start:08X}) for {rva:08X}")
    shown = 0
    cursor = start
    while shown < args.n:
        insns = image.disasm(cursor, 64)
        if not insns:
            break
        for insn in insns:
            print(fmt(image, insn, rva))
            shown += 1
            cursor = insn.address - image.base + insn.size
            if insn.mnemonic in ("ret", "retn") and cursor > rva:
                return
            if shown >= args.n:
                return


def linear_sweep(image: Image):
    """Cache (rva, size, mnemonic, op_str, disp, target) for the whole .text."""
    if CACHE.exists():
        with CACHE.open("rb") as handle:
            cached = pickle.load(handle)
        if cached.get("size") == len(image.mapped):
            return cached["rows"]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    md.skipdata = True
    rows = []
    code = image.bytes_at(image.text_rva, image.text_end - image.text_rva)
    for insn in md.disasm(code, image.base + image.text_rva):
        disp = None
        target = None
        try:
            for op in insn.operands:
                if op.type == capstone.x86.X86_OP_MEM:
                    disp = op.mem.disp & 0xFFFFFFFF
                elif op.type == capstone.x86.X86_OP_IMM:
                    target = op.imm & 0xFFFFFFFF
        except capstone.CsError:
            pass
        rows.append((insn.address - image.base, insn.size, insn.mnemonic, insn.op_str, disp, target))
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    with CACHE.open("wb") as handle:
        pickle.dump({"size": len(image.mapped), "rows": rows}, handle)
    return rows


def cmd_xref(image: Image, args) -> None:
    rva = parse_addr(args.rva, image)
    va = image.base + rva
    for row_rva, _size, mnem, ops, disp, target in linear_sweep(image):
        if target == va or disp == va:
            print(f"   {row_rva:08X}  {mnem} {ops}")


def cmd_disp(image: Image, args) -> None:
    value = int(args.value, 16)
    writes = ("mov", "sub", "add", "and", "or", "xor", "inc", "dec", "movss", "movsd", "fstp", "fistp", "neg")
    for row_rva, _size, mnem, ops, disp, _target in linear_sweep(image):
        if disp != value:
            continue
        if args.write:
            first = ops.split(",", 1)[0]
            if "ptr" not in first or not mnem.startswith(writes):
                continue
        print(f"   {row_rva:08X}  {mnem} {ops}")


def cmd_vt(image: Image, args) -> None:
    rva = parse_addr(args.rva, image)
    for index in range(args.n):
        value = image.u32(rva + index * 4)
        target = value - image.base
        mark = "code" if image.text_rva <= target < image.text_end else "----"
        print(f"   [{index:3d}] +{index * 4:03X}  {value:08X}  rva {target:08X}  {mark}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--dump", type=Path, default=DEFAULT_DUMP)
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("dis"); p.add_argument("rva"); p.add_argument("-n", type=int, default=40); p.add_argument("-b", "--back", type=int, default=0)
    p = sub.add_parser("func"); p.add_argument("rva"); p.add_argument("-n", type=int, default=400)
    p = sub.add_parser("xref"); p.add_argument("rva")
    p = sub.add_parser("disp"); p.add_argument("value"); p.add_argument("--write", action="store_true")
    p = sub.add_parser("vt"); p.add_argument("rva"); p.add_argument("-n", type=int, default=16)
    args = parser.parse_args()
    image = Image(args.exe, args.dump)
    {"dis": cmd_dis, "func": cmd_func, "xref": cmd_xref, "disp": cmd_disp, "vt": cmd_vt}[args.cmd](image, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
