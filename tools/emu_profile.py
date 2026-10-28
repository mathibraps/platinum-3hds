#!/usr/bin/env python3
"""Statistical profiler for the 3DS build running in Azahar (started with its
gdb stub, -g PORT). Repeatedly interrupts the emulated CPU, reads the program
counter, resumes, and finally maps the samples to functions with
arm-none-eabi-addr2line.

usage: emu_profile.py <port> <elf> <samples> [interval seconds] [seconds to run first] [--lines]
(--lines: also list the hottest source lines, --file=NAME only those of one
source file; --tag=ADDR:OUT.csv also reads the word at ADDR with each sample
(e.g. the frame counter) and writes the samples to OUT.csv; --when=ADDR:MOD:REM
keeps only samples taken while the word at ADDR is REM modulo MOD (e.g. one
frame in 20), until <samples> are kept; the callers of the hottest
system calls are listed too, from the link register)
"""
import collections
import socket
import subprocess
import sys
import time

port, elf, count = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
interval = float(sys.argv[4]) if len(sys.argv) > 4 else 0.02
warmup = float(sys.argv[5]) if len(sys.argv) > 5 else 0
sock = socket.create_connection(("127.0.0.1", port))
sock.settimeout(10)
buf = b""


def send(data):
    cs = sum(data.encode()) & 0xff
    sock.sendall(f"${data}#{cs:02x}".encode())


def recv_packet():
    global buf
    while True:
        start = buf.find(b"$")
        if start >= 0:
            end = buf.find(b"#", start)
            if end >= 0 and len(buf) >= end + 3:
                pkt = buf[start + 1:end].decode(errors="replace")
                buf = buf[end + 3:]
                sock.sendall(b"+")
                return pkt
        buf += sock.recv(65536)


def request(data):
    send(data)
    return recv_packet()


sock.sendall(b"+")
request("?")
if warmup:
    send("c")
    time.sleep(warmup)
    sock.sendall(b"\x03")
    recv_packet()
pcs = []
lrs = []
tags = []
tag = None
when = None
for a in sys.argv[1:]:
    if a.startswith("--tag="):
        addr, out = a[6:].split(":", 1)
        tag = (int(addr, 16), out)
    if a.startswith("--when="):
        addr, mod, rem = a[7:].split(":")
        when = (int(addr, 16), int(mod), int(rem))
try:
    while len(pcs) < count:
        send("c")
        time.sleep(interval)
        sock.sendall(b"\x03")
        recv_packet()  # stop reply
        if when:
            word = request(f"m{when[0]:x},4")
            value = int.from_bytes(bytes.fromhex(word[:8]), "little") if len(word) >= 8 else -1
            if value % when[1] != when[2]:
                continue
        reg = request("pf")
        lr = request("pe")
        if tag:
            word = request(f"m{tag[0]:x},4")
            tags.append(int.from_bytes(bytes.fromhex(word[:8]), "little") if len(word) >= 8 else -1)
        if len(reg) >= 8:
            pcs.append(int.from_bytes(bytes.fromhex(reg[:8]), "little"))
            lrs.append(int.from_bytes(bytes.fromhex(lr[:8]), "little") if len(lr) >= 8 else 0)
    send("c")
    sock.close()
except OSError as e:  # the program quit (a test script ended): keep what we have
    print(f"stopped after {len(pcs)} samples: {e}")

addrs = sorted(set(pcs))
out = subprocess.run(["/opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line", "-f", "-e", elf] + [hex(a) for a in addrs],
                     capture_output=True, text=True).stdout.split("\n")
name = {a: out[i * 2] for i, a in enumerate(addrs)}
funcs = collections.Counter(name[p] for p in pcs)
if "--lines" in sys.argv:
    lout = subprocess.run(["/opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line", "-e", elf] + [hex(a) for a in addrs],
                          capture_output=True, text=True).stdout.split("\n")
    line = {a: lout[i].rsplit("/", 1)[-1] for i, a in enumerate(addrs)}
    print("hottest lines:")
    only = [a for a in sys.argv[1:] if a.startswith("--file=")]
    sel = [p for p in pcs if not only or line[p].startswith(only[0][7:])]
    for l, c in collections.Counter(line[p] for p in sel).most_common(30):
        print(f"{c * 100 / len(pcs):5.1f}%  {l}")
print(f"{len(pcs)} samples")
for f, c in funcs.most_common(40):
    print(f"{c * 100 / len(pcs):5.1f}%  {f}")

# Who calls the system calls (svc wrappers return straight to their caller).
svc = [(pcs[i], lrs[i]) for i in range(len(pcs)) if name[pcs[i]].startswith("svc")]
if svc:
    las = sorted(set(l for _, l in svc))
    lout = subprocess.run(["/opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line", "-f", "-e", elf] + [hex(a) for a in las],
                          capture_output=True, text=True).stdout.split("\n")
    lname = {a: lout[i * 2] for i, a in enumerate(las)}
    print("system call callers:")
    for (f, c), k in collections.Counter((name[p], lname[l]) for p, l in svc).most_common(15):
        print(f"{k * 100 / len(pcs):5.1f}%  {f} <- {c}")

if tag:
    with open(tag[1], "w") as f:
        f.write("tag,function\n")
        for t, p in zip(tags, pcs):
            f.write(f"{t},{name[p]}\n")
