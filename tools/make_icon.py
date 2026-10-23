"""Draws the Tetradkata++ icon (a spiral notebook with "++") and writes src/tetradkata.ico.

Pure Python, no dependencies: shapes are sampled 4x4 per pixel for anti-aliasing.
Usage: python tools/make_icon.py
"""
import math
import os
import struct

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]


def rounded_rect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def plus(x, y, cx, cy, half, bar):
    return (abs(x - cx) <= bar and abs(y - cy) <= half) or (abs(y - cy) <= bar and abs(x - cx) <= half)


def sample(x, y):
    """Colour (r, g, b, a) at unit coordinates, painter's order: later shapes win."""
    color = None
    if rounded_rect(x, y, 0.12, 0.05, 0.90, 0.95, 0.09):
        color = (0x1E, 0x7D, 0x5A, 255)  # green cover
    if rounded_rect(x, y, 0.22, 0.10, 0.85, 0.90, 0.04):
        color = (0xFF, 0xFF, 0xFF, 255)  # page
        for i in range(7):
            ly = 0.24 + i * 0.095
            if abs(y - ly) < 0.012 and 0.24 < x < 0.83:
                color = (0x8F, 0xB8, 0xE8, 255)  # ruled lines
        if abs(x - 0.33) < 0.012 and 0.12 < y < 0.88:
            color = (0xE5, 0x6B, 0x6B, 255)  # margin line
    for i in range(6):
        cy = 0.17 + i * 0.135
        d = math.hypot(x - 0.20, y - cy)
        if 0.030 <= d <= 0.058:
            color = (0x45, 0x4F, 0x5A, 255)  # spiral rings
    for cx in (0.56, 0.77):
        if plus(x, y, cx, 0.70, 0.125, 0.052):
            color = (0xFF, 0xFF, 0xFF, 255)  # white outline
        if plus(x, y, cx, 0.70, 0.095, 0.030):
            color = (0xFF, 0x8A, 0x10, 255)  # orange "++"
    return color


def render(size):
    n = 4
    rows = []
    for py in range(size):
        row = []
        for px in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(n):
                for sx in range(n):
                    c = sample((px + (sx + 0.5) / n) / size, (py + (sy + 0.5) / n) / size)
                    if c:
                        acc[0] += c[0]
                        acc[1] += c[1]
                        acc[2] += c[2]
                        acc[3] += 1
            if acc[3]:
                row.append((acc[0] // acc[3], acc[1] // acc[3], acc[2] // acc[3], 255 * acc[3] // (n * n)))
            else:
                row.append((0, 0, 0, 0))
        rows.append(row)
    return rows


def bmp_entry(size, rows):
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = bytearray()
    for row in reversed(rows):
        for r, g, b, a in row:
            pixels += bytes((b, g, r, a))
    mask_stride = ((size + 31) // 32) * 4
    mask = bytes(mask_stride * size)
    return header + bytes(pixels) + mask


def main():
    images = [bmp_entry(s, render(s)) for s in SIZES]
    out = bytearray(struct.pack("<HHH", 0, 1, len(SIZES)))
    offset = 6 + 16 * len(SIZES)
    for s, data in zip(SIZES, images):
        dim = 0 if s >= 256 else s
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for data in images:
        out += data
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "tetradkata.ico")
    with open(path, "wb") as f:
        f.write(out)
    print("wrote", os.path.normpath(path), len(out), "bytes")


if __name__ == "__main__":
    main()
