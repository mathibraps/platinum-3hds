#!/usr/bin/env python3
"""Converts screen dumps of the 3DS test mode (frameNNNN_*.raw: "8888" or
"5551", width, height, RGBA8 (0xRRGGBBAA) or RGBA5551 pixels) into PNGs like
the desktop harness writes.
usage: raw_to_png.py <dir with .raw files> <out dir>"""
import os
import struct
import sys
import zlib


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)


src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
for name in sorted(os.listdir(src)):
    if not name.endswith(".raw"):
        continue
    data = open(os.path.join(src, name), "rb").read()
    magic, w, h = data[:4], *struct.unpack("<HH", data[4:8])
    raw = bytearray()
    if magic == b"8888":
        px = struct.unpack(f"<{w * h}I", data[8:8 + w * h * 4])
        for y in range(h):
            raw.append(0)
            for x in range(w):
                c = px[y * w + x]
                raw += bytes((c >> 24, (c >> 16) & 0xff, (c >> 8) & 0xff, 255))
    elif magic == b"5551":
        px = struct.unpack(f"<{w * h}H", data[8:8 + w * h * 2])
        for y in range(h):
            raw.append(0)
            for x in range(w):
                c = px[y * w + x]
                r, g, b = c >> 11, (c >> 6) & 31, (c >> 1) & 31
                raw += bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255))
    else:
        continue
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    open(os.path.join(dst, name[:-4] + ".png"), "wb").write(png)
