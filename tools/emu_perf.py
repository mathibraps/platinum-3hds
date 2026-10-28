#!/usr/bin/env python3
"""Reads the port's frame-time breakdown (gPortPerf in frontend/main_3ds.c)
from a running Azahar emulator over its RPC server, and optionally saves a
screenshot. Enable "RPC server" in Azahar's debug settings first.

usage: emu_perf.py <AzaharRPC.py dir> <.map file> [screenshot.png]
"""
import re
import struct
import sys

sys.path.insert(0, sys.argv[1])
from AzaharRPC import AzaharRPC  # noqa: E402

addr = bench = None
for line in open(sys.argv[2]):
    m = re.match(r"\s+0x([0-9a-f]+)\s+(gPortPerf|gPortBench)$", line)
    if m and m.group(2) == "gPortPerf":
        addr = int(m.group(1), 16)
    elif m:
        bench = int(m.group(1), 16)
if addr is None:
    sys.exit("gPortPerf not in the map file")

rpc = AzaharRPC(timeout=3)
data = rpc.read_memory(addr, 48)
magic, frames, frame, game, raster, render, present, bg, obj, compose, gpu3d, wait = struct.unpack("<12I", data)
if magic != 0x46524550:
    sys.exit(f"no perf block at 0x{addr:x} (is the game running?)")
print(f"after {frames} frames (us/frame): frame {frame}  game {game} (3D prep {raster})  GPU 3D {gpu3d}  wait {wait}  "
      f"2D render {render} (BG {bg}, OBJ {obj}, compose {compose})  present+vsync {present}  -> {1e6 / max(frame, 1):.1f} fps possible")
if bench is not None:
    n = 16
    names = ("copyright (frames 600-659)", "logo (1500-1559)", "field (2250-2309)", "sprite bands (3600-3659)")
    raw = b"".join(rpc.read_memory(bench + i * n * 4, n * 4) for i in range(len(names)))  # one window per read
    b = struct.unpack(f"<{len(names) * n}I", raw)
    for i, name in enumerate(names):
        frame_us, game_us, render_us, gpu_us = b[i * n:i * n + 4]
        stages = b[i * n + 4:i * n + 10]
        if frame_us:
            line = f"bench {name}: frame {frame_us / 1000:.1f} ms  game {game_us / 1000:.1f}  2D {render_us / 1000:.1f}  GPU 3D {gpu_us / 1000:.1f}"
            if any(stages):
                line += "  stages main BG/OBJ/compose {:.1f}/{:.1f}/{:.1f}  sub {:.1f}/{:.1f}/{:.1f}".format(*(v / 1000 for v in stages))
            line += f"  (longest: game {b[i * n + 10] / 1000:.1f}, work {b[i * n + 11] / 1000:.1f})"
            ext = b[i * n + 12:i * n + 16]
            if any(ext):
                line += "  compose: search/columns/rows/passes {:.1f}/{:.1f}/{:.1f}/{:.1f}".format(*(v / 1000 for v in ext))
            print(line)
extra = struct.unpack("<15I", rpc.read_memory(addr + 48, 60))
print(f"render worker on second core: {bool(extra[0] & 1)}, New 3DS: {bool(extra[0] & 2)}, measured CPU clock ~{extra[1]} MHz")
print(f"longest of the last 60 frames: game {extra[2] / 1000:.1f} ms, frame {extra[3] / 1000:.1f} ms; frames over two VBlanks so far: {extra[4]}")
mode, snd_frames, snd_us, starved = extra[5:9]
secs = snd_frames / 32728.5
print(f"sound: {('silent (no NDSP)', 'game thread', 'audio thread')[min(mode, 2)]}, {secs:.1f} s made"
      + (f", audio thread {snd_us / max(secs, 1e-9) / 1e4:.1f}% of a core" if mode == 2 else "")
      + f", NDSP ran dry {starved} times")
print(f"memory: malloc heap {extra[9] / 1024:.1f} MB, {extra[10] / 1024:.1f} MB in use; linear memory free {extra[11] / 1024:.1f} MB; "
      f"file cache {extra[12] / 1024:.1f} MB, read ahead {extra[13] / 1024:.1f} MB; failed game allocations {extra[14]}")
emu = rpc.perf_stats()
print(f"emulator: game fps {emu['game_fps']:.1f}, speed {emu['emulation_speed'] * 100:.0f}%")
if len(sys.argv) > 3:
    rpc.save_screenshot(sys.argv[3])
