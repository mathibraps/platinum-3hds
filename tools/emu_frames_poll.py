#!/usr/bin/env python3
"""Collects the per-frame times of a whole run of the 3DS build in Azahar,
however long: reads what is new in the frame log (gPortFrameLog, 4096
frames) over the RPC server every few seconds until the game quits, and
writes every frame to a CSV file (the columns of tools/emu_frames.py; the
last three are counts, not times).

usage: emu_frames_poll.py <AzaharRPC.py dir> <.map file> <frames.csv> [seconds between reads] [frames of the script]

The RPC server sometimes stops answering for a while; the reads are tried
again for up to five minutes. With the script's frame count it stops as soon
as the game has played them.
"""
import re
import struct
import sys
import time

sys.path.insert(0, sys.argv[1])
from AzaharRPC import AzaharRPC  # noqa: E402

FRAME_LOG = 4096
COLUMNS = 19
TIMES = 16  # columns in units of 10 us; the rest are counts
addr = None
for line in open(sys.argv[2]):
    m = re.match(r"\s+0x([0-9a-f]+)\s+gPortFrameLog$", line)
    if m:
        addr = int(m.group(1), 16)
if addr is None:
    sys.exit("gPortFrameLog not in the map file")
every = float(sys.argv[4]) if len(sys.argv) > 4 else 10
expected = int(sys.argv[5]) if len(sys.argv) > 5 else 0

rpc = AzaharRPC(timeout=3)
rows = {}
done = 0  # frames read so far
started = False
misses = 0


def read_column(k, first, last):
    """Values of column k for frames [first, last), in up to two reads (the log is a ring)."""
    out = []
    n = first
    while n < last:
        i = n % FRAME_LOG
        count = min(last - n, FRAME_LOG - i)
        data = rpc.read_memory(addr + 8 + 2 * FRAME_LOG * k + 2 * i, 2 * count)
        if data is None or len(data) != 2 * count:
            raise OSError("short read")
        out += struct.unpack(f"<{count}H", data)
        n += count
    return out


while misses < 300 / every and not (expected and done >= expected - 60):
    try:
        header = rpc.read_memory(addr, 8)
        if header is None:
            raise OSError("no reply")
        magic, count = struct.unpack("<II", header)
        if magic != 0x474F4C46:
            raise OSError("no frame log yet")
        started = True
        misses = 0
        # The newest frame may be half written: leave it for the next read.
        first, last = max(done, count - FRAME_LOG + 1), count - 1
        if last > first:
            cols = [read_column(k, first, last) for k in range(COLUMNS)]
            for j, n in enumerate(range(first, last)):
                rows[n] = tuple(cols[k][j] / (100 if k < TIMES else 1) for k in range(COLUMNS))
            done = last
    except Exception as e:  # not running yet, quit, or a slow reply (tried again next time)
        misses += started
        if started:
            print(f"read failed ({e!r})", file=sys.stderr)
    time.sleep(every)

with open(sys.argv[3], "w") as f:
    f.write("frame,whole_ms,busy_ms,game_ms,render_ms,flush_ms,geometry_ms,setup3d_ms,fs_ms,lz_ms,compose_ms,draw2d_ms,present_ms,"
            "bg_ms,obj_ms,passes_ms,widen_ms,tex_decodes,tex_ktexels,tex_hashes\n")
    for n in sorted(rows):
        f.write("%d," % n + ",".join("%.2f" % v for v in rows[n]) + "\n")
print(f"{len(rows)} frames in {sys.argv[3]}")
