#!/usr/bin/env python3
"""Reads the per-frame times of the 3DS build (gPortFrameLog in
frontend/main_3ds.c) from a running Azahar emulator over its RPC server and
reports the frames that could not keep 60 fps: those busy for more than a
60th of a second. Optionally writes every logged frame to a CSV file.

usage: emu_frames.py <AzaharRPC.py dir> <.map file> [frames.csv]
"""
import re
import struct
import sys

sys.path.insert(0, sys.argv[1])
from AzaharRPC import AzaharRPC  # noqa: E402

FRAME_LOG = 4096
BUDGET_MS = 1000 / 60

addr = None
for line in open(sys.argv[2]):
    m = re.match(r"\s+0x([0-9a-f]+)\s+gPortFrameLog$", line)
    if m:
        addr = int(m.group(1), 16)
if addr is None:
    sys.exit("gPortFrameLog not in the map file")

rpc = AzaharRPC(timeout=3)
magic, count = struct.unpack("<II", rpc.read_memory(addr, 8))
if magic != 0x474F4C46:
    sys.exit(f"no frame log at 0x{addr:x} (is the game running?)")
data = rpc.read_memory(addr + 8, 38 * FRAME_LOG)
cols = [struct.unpack_from(f"<{FRAME_LOG}H", data, 2 * FRAME_LOG * k) for k in range(19)]
first = max(0, count - FRAME_LOG)
rows = []
for n in range(first, count):
    i = n % FRAME_LOG
    rows.append((n, *(c[i] / 100 for c in cols)))  # frame, busy, game, render, flush, geometry, 3D setup, file reads, LZ77 in ms

drops = [r for r in rows if r[2] > BUDGET_MS]
print(f"{count} frames so far; the last {len(rows)}: {len(drops)} busy for more than {BUDGET_MS:.2f} ms")
if rows:
    busy = sorted(r[2] for r in rows)
    print("busy ms: median {:.1f}, 99th percentile {:.1f}, max {:.1f}".format(
        busy[len(busy) // 2], busy[min(len(busy) - 1, len(busy) * 99 // 100)], busy[-1]))
for r in sorted(drops, key=lambda r: -r[2])[:25]:
    print("  frame {:6d}: busy {:5.1f} ms (game {:5.1f}, 2D and drawing {:5.1f}, of which cache flushes {:4.1f}), whole frame {:5.1f}"
          .format(r[0], r[2], r[3], r[4], r[5], r[1]))
if len(sys.argv) > 3:
    with open(sys.argv[3], "w") as f:
        f.write("frame,whole_ms,busy_ms,game_ms,render_ms,flush_ms,geometry_ms,setup3d_ms,fs_ms,lz_ms,compose_ms,draw2d_ms,present_ms,bg_ms,obj_ms,passes_ms,widen_ms,tex_decodes,tex_ktexels,tex_hashes\n")
        for r in rows:
            f.write("%d," % r[0] + ",".join("%.2f" % v for v in r[1:]) + "\n")
