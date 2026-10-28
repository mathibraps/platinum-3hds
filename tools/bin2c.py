#!/usr/bin/env python3
"""Embeds a binary file as a C array: bin2c.py <in> <out.c> <symbol>
(defines const unsigned char <symbol>[] and const unsigned <symbol>_size)."""
import sys

data = open(sys.argv[1], "rb").read()
name = sys.argv[3]
with open(sys.argv[2], "w") as f:
    f.write(f"/* Generated from {sys.argv[1]} by tools/bin2c.py. */\n")
    f.write(f"const unsigned char {name}[] __attribute__((aligned(4))) = {{\n")
    for i in range(0, len(data), 16):
        f.write("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",\n")
    f.write(f"}};\nconst unsigned {name}_size = {len(data)};\n")
