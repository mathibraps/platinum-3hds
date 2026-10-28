#!/usr/bin/env python3
"""Summarizes a frame log CSV (tools/emu_frames.py): the logic frames (game
time above 1 ms) between two frame numbers, and the frames that missed 60 fps.

usage: frames_summary.py <frames.csv> [first frame] [last frame]"""
import sys

rows = [l.strip().split(",") for l in open(sys.argv[1])][1:]
lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 30
rows = [(int(r[0]), *map(float, r[1:])) for r in rows if lo <= int(r[0]) <= hi]
game = sorted(r[3] for r in rows if r[3] > 1)
busy = [r for r in rows if r[2] > 1000 / 60]
pairs = sum(1 for a, b in zip(rows, rows[1:]) if a[2] + b[2] > 2000 / 60)
if game:
    print("%d frames, %d logic frames: game median %.1f ms, 90%% %.1f, max %.1f; %d busy over 16.7 ms, %d pairs over 33.3 ms"
          % (len(rows), len(game), game[len(game) // 2], game[len(game) * 9 // 10], game[-1], len(busy), pairs))
