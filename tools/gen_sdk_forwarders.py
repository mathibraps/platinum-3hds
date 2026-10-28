#!/usr/bin/env python3
"""Create a forwarding header for every NitroSDK header path.

Code includes SDK headers by many paths (<nitro/os.h>, <nitro/gx/g3.h>, ...).
Each one becomes a one-line header that includes the port's SDK replacement,
ntr_shim.h, so any SDK include resolves to it.

Usage: gen_sdk_forwarders.py <NitroSDK dir> <out include dir>
"""
import os
import sys

sdk, out = sys.argv[1], sys.argv[2]
root = os.path.join(sdk, "include")
count = 0
for dirpath, _, files in os.walk(os.path.join(root, "nitro")):
    for name in files:
        if not name.endswith(".h"):
            continue
        rel = os.path.relpath(os.path.join(dirpath, name), root)
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w") as f:
            f.write('#include "ntr_shim.h"\n')
        count += 1
# Headers the SDK generates at build time (nitro/fx/fx_const.h).
for dirpath, _, files in os.walk(os.path.join(sdk, "gen", "nitro")):
    for name in files:
        if name.endswith(".csv"):
            rel = os.path.join("nitro", os.path.relpath(dirpath, os.path.join(sdk, "gen", "nitro")), name[:-4] + ".h")
            dst = os.path.join(out, os.path.normpath(rel))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "w") as f:
                f.write('#include "ntr_shim.h"\n')
            count += 1
for top in ("nitro.h",):
    with open(os.path.join(out, top), "w") as f:
        f.write('#include "ntr_shim.h"\n')
print(f"generated {count} SDK forwarding headers")
