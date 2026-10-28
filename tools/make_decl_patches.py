#!/usr/bin/env python3
"""Create patches for decomp files that call functions without declaring them.

Metrowerks accepted implicit function declarations; GCC 14+/Clang reject them,
and they are unsafe anyway. For each (file, function) reported by
tools/trial_compile.sh, this adds the #include of the decomp header declaring
the function, or, if no header declares it, a prototype copied from the
function's definition. Patches go to patches/<path under src>.patch; files
that already have a patch are reported for merging by hand.

Usage: make_decl_patches.py <decomp dir> <trial report> [skip file ...]
"""
import os
import re
import subprocess
import sys
import tempfile

decomp, report = sys.argv[1], sys.argv[2]
skip = set(sys.argv[3:])
LIBC = {"abs": "<stdlib.h>", "labs": "<stdlib.h>", "memcmp": "<string.h>"}

missing = {}
for line in open(report):
    m = re.match(r"^([^:]+): .*error: call to undeclared (?:library )?function '(\w+)'", line)
    if m and m.group(1) not in skip:
        missing.setdefault(m.group(1), set()).add(m.group(2))

headers = {}
for root, _, files in os.walk(os.path.join(decomp, "include")):
    for f in files:
        if f.endswith(".h"):
            path = os.path.join(root, f)
            headers[os.path.relpath(path, os.path.join(decomp, "include"))] = open(path, errors="replace").read()

sources = {}
for root, _, files in os.walk(os.path.join(decomp, "src")):
    for f in files:
        if f.endswith(".c"):
            path = os.path.join(root, f)
            sources[os.path.relpath(path, os.path.join(decomp, "src"))] = open(path, errors="replace").read()

for src, funcs in sorted(missing.items()):
    text = sources[src]
    additions = []
    for fn in sorted(funcs):
        if fn in LIBC:
            additions.append(f"#include {LIBC[fn]}")
            continue
        decl = re.compile(r"^[\w\s\*]+\b" + fn + r"\s*\([^;{]*\)\s*;", re.M)
        hdr = next((h for h, t in sorted(headers.items()) if decl.search(t)), None)
        if hdr:
            additions.append(f'#include "{hdr}"')
            continue
        defn = re.compile(r"^((?!static)[\w\s\*]+\b" + fn + r"\s*\([^;{]*\))\s*\n\{", re.M)
        found = next((m.group(1) for t in sources.values() for m in [defn.search(t)] if m), None)
        if found:
            additions.append(f"{' '.join(found.split())}; // port: declared where Metrowerks inferred it")
        else:
            print(f"{src}: no declaration found for {fn}", file=sys.stderr)
    additions = sorted(set(additions), key=lambda a: (not a.startswith("#"), a))
    if not additions:
        continue

    patch_path = os.path.join("patches", src + ".patch")
    os.makedirs(os.path.dirname(patch_path), exist_ok=True)
    if os.path.exists(patch_path):
        print(f"{src}: already has {patch_path}; merge by hand: {additions}", file=sys.stderr)
        continue

    lines = text.split("\n")
    last_include = max(i for i, l in enumerate(lines) if l.startswith("#include"))
    new = lines[:last_include + 1] + additions + lines[last_include + 1:]
    with tempfile.TemporaryDirectory() as tmp:
        a, b = os.path.join(tmp, "a"), os.path.join(tmp, "b")
        os.makedirs(a)
        os.makedirs(b)
        name = os.path.basename(src)
        open(os.path.join(a, name), "w").write(text)
        open(os.path.join(b, name), "w").write("\n".join(new))
        diff = subprocess.run(["diff", "-u", f"a/{name}", f"b/{name}"], cwd=tmp, capture_output=True, text=True).stdout
    open(patch_path, "w").write(diff)
    print(f"{patch_path}: {', '.join(additions)}")
