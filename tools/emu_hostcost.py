#!/usr/bin/env python3
"""What the 3DS build costs the Azahar emulator on this computer: the CPU
time of Azahar's threads per emulated frame (the frame log's count, read over
RPC), over a window of a run. Unlike the emulation speed, this does not
depend on frame pacing or on other programs taking the CPU.

usage: emu_hostcost.py <AzaharRPC.py dir> <.map file> <azahar pid> <start s> <end s>
"""
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, sys.argv[1])
from AzaharRPC import AzaharRPC  # noqa: E402

addr = None
for line in open(sys.argv[2]):
    m = re.match(r"\s+0x([0-9a-f]+)\s+gPortFrameLog$", line)
    if m:
        addr = int(m.group(1), 16)
pid, start, end = sys.argv[3], float(sys.argv[4]), float(sys.argv[5])


def secs(t):
    parts = [float(p) for p in t.split(":")]
    v = 0.0
    for p in parts:
        v = v * 60 + p
    return v


def threads():
    out = subprocess.run(["ps", "-M", "-p", pid], capture_output=True, text=True).stdout.split("\n")[1:]
    times = []
    for l in out:
        f = l.split()
        if len(f) < 4:
            continue
        # STIME UTIME are the last two time fields (the first line also has the command).
        tf = [x for x in f if re.match(r"^\d+:\d+(\.\d+)?$", x) or re.match(r"^\d+:\d+:\d+(\.\d+)?$", x)]
        if len(tf) >= 2:
            times.append(secs(tf[0]) + secs(tf[1]))
    return times


rpc = AzaharRPC(timeout=3)
t0 = time.time()
time.sleep(start)
a = threads()
fa = struct.unpack("<I", rpc.read_memory(addr + 4, 4))[0]
time.sleep(end - start)
b = threads()
fb = struct.unpack("<I", rpc.read_memory(addr + 4, 4))[0]
frames = fb - fa
d = sorted((y - x for x, y in zip(a, b)), reverse=True)
total = sum(d)
print(f"{frames} frames in {end - start:.0f} s: host CPU per frame {total / frames * 1000:.2f} ms in all, "
      f"busiest threads " + " ".join(f"{v / frames * 1000:.2f}" for v in d[:4]) + " ms")
