#!/usr/bin/env python3
"""Create patches for Metrowerks-only constructs in decomp sources.

  * lvalue casts:  (T *)p += n;  ->  p = (void *)((T *)p + (n));
  * \\uXXXX escapes for control characters in strings (Metrowerks stored them
    as raw bytes)  ->  \\xXX

Usage: make_compat_patches.py <decomp dir> <src-relative file> ...
Writes patches/<file>.patch (a file must not already have a patch).
"""
import os
import re
import subprocess
import sys
import tempfile

decomp = sys.argv[1]
LVALUE = re.compile(r"^(\s*)\((\w+) \*\)([\w\->\.\[\]]+) \+= (.*);(.*)$")

def fix_uescape(m):
    v = int(m.group(1), 16)
    return f"\\x{v:02X}" if v < 0x100 else m.group(0)

for rel in sys.argv[2:]:
    path = os.path.join(decomp, "src", rel)
    text = open(path, errors="replace").read()
    out = []
    for line in text.split("\n"):
        m = LVALUE.match(line)
        if m:
            ind, typ, var, rhs, rest = m.groups()
            line = f"{ind}{var} = (void *)(({typ} *){var} + ({rhs}));{rest} // port: was a Metrowerks lvalue cast"
        if '"' in line and "\\u" in line:
            line = re.sub(r"\\u([0-9A-Fa-f]{4})", fix_uescape, line)
        out.append(line)
    new = "\n".join(out)
    if new == text:
        print(f"{rel}: nothing to change", file=sys.stderr)
        continue
    patch_path = os.path.join("patches", rel + ".patch")
    if os.path.exists(patch_path):
        sys.exit(f"{patch_path} exists; merge by hand")
    os.makedirs(os.path.dirname(patch_path) or ".", exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        name = os.path.basename(rel)
        for d, t in (("a", text), ("b", new)):
            os.makedirs(os.path.join(tmp, d))
            open(os.path.join(tmp, d, name), "w").write(t)
        diff = subprocess.run(["diff", "-u", f"a/{name}", f"b/{name}"], cwd=tmp, capture_output=True, text=True).stdout
    open(patch_path, "w").write(diff)
    print(f"{patch_path}: {diff.count(chr(10) + '+') } changed line(s)")
