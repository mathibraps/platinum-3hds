#!/usr/bin/env python3
"""Rough call-graph closure over the decomp sources.

Indexes every non-static function definition in <decomp>/src, then walks
outward from the given start files, following identifiers that name a
function defined in another file. Files listed with --stop are treated as
port-provided (stubbed) and not expanded.

Usage: dep_closure.py <decomp> <start.c>... [--stop file.c ...]
"""
import os
import re
import sys

args = sys.argv[1:]
decomp = args.pop(0)
stops = set()
if "--stop" in args:
    i = args.index("--stop")
    stops = set(args[i + 1:])
    args = args[:i]

src_root = os.path.join(decomp, "src")
defs = {}
texts = {}
def_re = re.compile(r"^(?!static)[A-Za-z_][\w\s\*]*?\b(\w+)\s*\([^;{]*\)\s*\n\{", re.M)
for root, _, files in os.walk(src_root):
    for f in files:
        if not f.endswith(".c"):
            continue
        path = os.path.relpath(os.path.join(root, f), src_root)
        text = open(os.path.join(root, f), errors="replace").read()
        text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
        texts[path] = text
        for m in def_re.finditer(text):
            defs.setdefault(m.group(1), path)

ident_re = re.compile(r"\b([A-Za-z_]\w*)\b")
seen, queue, why = set(), list(args), {}
while queue:
    f = queue.pop()
    if f in seen:
        continue
    seen.add(f)
    if f in stops:
        continue
    for ident in set(ident_re.findall(texts.get(f, ""))):
        g = defs.get(ident)
        if g and g != f and g not in seen:
            why.setdefault(g, (f, ident))
            queue.append(g)

for f in sorted(seen):
    src, ident = why.get(f, ("(start)", ""))
    mark = "STUB " if f in stops else ""
    print(f"{mark}{f:45} <- {src}:{ident}")
print(len(seen), "files", file=sys.stderr)
