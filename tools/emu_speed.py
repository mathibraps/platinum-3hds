#!/usr/bin/env python3
"""Samples Azahar's own performance numbers (emulation speed, the game's
frame rate, host frame time) over the RPC server once a second, to tell the
game's slow frames from the emulator not keeping up on this computer.

usage: emu_speed.py <AzaharRPC.py dir> <seconds> [out.csv]
"""
import sys
import time

sys.path.insert(0, sys.argv[1])
from AzaharRPC import AzaharRPC, PERF_STATS_FIELDS  # noqa: E402

rpc = AzaharRPC(timeout=3)
rows = []
t0 = time.time()
while time.time() - t0 < float(sys.argv[2]):
    time.sleep(1)
    try:
        s = rpc.perf_stats(reset=True)
    except Exception:
        continue
    if s:
        rows.append((round(time.time() - t0, 1), s))
if len(sys.argv) > 3:
    with open(sys.argv[3], "w") as f:
        f.write("t," + ",".join(PERF_STATS_FIELDS) + "\n")
        for t, s in rows:
            f.write(f"{t}," + ",".join(str(s[k]) for k in PERF_STATS_FIELDS) + "\n")
slow = [(t, s) for t, s in rows if s["emulation_speed"] < 0.985]
print(f"{len(rows)} samples; emulation below 98.5% speed in {len(slow)}:",
      " ".join(f"{t}s={s['emulation_speed'] * 100:.0f}%" for t, s in slow[:40]))
