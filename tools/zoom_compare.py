#!/usr/bin/env python3
"""Crops the same region of several snapshots, enlarges it and puts the crops
side by side (for comparing rendering modes).

usage: zoom_compare.py <out.png> <x0> <y0> <x1> <y1> <scale> <a.png> [b.png ...]"""
import struct
import sys
import zlib

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from png_diff import read_png  # noqa: E402


def crop(path, x0, y0, x1, y1, sc):
    w, h, bpp, rows = read_png(path)
    out = []
    for y in range(y0, y1):
        line = b"".join(bytes(rows[y][x * bpp:x * bpp + 3]) * sc for x in range(x0, x1))
        out += [line] * sc
    return out


def main():
    out_path = sys.argv[1]
    x0, y0, x1, y1, sc = (int(v) for v in sys.argv[2:7])
    crops = [crop(p, x0, y0, x1, y1, sc) for p in sys.argv[7:]]
    gap = b"\x50\x50\x50" * 8
    rows = [gap.join(parts) for parts in zip(*crops)]
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)

    w = len(rows[0]) // 3
    with open(out_path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, len(rows), 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    main()
