#!/usr/bin/env python3
"""Watches the 3DS build's file troubles in a running Azahar emulator over its
RPC server: how many mounts romfs took at start and the files the last bad
mount was missing (gPortRomfs), and the opens that failed and reads that came
back short since (gNtrFsTrouble, platform/src/ntr_sys.c). Prints a line
whenever they change, with the frame, until the emulator has been gone for a
minute.

usage: emu_fs_watch.py <AzaharRPC.py dir> <.map file> [seconds between reads]
"""
import re
import struct
import sys
import time

sys.path.insert(0, sys.argv[1])
from AzaharRPC import AzaharRPC  # noqa: E402

syms = {}
for line in open(sys.argv[2]):
    m = re.match(r"\s+0x([0-9a-f]+)\s+(gPortRomfs|gNtrFsTrouble|gPortFrameLog)$", line)
    if m:
        syms[m.group(2)] = int(m.group(1), 16)
every = float(sys.argv[3]) if len(sys.argv) > 3 else 20
rpc = AzaharRPC(timeout=3)
last, misses, seen = None, 0, False
while misses * every < 60 or not seen:
    try:
        romfs = rpc.read_memory(syms["gPortRomfs"], 8)
        trouble = rpc.read_memory(syms["gNtrFsTrouble"], 16 + 128)
        header = rpc.read_memory(syms["gPortFrameLog"], 8)
        if None in (romfs, trouble, header):
            raise OSError("no reply")
        mounts, missing = struct.unpack("<2I", romfs)
        opens, shorts, err, files = struct.unpack("<4I", trouble[:16])
        path = trouble[16:].split(b"\0")[0].decode(errors="replace")
        frame = struct.unpack("<II", header)[1]
        now = (mounts, missing, opens, shorts, err, path)
        seen, misses = True, 0
        if now != last and mounts:
            print(f"frame {frame}: romfs mounts {mounts} (last bad one missing {missing}); open failures {opens}, "
                  f"short reads {shorts}, errno {err}, open files {files}, last {path}", flush=True)
            last = now
    except Exception:  # not started yet, gone, or a slow reply
        misses += 1
    time.sleep(every)
