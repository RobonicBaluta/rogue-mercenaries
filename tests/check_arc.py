"""Reads an archive the effects tests wrote (tests/test_effects.cpp) with Python's own zlib, as the
game's loader would: every entry's stream inflates to its size, the first one's data at 0x8000."""
import struct
import sys
import zlib

path = sys.argv[1]
data = open(path, 'rb').read()
assert data[:4] == b'ARC\0', 'not an archive'
version, count = struct.unpack('<HH', data[4:8])
assert version == 7, version
assert count == 3, count
offset = 8
for i in range(count):
    name = data[offset:offset + 64].split(b'\0')[0].decode('latin1')
    kind, packed, size, start = struct.unpack('<IIII', data[offset + 64:offset + 80])
    offset += 80
    assert size & 0xFF000000 == 0x40000000, hex(size)
    body = zlib.decompress(data[start:start + packed])
    assert len(body) == size & 0xFFFFFF, (name, len(body), size)
    if i == 0:
        assert start == 0x8000, hex(start)
    print(f'{name}: {kind:08x}, {len(body)} bytes')
print('archive ok')
