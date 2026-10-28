#!/usr/bin/env python3
"""Prepare a NitroSDK library source for GCC/Clang.

Removes Metrowerks `asm` function blocks (the port supplies C versions in
platform/src/ntr_sdk_asm.c) and turns plain C99 `inline` definitions, which
Metrowerks treated as static, into `static inline`.

Usage: strip_asm.py <in.c> <out.c>
"""
import re
import sys

src = open(sys.argv[1]).read()
out, i = [], 0
pattern = re.compile(r"^asm\s+[^{;]*\{", re.M)
while True:
    m = pattern.search(src, i)
    if not m:
        out.append(src[i:])
        break
    out.append(src[i:m.start()])
    depth, j = 0, m.end() - 1
    while j < len(src):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                break
        j += 1
    i = j + 1
text = "".join(out)
text = re.sub(r"^inline\s", "static inline ", text, flags=re.M)
open(sys.argv[2], "w").write(text)
