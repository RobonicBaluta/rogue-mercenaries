"""Reads RE5 rLayout (XFS v6) set data and prints the set infos of the given unit classes.

  python scripts/arc_extract.py <game>/nativePC_MT/Image/Archive/s400.arc 15302ef4 --out DIR
  python scripts/xfs_setinfo.py DIR uEm21 [uEm10 ...]

(research/re5-engine-map.md "Enemy look fields and set data"; read-only on the game's files.)

Layout: "XFS\\0", u16 version, u16 ?, u32 class count, u32 definition size, u32 offsets (from
0x10) to each class definition: u32 hash, u32 property count, 0x18-byte property records
(u32 name offset from 0x10, u8 type, u8 flags, u16 size, 16 bytes). Data at 0x10 + definition
size: an object is u16 (class index << 1 | 1), u16 ?, u32 size, then each property as u32 count
and that many values.
"""
import glob
import os
import struct
import sys

SIZES = {0x3: 1, 0x4: 1, 0x5: 2, 0x6: 4, 0x7: 8, 0x8: 1, 0x9: 2, 0xA: 4, 0xB: 8, 0xC: 4, 0xD: 8,
         0x14: 16, 0x15: 16, 0x10: 8, 0x13: 64, 0xF: 4}


def cstr(d, o):
    e = d.index(b'\0', o)
    return d[o:e].decode('latin1'), e + 1


def parse(path):
    return parse_bytes(open(path, 'rb').read())


def parse_bytes(d):
    """The set infos (mSetInfos) of one layout's bytes; an empty pointer (tag 0xFFFE) is None."""
    if d[:4] != b'XFS\0':
        return []
    ncls = struct.unpack_from('<I', d, 8)[0]
    defsize = struct.unpack_from('<I', d, 12)[0]
    classes = []
    for i in range(ncls):
        off = 0x10 + struct.unpack_from('<I', d, 0x10 + 4 * i)[0]
        h, n = struct.unpack_from('<II', d, off)
        props = []
        for k in range(n):
            p = off + 8 + 0x18 * k
            name_off, typ, flags, size = struct.unpack_from('<IBBH', d, p)
            name, _ = cstr(d, 0x10 + name_off)
            props.append((name, typ, flags, size))
        classes.append((h, props))
    out = []

    def read_obj(o):
        if struct.unpack_from('<H', d, o)[0] == 0xFFFE:  # an empty pointer: tag and a u16, no size
            return None, o + 4
        tag, _, size = struct.unpack_from('<HHI', d, o)
        ci = tag >> 1
        o += 8
        obj = {}
        for name, typ, flags, psize in classes[ci][1]:
            cnt = struct.unpack_from('<I', d, o)[0]
            o += 4
            vals = []
            for _ in range(cnt):
                if typ in (1, 2):
                    v, o = read_obj(o)
                elif typ == 0xE:
                    v, o = cstr(d, o)
                elif typ in SIZES:
                    raw = d[o:o + SIZES[typ]]
                    o += SIZES[typ]
                    if typ == 0xC:
                        v = struct.unpack('<f', raw)[0]
                    elif typ == 0x14:
                        v = tuple(round(x, 1) for x in struct.unpack('<4f', raw))
                    elif typ in (0x3, 0x4):
                        v = raw[0]
                    elif typ == 0x5:
                        v = struct.unpack('<H', raw)[0]
                    elif typ == 0x6:
                        v = struct.unpack('<I', raw)[0]
                    elif typ == 0xA:
                        v = struct.unpack('<i', raw)[0]
                    else:
                        v = raw.hex()
                else:
                    raise ValueError(f'type {typ:#x} of {name} at {o:#x}')
                vals.append(v)
            obj[name] = vals[0] if cnt == 1 else vals
        return obj, o

    root, _ = read_obj(0x10 + defsize)
    infos = (root or {}).get('mSetInfos', [])
    return infos if isinstance(infos, list) else [infos]


def main():
    wanted = set(sys.argv[2:])
    for path in sorted(glob.glob(os.path.join(sys.argv[1], '*.15302ef4'))):
        try:
            infos = parse(path)
        except Exception as e:  # noqa: BLE001 - a layout of another shape: skip it
            print(f'{os.path.basename(path)}: {e}')
            continue
        for si in infos:
            if not isinstance(si, dict):
                continue
            unit = si.get('mUnitClass')
            if unit in wanted:
                info = si.get('mpInfo') or {}
                keep = {k: info.get(k) for k in ('mModelType', 'mMontageNo', 'mMaskType', 'mAttribute', 'mArmId', 'mbArmed',
                                                  'mPlagaType', 'mIsActing', 'mIsExtra', 'mExtraNo', 'mLife', 'mSearchCircle')}
                print(f"{os.path.basename(path)}  {unit}  {keep}")


if __name__ == '__main__':
    main()
