#!/usr/bin/env python3
"""Puts two snapshots of the same size next to each other with a third panel
marking the differing pixels in red: side_by_side.py a.png b.png out.png"""
import struct
import sys
import zlib

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from png_diff import read_png  # noqa: E402


def write_png(path, w, h, rows):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
    raw = b"".join(b"\0" + r for r in rows)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


wa, ha, ba, ra = read_png(sys.argv[1])
wb, hb, bb, rb = read_png(sys.argv[2])
gap = b"\x40\x40\x40" * 4
rows = []
for y in range(ha):
    a = b"".join(ra[y][x * ba:x * ba + 3] for x in range(wa))
    b = b"".join(rb[y][x * bb:x * bb + 3] for x in range(wb))
    d = b"".join((b"\xff\x00\x00" if a[x * 3:x * 3 + 3] != b[x * 3:x * 3 + 3] else bytes(c // 3 for c in a[x * 3:x * 3 + 3]))
                 for x in range(wa))
    rows.append(a + gap + b + gap + d)
write_png(sys.argv[3], wa * 3 + 8, ha, rows)
