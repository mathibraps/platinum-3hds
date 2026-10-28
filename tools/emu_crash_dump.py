#!/usr/bin/env python3
"""After a crash of the 3DS build in Azahar (which keeps running, stuck on the
exception): prints the first exception in Azahar's log with the function at
its PC and LR, and tries to read the memory its registers point to over the
RPC server (Azahar, busy logging the exception again and again, often does
not answer; then it stops at the first read). For a crash in the billboard
code (billboard.c) it also prints the billboard's animation sequence.

usage: emu_crash_dump.py <AzaharRPC.py dir> <azahar_log.txt> <.elf>
"""
import re
import struct
import subprocess
import sys

sys.path.insert(0, sys.argv[1])
from AzaharRPC import AzaharRPC  # noqa: E402

log = open(sys.argv[2], errors="replace").read()
m = re.search(r"UnmappedAccess:\d+: ([^\n]*)\n[^\n]*LogException[^\n]*\n(.*?)Stack Dump", log, re.S)
if not m:
    sys.exit("no exception in the log")
print(m.group(1))
print(m.group(2).strip())
regs = dict(re.findall(r"(R\d+|SP|LR|PC)\s+0x([0-9A-F]+)", m.group(2)))
regs = {k: int(v, 16) for k, v in regs.items()}
addr2line = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line"
for k in ("PC", "LR"):
    out = subprocess.run([addr2line, "-f", "-e", sys.argv[3], hex(regs[k])], capture_output=True, text=True).stdout.split("\n")
    print(f"{k} 0x{regs[k]:08x} {out[0]} {out[1].split('/')[-1] if len(out) > 1 else ''}")
rpc = AzaharRPC(timeout=3)


def read(addr, n):
    try:
        data = rpc.read_memory(addr, n)
    except TimeoutError:
        sys.exit("(the emulator does not answer over RPC)")
    return data if data is not None and len(data) == n else None


for k in sorted(regs, key=lambda r: (len(r), r)):
    v = regs[k]
    if 0x08000000 <= v < 0x10000000 and k not in ("SP", "PC", "LR"):
        data = read(v & ~3, 32)
        print(f"{k:3} 0x{v:08x}: {data.hex(' ') if data else '(unreadable)'}")
if "BillboardLists_Draw" in subprocess.run([addr2line, "-f", "-e", sys.argv[3], hex(regs["PC"])], capture_output=True, text=True).stdout:
    b = regs["R4"]
    raw = read(b + 0x80, 0x40)
    modelSet, model, tex, animTex = struct.unpack("<4I", raw[4:20])
    seqCount, startFrame, textureIdx, plttIdx = struct.unpack("<4I", raw[0x20:0x30])
    frameNum = struct.unpack("<i", raw[0x38:0x3c])[0]
    print(f"billboard 0x{b:08x}: model 0x{model:08x} texture 0x{tex:08x} animTexture 0x{animTex:08x} frame {frameNum / 4096:.2f}")
    print(f"  sequence: {seqCount} steps, startFrame 0x{startFrame:08x} textureIdx 0x{textureIdx:08x} plttIdx 0x{plttIdx:08x}")
    n = min(seqCount, 64)
    for name, at, size in (("startFrame", startFrame, 2), ("textureIdx", textureIdx, 1), ("plttIdx", plttIdx, 1)):
        data = read(at, n * size) if n else None
        print(f"  {name}: {data.hex(' ') if data else '(unreadable)'}")
    # The resource the sequence lies in starts after a heap header (magic 'HEAP').
    data = read((startFrame - 4 - 64) & ~3, 64)
    if data:
        print(f"  64 bytes before the sequence: {data.hex(' ')}")
    data = read(animTex, 64)
    print(f"  animTexture: {data.hex(' ') if data else '(unreadable)'}")
