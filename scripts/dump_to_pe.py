#!/usr/bin/env python3
"""Turn the unpacked memory dump into a loadable PE for Ghidra.

The dump is the process image from the image base, so every section already sits
at its virtual address. Rewriting each section header's raw pointer/size to its
virtual values makes the PE loader map the file 1:1 (the usual dump fix-up).
"""
import struct
import sys
from pathlib import Path

src = Path(sys.argv[1])
dst = Path(sys.argv[2])
data = bytearray(src.read_bytes())
if data[:2] != b"MZ":
    sys.exit("dump does not start with an MZ header")
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
    sys.exit("no PE signature")
num_sections = struct.unpack_from("<H", data, e_lfanew + 6)[0]
opt_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
opt = e_lfanew + 24
size_of_image = struct.unpack_from("<I", data, opt + 56)[0]
sect = opt + opt_size
print(f"sections={num_sections} size_of_image=0x{size_of_image:X} dump=0x{len(data):X}")
for i in range(num_sections):
    o = sect + 40 * i
    name = bytes(data[o:o + 8]).rstrip(b"\0").decode("latin1", "replace")
    vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", data, o + 8)
    chars = struct.unpack_from("<I", data, o + 36)[0]
    struct.pack_into("<IIII", data, o + 8, vsize, vaddr, vsize, vaddr)
    print(f"  {name:8s} va=0x{vaddr:08X} vsize=0x{vsize:08X} (raw was 0x{rptr:08X}/0x{rsize:08X}) chars=0x{chars:08X}")
# Section alignment must accept the virtual layout; keep file alignment = section alignment.
sect_align = struct.unpack_from("<I", data, opt + 32)[0]
struct.pack_into("<I", data, opt + 36, sect_align)
dst.write_bytes(bytes(data[:size_of_image]))
print("wrote", dst, "bytes", size_of_image)
