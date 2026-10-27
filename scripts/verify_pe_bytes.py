#!/usr/bin/env python3
"""Compare expected instruction bytes at PE RVAs without modifying the file."""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def parse_pe(data: bytes) -> tuple[int, int, list[tuple[str, int, int, int, int]]]:
    if data[:2] != b"MZ":
        raise ValueError("missing MZ header")
    nt_offset = u32(data, 0x3C)
    if data[nt_offset : nt_offset + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")

    coff = nt_offset + 4
    section_count = u16(data, coff + 2)
    optional_size = u16(data, coff + 16)
    optional = coff + 20
    magic = u16(data, optional)
    if magic == 0x10B:
        image_base = u32(data, optional + 28)
    elif magic == 0x20B:
        image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    else:
        raise ValueError(f"unsupported PE optional-header magic 0x{magic:04X}")

    sections = []
    table = optional + optional_size
    for index in range(section_count):
        offset = table + index * 40
        name = data[offset : offset + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<IIII", data, offset + 8)
        sections.append((name, virtual_size, rva, raw_size, raw_offset))
    return image_base, magic, sections


def rva_bytes(data: bytes, sections: list[tuple[str, int, int, int, int]], rva: int, length: int) -> tuple[str, int, bytes]:
    for name, virtual_size, section_rva, raw_size, raw_offset in sections:
        span = max(virtual_size, raw_size)
        if section_rva <= rva and rva + length <= section_rva + span:
            file_offset = raw_offset + (rva - section_rva)
            if rva - section_rva + length > raw_size or file_offset + length > len(data):
                raise ValueError(f"RVA 0x{rva:X} is in zero-filled section data")
            return name, file_offset, data[file_offset : file_offset + length]
    raise ValueError(f"RVA 0x{rva:X} is not in a section")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pe", type=Path, help="PE executable to inspect (read-only)")
    parser.add_argument(
        "--check",
        action="append",
        default=[],
        metavar="RVA=BYTE BYTE ...",
        help="compare bytes at an RVA, e.g. --check '0x77CB40=66 29 8E 64 13 00 00'",
    )
    args = parser.parse_args()

    try:
        data = args.pe.read_bytes()
        image_base, magic, sections = parse_pe(data)
        print(f"PE={args.pe.resolve()}")
        print(f"IMAGE_BASE=0x{image_base:X} FORMAT={'PE32' if magic == 0x10B else 'PE32+'}")
        failures = 0
        for check in args.check:
            rva_text, expected_text = check.split("=", 1)
            rva = int(rva_text, 0)
            expected = bytes(int(part, 16) for part in expected_text.replace(",", " ").split())
            section, file_offset, actual = rva_bytes(data, sections, rva, len(expected))
            matches = actual == expected
            print(
                f"RVA=0x{rva:08X} FILE=0x{file_offset:08X} SECTION={section} "
                f"EXPECTED={expected.hex(' ').upper()} ACTUAL={actual.hex(' ').upper()} "
                f"{'MATCH' if matches else 'MISMATCH'}"
            )
            failures += not matches
        return 1 if failures else 0
    except (OSError, ValueError, struct.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
