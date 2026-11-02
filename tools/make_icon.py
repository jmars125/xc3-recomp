"""Draw runtime/xc3.ico: an original mark for XC3 Recomp (no game artwork), a gold 3 on a dark rounded square.

    .venv\\Scripts\\python tools\\make_icon.py

Sizes 16, 24, 32, 48, 64 and 256 as 32-bit BMP entries, drawn 4x4 supersampled with numpy (no imaging library).
"""
import struct
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
SIZES = (16, 24, 32, 48, 64, 256)
BG_TOP, BG_BOTTOM = np.array([30, 28, 62]), np.array([10, 9, 24])
CYAN_HI, CYAN_LO = np.array([255, 232, 170]), np.array([240, 130, 50])
EDGE = np.array([96, 40, 24])
THREE = ((0.28, 0.22), (0.70, 0.22), (0.47, 0.47), (0.64, 0.50), (0.72, 0.62), (0.64, 0.75), (0.46, 0.80), (0.28, 0.72))


def seg_dist(px, py, ax, ay, bx, by):
    """Distance from each point to the segment a-b."""
    dx, dy = bx - ax, by - ay
    t = np.clip(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0, 1)
    return np.hypot(px - (ax + t * dx), py - (ay + t * dy))


def draw(n):
    ss = 4
    m = n * ss
    y, x = np.mgrid[0:m, 0:m] / m + 0.5 / m   # 0..1, y down
    # rounded square
    r = 0.18
    qx, qy = np.abs(x - 0.5) - (0.5 - r), np.abs(y - 0.5) - (0.5 - r)
    box = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r
    inside = box <= -0.004
    # the 3: one angular stroke, top bar, diagonal to the middle, then down around the bowl
    d = np.min([seg_dist(x, y, *p, *q) for p, q in zip(THREE, THREE[1:])], axis=0)
    heavy = light = d < 0.085
    edge = d < 0.085 + 0.03
    t = y[..., None]
    rgb = BG_TOP * (1 - t) + BG_BOTTOM * t
    rgb = np.where(edge[..., None], EDGE, rgb)
    cy = CYAN_HI * (1 - t) + CYAN_LO * t
    rgb = np.where((heavy | light)[..., None], cy, rgb)
    a = inside.astype(float)
    rgb = rgb * a[..., None]   # premultiplied for the downsample
    rgb = rgb.reshape(n, ss, n, ss, 3).mean(axis=(1, 3))
    a = a.reshape(n, ss, n, ss).mean(axis=(1, 3))
    rgb = np.where(a[..., None] > 0, rgb / np.maximum(a, 1e-6)[..., None], 0)
    return np.dstack([rgb, a * 255]).round().clip(0, 255).astype(np.uint8)


def bmp_entry(img):
    n = img.shape[0]
    bgra = img[::-1][..., [2, 1, 0, 3]].tobytes()   # bottom-up rows, BGRA
    mask_row = ((n + 31) // 32) * 4
    header = struct.pack('<IiiHHIIiiII', 40, n, 2 * n, 1, 32, 0, len(bgra) + mask_row * n, 0, 0, 0, 0)
    return header + bgra + bytes(mask_row * n)   # AND mask all zero: the alpha channel decides


def main():
    entries = [bmp_entry(draw(n)) for n in SIZES]
    out = struct.pack('<HHH', 0, 1, len(SIZES))
    offset = 6 + 16 * len(SIZES)
    for n, e in zip(SIZES, entries):
        out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(e), offset)
        offset += len(e)
    out += b''.join(entries)
    (ROOT / 'runtime' / 'xc3.ico').write_bytes(out)
    print(f'runtime/xc3.ico: {len(SIZES)} sizes, {len(out)} bytes')


if __name__ == '__main__':
    main()
