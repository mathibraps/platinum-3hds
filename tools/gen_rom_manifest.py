#!/usr/bin/env python3
"""Writes the builder's list of the game's files (builder/templates/
manifest.txt): "sha1 size path" for each file of the extracted ROM, which the
builder checks a ROM against. Only the hashes go in it, none of the files.

usage: gen_rom_manifest.py <romfs dir> <manifest.txt>
"""
import hashlib
import os
import sys

root, out = sys.argv[1], sys.argv[2]
lines = []
for d, _, files in os.walk(root):
    for f in files:
        full = os.path.join(d, f)
        data = open(full, "rb").read()
        lines.append("%s %d %s" % (hashlib.sha1(data).hexdigest(), len(data), os.path.relpath(full, root).replace(os.sep, "/")))
lines.sort(key=lambda l: l.split(" ", 2)[2])
with open(out, "w") as f:
    f.write("\n".join(lines) + "\n")
print(f"{len(lines)} files in {out}")
