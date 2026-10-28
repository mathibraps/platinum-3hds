#!/usr/bin/env python3
"""Medians of the logic frames (game time above 1 ms) in a frame log CSV
(tools/emu_frames.py), between two frame numbers.
usage: frames_logic.py <frames.csv> [first] [last]"""
import sys

rows = [l.strip().split(",") for l in open(sys.argv[1])]
head, rows = rows[0], rows[1:]
lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 30
rows = [[float(v) for v in r] for r in rows if lo <= int(r[0]) <= hi]
logic = [r for r in rows if r[3] > 1]
over = sum(1 for r in rows if r[2] > 1000 / 60)
pairs = sum(1 for a, b in zip(rows, rows[1:]) if a[2] + b[2] > 2000 / 60)
out = [f"{len(logic)} logic frames of {len(rows)}"]
for k in range(1, len(head)):
    v = sorted(r[k] for r in logic)
    out.append(f"{head[k]} {v[len(v) // 2]:.1f}")
print(", ".join(out) + f"; {over} frames busy over 16.7 ms, {pairs} pairs over 33.3 ms")
