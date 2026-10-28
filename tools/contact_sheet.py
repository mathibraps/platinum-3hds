#!/usr/bin/env python3
"""Tile harness snapshots (top above bottom, frames left to right) into one PNG.

Usage: contact_sheet.py [--cols N] <out.png> <dir> <frame> [<frame> ...]
Reads <dir>/frameNNNN_top.png and _bottom.png as written by pokeplat_host
(any size; a narrower bottom screen is centred). --cols wraps into rows.
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = w * 4 + 1
    return w, h, [raw[y * stride + 1:(y + 1) * stride] for y in range(h)]


def write_png(path, w, h, rows):
    raw = b"".join(b"\0" + r for r in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


args = sys.argv[1:]
cols = 0
if args[0] == "--cols":
    cols = int(args[1])
    args = args[2:]
out, src, frames = args[0], args[1], args[2:]
cols = cols or len(frames)
gap = 4
gray = b"\x40\x40\x40\xff"


def pad(rows, w, width):
    left = (width - w) // 2
    return [gray * left + r + gray * (width - w - left) for r in rows]


tiles = []
for fr in frames:
    tw, _, top = read_png(f"{src}/frame{int(fr):04d}_top.png")
    bw, _, bottom = read_png(f"{src}/frame{int(fr):04d}_bottom.png")
    width = max(tw, bw)
    tiles.append(pad(top, tw, width) + [gray * width] * gap + pad(bottom, bw, width))

tile_w = max(len(t[0]) for t in tiles) // 4
tile_h = max(len(t) for t in tiles)
tiles = [[r + gray * (tile_w - len(r) // 4) for r in t] + [gray * tile_w] * (tile_h - len(t)) for t in tiles]
rows = []
for start in range(0, len(tiles), cols):
    group = tiles[start:start + cols]
    group += [[gray * tile_w] * tile_h] * (cols - len(group))
    if rows:
        rows += [gray * (cols * tile_w + (cols - 1) * gap)] * gap
    for y in range(tile_h):
        rows.append((gray * gap).join(t[y] for t in group))
write_png(out, len(rows[0]) // 4, len(rows), rows)
print(f"wrote {out}")
