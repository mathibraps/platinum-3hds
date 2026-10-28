#!/usr/bin/env python3
"""Create patches for constructs clang accepts but GCC (devkitARM) rejects.

Reads a GCC build log and fixes, in the decomp sources it names:
  * "static declaration of 'x' follows non-static declaration": Metrowerks
    (and clang with -fms-extensions) let a static definition follow an extern
    declaration; the definition loses "static".
  * a const variable used as an array size (not a constant expression in C):
    the variable becomes an enum constant.
  * NULL passed for an enum argument: becomes 0.

Patches build on any existing patch for the file (the patched copy in
<build>/gamesrc is the starting point) and replace patches/<file>.patch.

usage: make_gcc_patches.py <decomp dir> <build dir> <gcc log>
"""
import collections
import os
import re
import subprocess
import sys
import tempfile

decomp, build, log = sys.argv[1:4]
gamesrc = os.path.join(build, "gamesrc") + "/"
edits = collections.defaultdict(list)  # rel path -> [(line, kind, name)]

for line in open(log, errors="replace"):
    m = re.match(r"^(\S+?):(\d+):\d+: error: (.*)$", line.rstrip())
    if not m or not m.group(1).startswith(gamesrc):
        continue
    rel, num, msg = m.group(1)[len(gamesrc):], int(m.group(2)), m.group(3)
    if "follows non-static declaration" in msg:
        edits[rel].append((num, "static", None))
    elif "storage size of" in msg and "isn't constant" in msg:
        edits[rel].append((num, "vla", None))
    elif re.match(r"incompatible type for argument \d+ of", msg):
        edits[rel].append((num, "null", None))

for rel, items in sorted(edits.items()):
    orig = open(os.path.join(decomp, "src", rel), errors="replace").read()
    lines = open(gamesrc + rel, errors="replace").read().split("\n")
    for num, kind, _ in sorted(set(items)):
        i = num - 1
        if kind == "static":
            lines[i] = re.sub(r"\bstatic\s+", "", lines[i], count=1)
        elif kind == "null":
            lines[i] = re.sub(r"\(NULL,", "(0,", lines[i], count=1)
        elif kind == "vla":
            # The size names a "static const <int type> name = value;" above it.
            for size in re.findall(r"\[(\w+)\]", lines[i]):
                for j in range(i - 1, max(i - 40, 0), -1):
                    d = re.match(r"^(\s*)static const \w+ " + size + r" = (\w+);(.*)$", lines[j])
                    if d:
                        lines[j] = f"{d.group(1)}enum {{ {size} = {d.group(2)} }};{d.group(3)} // port: constant expression for GCC"
                        break
    new = "\n".join(lines)
    patch_path = os.path.join("patches", rel + ".patch")
    os.makedirs(os.path.dirname(patch_path) or ".", exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        name = os.path.basename(rel)
        for d, t in (("a", orig), ("b", new)):
            os.makedirs(os.path.join(tmp, d))
            open(os.path.join(tmp, d, name), "w").write(t)
        diff = subprocess.run(["diff", "-u", f"a/{name}", f"b/{name}"], cwd=tmp, capture_output=True, text=True).stdout
    open(patch_path, "w").write(diff)
    print(f"{patch_path}: {len(set(items))} fix(es)")
