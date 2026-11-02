#!/usr/bin/env python3
"""Lists or extracts resources of an MT Framework .arc (read-only on the game files).

  python scripts/arc_extract.py ARCHIVE [TYPE_HASH ...] [--out DIR]

Without type hashes it lists every entry (name, type hash, size). With hashes it writes those
entries, decompressed, to DIR (default build/extract) as <path with \\ -> _>.<hash>.
Types seen: 0x4ef19843 rNavigationMesh, 0x5f36b659 rAIWayPoint, 0x15302ef4 rLayout (set data),
0x65b275e5 rScenario, 0x66b45610 rAIFSM, 0x58a15856 rModel, 0x241f5deb rTexture.
"""
import argparse
import os
import struct
import zlib

parser = argparse.ArgumentParser()
parser.add_argument('archive')
parser.add_argument('types', nargs='*')
parser.add_argument('--out', default='build/extract')
args = parser.parse_args()

data = open(args.archive, 'rb').read()
assert data[:4] == b'ARC\0', 'not an ARC file'
count = struct.unpack('<H', data[6:8])[0]
wanted = {int(t, 16) for t in args.types}
os.makedirs(args.out, exist_ok=True)
offset = 8
for _ in range(count):
    name = data[offset:offset + 64].split(b'\0')[0].decode('latin1')
    kind, packed, size, start = struct.unpack('<IIII', data[offset + 64:offset + 80])
    offset += 80
    if not wanted:
        print(f'{kind:08x} {size & 0xFFFFFF:9d} {name}')
        continue
    if kind not in wanted:
        continue
    raw = data[start:start + packed]
    try:
        body = zlib.decompress(raw)
    except zlib.error:
        body = raw
    path = os.path.join(args.out, name.replace(chr(92), '_') + f'.{kind:08x}')
    open(path, 'wb').write(body)
    print(f'{path} {len(body)} bytes, header {body[:16].hex()}')
