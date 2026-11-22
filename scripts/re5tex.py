#!/usr/bin/env python3
"""RE5 (MT Framework 1) .tex textures <-> PNG, for the mod's own replacement textures.

  python scripts/re5tex.py topng TEX PNG          decode (DXT1, DXT5, or 32-bit)
  python scripts/re5tex.py frompng PNG TEMPLATE OUT   encode PNG as TEMPLATE's format and size

A .tex here: "TEX\\0", u16 version 0x70, ..., u16 width at +0x0C, u16 height at +0x0E, the
format's FourCC at +0x14, four floats, the pixels at +0x2C (one mip level, one face: the UI's
*_NOMIP_* textures). Encoding keeps the template's 0x2C-byte header and writes only the first
level, so the template must be a single-level texture.
"""
import struct
import sys

import numpy as np
from PIL import Image

HEADER = 0x2C


def read_header(data):
    assert data[:4] == b'TEX\0', 'not a TEX file'
    width, height = struct.unpack_from('<HH', data, 0x0C)
    fourcc = data[0x14:0x18]
    return width, height, fourcc


def unpack565(c):
    r = ((c >> 11) & 31) * 255 // 31
    g = ((c >> 5) & 63) * 255 // 63
    b = (c & 31) * 255 // 31
    return np.stack([r, g, b], axis=-1).astype(np.int32)


def decode_colour(blocks):
    """blocks: (n, 8) uint8 DXT colour blocks -> (n, 16, 3)."""
    c0 = blocks[:, 0].astype(np.int32) | (blocks[:, 1].astype(np.int32) << 8)
    c1 = blocks[:, 2].astype(np.int32) | (blocks[:, 3].astype(np.int32) << 8)
    p0, p1 = unpack565(c0), unpack565(c1)
    four = (c0 > c1)[:, None]
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    palette = np.stack([p0, p1, p2, p3], axis=1)  # (n, 4, 3)
    bits = blocks[:, 4:8].astype(np.uint32)
    word = bits[:, 0] | (bits[:, 1] << 8) | (bits[:, 2] << 16) | (bits[:, 3] << 24)
    idx = np.stack([(word >> (2 * i)) & 3 for i in range(16)], axis=1).astype(np.int64)
    return np.take_along_axis(palette, idx[:, :, None].repeat(3, axis=2), axis=1), (c0 > c1)


def decode_alpha(blocks):
    """blocks: (n, 8) uint8 DXT5 alpha blocks -> (n, 16)."""
    a0 = blocks[:, 0].astype(np.int32)
    a1 = blocks[:, 1].astype(np.int32)
    eight = (a0 > a1)[:, None]
    steps = []
    for i in range(8):
        if i == 0:
            v = a0
        elif i == 1:
            v = a1
        else:
            k = i - 1
            v8 = ((8 - 1 - k) * a0 + k * a1) // 7
            if i < 6:
                k6 = i - 1
                v6 = ((6 - 1 - k6) * a0 + k6 * a1) // 5
            else:
                v6 = np.zeros_like(a0) if i == 6 else np.full_like(a0, 255)
            v = np.where(eight[:, 0], v8, v6)
        steps.append(v)
    palette = np.stack(steps, axis=1)
    bits = np.zeros(blocks.shape[0], dtype=np.uint64)
    for i in range(6):
        bits |= blocks[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    idx = np.stack([(bits >> np.uint64(3 * i)) & np.uint64(7) for i in range(16)], axis=1).astype(np.int64)
    return np.take_along_axis(palette, idx, axis=1)


def to_rgba(data):
    width, height, fourcc = read_header(data)
    body = np.frombuffer(data, dtype=np.uint8, offset=HEADER)
    bw, bh = (width + 3) // 4, (height + 3) // 4
    n = bw * bh
    if fourcc == b'DXT5':
        blocks = body[:n * 16].reshape(n, 16)
        alpha = decode_alpha(blocks[:, :8])
        colour, _ = decode_colour(blocks[:, 8:])
    elif fourcc == b'DXT1':
        blocks = body[:n * 8].reshape(n, 8)
        colour, four = decode_colour(blocks)
        alpha = np.full((n, 16), 255)
        bits = blocks[:, 4:8].astype(np.uint32)
        word = bits[:, 0] | (bits[:, 1] << 8) | (bits[:, 2] << 16) | (bits[:, 3] << 24)
        idx = np.stack([(word >> (2 * i)) & 3 for i in range(16)], axis=1)
        alpha[(~four)[:, None] & (idx == 3)] = 0
    else:
        raise SystemExit(f'format {fourcc!r} not handled')
    rgba = np.concatenate([colour, alpha[:, :, None]], axis=2).astype(np.uint8)  # (n, 16, 4)
    img = rgba.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:height, :width]


# ---- encoding (DXT5: a plain least-squares-free fit, good enough for UI text) ------------------

def pack565(rgb):
    r = (rgb[..., 0].astype(np.int32) * 31 + 127) // 255
    g = (rgb[..., 1].astype(np.int32) * 63 + 127) // 255
    b = (rgb[..., 2].astype(np.int32) * 31 + 127) // 255
    return (r << 11) | (g << 5) | b


def encode_dxt5(img):
    h, w = img.shape[:2]
    bw, bh = (w + 3) // 4, (h + 3) // 4
    pad = np.zeros((bh * 4, bw * 4, 4), dtype=np.uint8)
    pad[:h, :w] = img
    px = pad.reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * bw, 16, 4).astype(np.int32)
    n = px.shape[0]
    out = np.zeros((n, 16), dtype=np.uint8)
    # Alpha: a0 = max, a1 = min, eight-step ramp.
    a = px[:, :, 3]
    a0 = a.max(axis=1)
    a1 = a.min(axis=1)
    same = a0 == a1
    a0 = np.where(same & (a0 < 255), a0 + 1, a0)  # keep a0 > a1 (eight steps)
    a1 = np.where(same & (a0 == 255) & (a1 == 255), 254, a1)
    ramp = np.stack([a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)], axis=1)  # (n, 8)
    aidx = np.abs(a[:, :, None] - ramp[:, None, :]).argmin(axis=2).astype(np.uint64)
    out[:, 0] = a0
    out[:, 1] = a1
    bits = np.zeros(n, dtype=np.uint64)
    for i in range(16):
        bits |= aidx[:, i] << np.uint64(3 * i)
    for i in range(6):
        out[:, 2 + i] = ((bits >> np.uint64(8 * i)) & np.uint64(0xFF)).astype(np.uint8)
    # Colour: the endpoints are the brightest and darkest of the block's visible texels.
    rgb = px[:, :, :3]
    weight = (a > 8)
    luma = rgb[:, :, 0] * 3 + rgb[:, :, 1] * 6 + rgb[:, :, 2]
    lo_l = np.where(weight, luma, 1 << 30)
    hi_l = np.where(weight, luma, -1)
    any_visible = weight.any(axis=1)
    hi = np.where(any_visible, hi_l.argmax(axis=1), luma.argmax(axis=1))
    lo = np.where(any_visible, lo_l.argmin(axis=1), luma.argmin(axis=1))
    rows = np.arange(n)
    e0 = rgb[rows, hi]
    e1 = rgb[rows, lo]
    c0 = pack565(e0)
    c1 = pack565(e1)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    eq = c0 == c1
    p0, p1 = unpack565(c0), unpack565(c1)
    palette = np.stack([p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3], axis=1)  # (n, 4, 3)
    dist = ((rgb[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(axis=3)
    cidx = dist.argmin(axis=2).astype(np.uint32)
    cidx[eq] = 0
    word = np.zeros(n, dtype=np.uint32)
    for i in range(16):
        word |= cidx[:, i] << np.uint32(2 * i)
    out[:, 8] = c0 & 0xFF
    out[:, 9] = c0 >> 8
    out[:, 10] = c1 & 0xFF
    out[:, 11] = c1 >> 8
    for i in range(4):
        out[:, 12 + i] = (word >> np.uint32(8 * i)) & 0xFF
    return out.tobytes()


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == 'topng':
        data = open(sys.argv[2], 'rb').read()
        Image.fromarray(to_rgba(data), 'RGBA').save(sys.argv[3])
        w, h, f = read_header(data)
        print(f'{sys.argv[3]}: {w}x{h} {f.decode()}')
    elif len(sys.argv) >= 5 and sys.argv[1] == 'frompng':
        template = open(sys.argv[3], 'rb').read()
        w, h, f = read_header(template)
        assert f == b'DXT5', 'only DXT5 templates are encoded'
        img = np.array(Image.open(sys.argv[2]).convert('RGBA'))
        assert img.shape[1] == w and img.shape[0] == h, f'the PNG must be {w}x{h}'
        body = encode_dxt5(img)
        open(sys.argv[4], 'wb').write(template[:HEADER] + body + template[HEADER + len(body):])
        print(f'{sys.argv[4]}: {w}x{h} DXT5, {len(body)} bytes of pixels')
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == '__main__':
    main()
