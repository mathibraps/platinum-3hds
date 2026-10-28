#!/usr/bin/env python3
"""Converts a 3D frame dump (u16 width, height, RGB555 pixels with bit 15 =
no pixel, then optionally 5-bit alpha bytes) from the GPU 3D debug build into
a PNG (no pixel = magenta). With a third argument, also writes the alpha
plane as a grey image.
usage: bin555_to_png.py <in.bin> <out.png> [alpha.png]"""
import struct
import sys
import zlib

data = open(sys.argv[1], "rb").read()
w, h = struct.unpack("<HH", data[:4])
px = struct.unpack(f"<{w * h}H", data[4:4 + w * h * 2])
raw = bytearray()
for y in range(h):
    raw.append(0)
    for x in range(w):
        c = px[y * w + x]
        if c & 0x8000:
            raw += b"\xff\x00\xff"
        else:
            r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
            raw += bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)))


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)


if len(sys.argv) > 3 and len(data) >= 4 + w * h * 3:
    alpha = data[4 + w * h * 2:4 + w * h * 3]
    araw = bytearray()
    for y in range(h):
        araw.append(0)
        for x in range(w):
            v = min(alpha[y * w + x] * 255 // 31, 255)
            araw += bytes((v, v, v))
    apng = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    apng += chunk(b"IDAT", zlib.compress(bytes(araw))) + chunk(b"IEND", b"")
    open(sys.argv[3], "wb").write(apng)

png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")
open(sys.argv[2], "wb").write(png)
