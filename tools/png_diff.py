#!/usr/bin/env python3
"""Compares two PNG snapshots written by the harness (8-bit RGBA or RGB):
prints how many pixels differ and their bounding box. With --tol N, pixels
whose channels all differ by at most N (0-255) count as equal, and the
largest difference is reported too.
usage: png_diff.py [--tol N] a.png b.png"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    pos, idat, w, h, ctype = 8, b"", 0, 0, 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, _, ctype = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    rows, prev, stride = [], bytearray(w * bpp), w * bpp
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, bpp, rows


if __name__ == "__main__":
    args = sys.argv[1:]
    tol = 0
    if args[0] == "--tol":
        tol = int(args[1])
        args = args[2:]
    wa, ha, ba, ra = read_png(args[0])
    wb, hb, bb, rb = read_png(args[1])
    if (wa, ha) != (wb, hb):
        sys.exit(f"sizes differ: {wa}x{ha} vs {wb}x{hb}")
    diff, box, maxd = 0, [wa, ha, -1, -1], 0
    for y in range(ha):
        for x in range(wa):
            pa, pb = ra[y][x * ba:x * ba + 3], rb[y][x * bb:x * bb + 3]
            d = max(abs(pa[k] - pb[k]) for k in range(3))
            maxd = max(maxd, d)
            if d > tol:
                diff += 1
                box = [min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)]
    print(f"{diff} pixels differ" + (f", box x {box[0]}-{box[2]} y {box[1]}-{box[3]}, max channel difference {maxd}" if diff else ""))
