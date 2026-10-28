#!/usr/bin/env python3
"""Extract NitroSDK definitions into one header for the port.

The port replaces NitroSDK's hardware-facing functions, but NitroSystem and
the game also rely on SDK constants, types, prototypes and small inline
helpers (fixed-point math, divider access, OAM attribute packing, 3D command
packing). This takes exactly those from the SDK headers:

  * macros, typedefs and prototypes, via the C preprocessor;
  * static inline helper functions, copied from the original header text so
    register names (reg_CP_DIVCNT, ...) stay symbolic and resolve to the
    port's emulated memory map (ntr_memmap.h). Helpers written in assembly
    (and helpers that call them) are skipped.

SDK debug assertions become no-ops. Names the port defines itself are skipped.

A third output is the ARM7 view of the sound headers, for the port's build of
the SDK's ARM7 sound driver (platform/include/ntr_snd7.h): its prototypes and
helpers, and the ARM7 sound registers. The ARM9 view's sound prototypes and
helpers are left out of that build (NTR_SND7), as the two differ.

Usage: gen_sdk_defs.py <cc> <NitroSDK dir> <out defs.h> <out inlines.h> <out ARM7 sound defs.h>
"""
import os
import re
import subprocess
import sys
import tempfile

# Headers whose own definitions are wanted (paths under include/).
TARGETS = [
    "nitro/platform.h",
    "nitro/fx/fx.h",
    "nitro/fx/fx_const.h",
    "nitro/fx/fx_cp.h",
    "nitro/fx/fx_vec.h",
    "nitro/fx/fx_trig.h",
    "nitro/fx/fx_mtx22.h",
    "nitro/fx/fx_mtx33.h",
    "nitro/fx/fx_mtx43.h",
    "nitro/fx/fx_mtx44.h",
    "nitro/cp/divider.h",
    "nitro/cp/sqrt.h",
    "nitro/math/math.h",
    "nitro/gx/gxcommon.h",
    "nitro/gx/g3.h",
    "nitro/gx/g3x.h",
    "nitro/gx/g2_oam.h",
    "nitro/gx/struct_2d.h",
    "nitro/mi/memory.h",
    "nitro/hw/ARM9/ioreg_G3.h",
    "nitro/hw/ARM9/ioreg_G3X.h",
    "nitro/hw/ARM9/ioreg_GX.h",
    "nitro/hw/ARM9/ioreg_GXS.h",
    "nitro/hw/ARM9/ioreg_G2.h",
    "nitro/hw/ARM9/ioreg_G2S.h",
    "nitro/hw/ARM9/ioreg_CP.h",
]

# Defined by the port itself (ntr_shim.h, ntr_memmap.h) in its own way.
SKIP_NAMES = {"FSFile", "FSArchive", "FSSeekFileMode", "FSOverlayID", "FSFileID"}

# Whole SDK areas whose definitions are wanted (every header under these).
TARGET_DIRS = [
    "nitro/os/", "nitro/rtc/", "nitro/wm/", "nitro/card/", "nitro/ctrdg/", "nitro/spi/", "nitro/math/",
    "nitro/mi/", "nitro/pad/", "nitro/env/", "nitro/std/", "nitro/prc/", "nitro/pxi/", "nitro/cp/",
    "nitro/dgt/", "nitro/init/", "nitro/cht/", "nitro/exi/", "nitro/ext/", "nitro/mb/", "nitro/vib/",
    "nitro/wbt/", "nitro/wvr/", "nitro/fx/", "nitro/gx/", "nitro/hw/", "nitro/snd/",
]

# Only constants are wanted from register headers (no register accessors).
REGISTER_HEADERS = ("ioreg_",)

# SDK_CW: some SDK headers only describe the Metrowerks configuration. It only
# affects which definitions are extracted, never how the port is compiled.
DEFINES = ["-DSDK_ARM9", "-DSDK_FINALROM", "-DSDK_TS", "-DSDK_4M", "-DSDK_CODE_ARM", "-DNNS_FINALROM", "-DSDK_CW"]

# The ARM7 view of the sound library (third output).
SND_DIR = "nitro/snd/"
ARM7_TARGETS = ["nitro/hw/ARM7/ioreg_SND.h"]
ARM7_TARGET_DIRS = ["nitro/snd/common/"]
ARM7_DEFINES = [d for d in DEFINES if d != "-DSDK_ARM9"] + ["-DSDK_ARM7"]


def preprocess(cc, sdk, gen_inc, targets=TARGETS, target_dirs=TARGET_DIRS, defines=DEFINES):
    with tempfile.TemporaryDirectory() as tmp:
        probe = os.path.join(tmp, "probe.c")
        with open(probe, "w") as f:
            f.write("".join(f"#include <{t}>\n" for t in targets))
            for d in target_dirs:
                if d.endswith("/"):
                    umbrella = d.rstrip("/") + ".h"
                    if os.path.exists(os.path.join(sdk, "include", umbrella)):
                        f.write(f"#include <{umbrella}>\n")
                    elif targets is ARM7_TARGETS:
                        for name in sorted(os.listdir(os.path.join(sdk, "include", d))):
                            f.write(f"#include <{d}{name}>\n")
                else:
                    f.write(f"#include <{d}>\n")
        return subprocess.run(
            [*cc.split(), "-E", "-dD", "-fsigned-char", *defines, "-I", gen_inc, "-I", os.path.join(sdk, "include"), probe],
            check=True, capture_output=True, text=True,
        ).stdout


def target_of(path, targets=TARGETS, target_dirs=TARGET_DIRS):
    for t in targets:
        if path.endswith("/include/" + t) or path.endswith("/" + t):
            return t
    if "/include/" in path:
        rel = path.split("/include/", 1)[1]
        for d in target_dirs:
            if rel == d or (d.endswith("/") and (rel.startswith(d) or rel == d.rstrip("/") + ".h")):
                return rel
    return None


def statements(text, targets=TARGETS, target_dirs=TARGET_DIRS):
    """Splits preprocessed code into top-level statements.

    Yields (target header or None, source line, text). Macro definitions are
    single statements; other code is split at a top-level ';' or at the end of
    a top-level function body.
    """
    cur_file, cur_line = "", 0
    buf, depth, start = [], 0, None

    for raw in text.splitlines():
        m = re.match(r'^# (\d+) "([^"]+)"', raw)
        if m:
            cur_line, cur_file = int(m.group(1)), m.group(2)
            continue
        if raw.startswith("#define ") and depth == 0 and not "".join(buf).strip():
            yield target_of(cur_file, targets, target_dirs), cur_line, raw
            cur_line += 1
            continue
        if raw.startswith("#"):
            cur_line += 1
            continue

        for ch in raw + "\n":
            if start is None and not ch.isspace():
                start = (target_of(cur_file, targets, target_dirs), cur_line)
            buf.append(ch)
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    stmt = "".join(buf)
                    head = stmt.split("{", 1)[0]
                    if re.search(r"\)\s*$", head.strip()) and "=" not in head:
                        yield start[0], start[1], stmt.strip()
                        buf, start = [], None
            elif ch == ";" and depth == 0:
                yield (start[0], start[1], "".join(buf).strip()) if start else (None, cur_line, "")
                buf, start = [], None
        cur_line += 1


def drop_orphan_conditionals(text):
    """Removes the rest of a conditional the copy started inside of: a
    function written as `#ifdef X static inline #else __inline #endif f(...)`
    is copied from its first branch, leaving an unopened #else ... #endif."""
    out, depth, skipping = [], 0, False
    for ln in text.split("\n"):
        d = ln.strip()
        if skipping:
            if d.startswith("#endif"):
                skipping = False
            continue
        if d.startswith("#if"):
            depth += 1
        elif d.startswith("#endif"):
            if depth == 0:
                continue
            depth -= 1
        elif d.startswith(("#else", "#elif")) and depth == 0:
            skipping = True
            continue
        out.append(ln)
    return "\n".join(out)


def original_function(sdk, target, line):
    """Copies a function definition from the original header, starting at line."""
    with open(os.path.join(sdk, "include", target)) as f:
        lines = f.read().splitlines()
    text = "\n".join(lines[line - 1:])
    depth, seen = 0, False
    for i, ch in enumerate(text):
        if ch == "{":
            depth += 1
            seen = True
        elif ch == "}":
            depth -= 1
            if seen and depth == 0:
                return drop_orphan_conditionals(text[:i + 1])
    return None


def extract(sdk, text, targets=TARGETS, target_dirs=TARGET_DIRS):
    """Returns macros, typedefs, prototypes and inline helpers; prototypes
    and helpers as (target header, text)."""
    macros, typedefs, prototypes, inlines = [], [], [], []
    # Helpers left out (assembly), and helpers that depend on them.
    dropped = set()
    seen_inline = set()
    for target, line, stmt in statements(text, targets, target_dirs):
        if target is None or not stmt:
            continue
        if stmt.startswith("#define "):
            macros.append(stmt)
            continue
        if target.split("/")[-1].startswith(REGISTER_HEADERS):
            continue
        if re.match(r"^(struct|union|enum)\s+\w+\s*\{", stmt) and stmt.endswith(";"):
            typedefs.append(stmt)
            continue
        if stmt.startswith("typedef"):
            name = re.findall(r"(\w+)\s*(?:\[[^\]]*\])?\s*;\s*$", stmt)
            if not (name and name[-1] in SKIP_NAMES):
                typedefs.append(stmt)
            continue
        if stmt.startswith("extern") and "(" not in stmt.split("=")[0]:
            prototypes.append((target, stmt))  # extern data, e.g. FX_SinCosTable_
            continue
        fn = re.match(r"^([\w\s\*]+?)\b(\w+)\s*\(", stmt)
        if not fn:
            continue
        name = fn.group(2)
        if name in SKIP_NAMES:
            continue
        if stmt.endswith(";"):
            if "static" not in fn.group(1):
                prototypes.append((target, stmt))
            continue
        if ("static" in fn.group(1) or "inline" in fn.group(1)) and name not in seen_inline:
            body = original_function(sdk, target, line)
            if body and "static" not in fn.group(1):
                # Metrowerks "inline" has internal linkage; C99 needs "static inline".
                body = re.sub(r"\binline\b", "static inline", body, count=1)
            words = re.findall(r"\w+", body.split("{", 1)[1]) if body else []
            if body and "asm" not in words and not (set(words) & dropped):
                seen_inline.add(name)
                inlines.append((target, f"/* {target} */\n{body}"))
            else:
                dropped.add(name)
    return macros, typedefs, prototypes, inlines


def macro_lines(macros):
    out = []
    for m in macros:
        mm = re.match(r"#define (\w+)(\([^)]*\))?\s*(.*)$", m)
        name, params, body = mm.group(1), mm.group(2), mm.group(3)
        if name in SKIP_NAMES or re.match(r"NITRO_\w+_H_?$", name) or name.endswith("_H_"):
            continue
        if name.endswith("_ASSERT") or name.endswith("_ASSERTMSG"):
            body = "((void)0)"
        out.append(f"#ifndef {name}\n#define {name}{params or ''} {body}\n#endif")
    return out


def arm9_only(items):
    """The ARM9 view's sound declarations, which the ARM7 driver build replaces."""
    out = []
    for target, text in items:
        if target.startswith(SND_DIR):
            out.append(f"#ifndef NTR_SND7\n{text}\n#endif")
        else:
            out.append(text)
    return out


def main():
    cc, sdk, out_path, inl_path, snd7_path = sys.argv[1:6]
    with tempfile.TemporaryDirectory() as gen_inc:
        fx_dir = os.path.join(gen_inc, "nitro", "fx")
        os.makedirs(fx_dir)
        subprocess.run([sys.executable, os.path.join(sdk, "gen/nitro/fx/gen_fx_const.py"),
                        os.path.join(sdk, "gen/nitro/fx/fx_const.csv"), os.path.join(fx_dir, "fx_const.h")], check=True)
        text = preprocess(cc, sdk, gen_inc)
        text7 = preprocess(cc, sdk, gen_inc, ARM7_TARGETS, ARM7_TARGET_DIRS, ARM7_DEFINES)

    macros, typedefs, prototypes, inlines = extract(sdk, text)
    macros7, typedefs7, prototypes7, inlines7 = extract(sdk, text7, ARM7_TARGETS, ARM7_TARGET_DIRS)

    out = ["/* Generated by tools/gen_sdk_defs.py from NitroSDK headers. Do not edit. */",
           "#ifndef NTR_SDK_DEFS_H", "#define NTR_SDK_DEFS_H", ""]
    out += macro_lines(macros)
    # Every SDK debug assertion (from any header) becomes a no-op.
    asserts = set()
    for dirpath, _, files in os.walk(os.path.join(sdk, "include")):
        for name in files:
            if name.endswith(".h"):
                with open(os.path.join(dirpath, name), errors="replace") as f:
                    asserts.update(re.findall(r"#\s*define\s+(\w+_ASSERT\w*)\s*\(", f.read()))
    for name in sorted(asserts):
        out.append(f"#ifndef {name}\n#define {name}(...) ((void)0)\n#endif")
    out += ["", *typedefs, "", *arm9_only(prototypes), "", "#endif /* NTR_SDK_DEFS_H */", ""]
    inl = ["/* Generated by tools/gen_sdk_defs.py from NitroSDK headers. Do not edit. */",
           "#ifndef NTR_SDK_INLINES_H", "#define NTR_SDK_INLINES_H", "", *arm9_only(inlines), "",
           "#endif /* NTR_SDK_INLINES_H */", ""]
    # The ARM7 view: the types are the same as the ARM9 view's (any that
    # differ would be redefined here and fail to compile).
    seen = set(typedefs)
    snd7 = ["/* Generated by tools/gen_sdk_defs.py from NitroSDK headers (ARM7 view of the sound",
            " * library, see platform/include/ntr_snd7.h). Do not edit. */",
            "#ifndef NTR_SDK_DEFS7_H", "#define NTR_SDK_DEFS7_H", "", *macro_lines(macros7), "",
            *[t for t in typedefs7 if t not in seen], "", *[p for _, p in prototypes7], "",
            *[i for _, i in inlines7], "", "#endif /* NTR_SDK_DEFS7_H */", ""]

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        f.write("\n".join(out))
    with open(inl_path, "w") as f:
        f.write("\n".join(inl))
    with open(snd7_path, "w") as f:
        f.write("\n".join(snd7))
    print(f"SDK defs: {len(macros)} macros, {len(typedefs)} typedefs, {len(prototypes)} prototypes, {len(inlines)} inline helpers")


if __name__ == "__main__":
    main()
