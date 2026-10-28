#!/bin/sh
# Generates the headers the decomp's own build produces (constants, NARC
# indices, text bank IDs, Pokemon/item/move data tables, embedded graphics...)
# by running the decomp's meson build for just those targets, using the host
# compiler. The DS toolchain (Metrowerks under Wine) is not needed: stand-ins
# satisfy configure, and no DS code is compiled.
#
# Needs: Python >= 3.12 (PYTHON3), clang/clang++, ninja, pkg-config, libpng,
# git (the decomp fetches its subprojects).
#
#   tools/setup_decomp_gen.sh [decomp dir] [output dir]
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
DECOMP=$(cd "${1:-$here/../decomp}" && pwd)
OUT=${2:-$here/../decomp-gen}
PYTHON3=${PYTHON3:-python3}
MESON_DIR=${MESON_DIR:-$here/../tools-host/meson}

"$PYTHON3" -c 'import sys; sys.exit(sys.version_info < (3, 12))' || {
    echo "Python >= 3.12 is required (set PYTHON3=...)"; exit 1; }
[ -f "$MESON_DIR/meson.py" ] || git clone -q --depth=1 -b 1.12.0 https://github.com/mesonbuild/meson "$MESON_DIR"

# Kept next to the output: meson records these paths and may reconfigure later.
work="$OUT.tools"
mkdir -p "$work/bin" "$OUT"
# Stand-in for the Metrowerks ARM assembler: meson only asks for its version.
cat > "$work/bin/nasm" <<'SH'
#!/bin/sh
case "$1" in -v|--version) echo "Metrowerks C/C++ Assembler for ARM"; echo "Version 2.0 build 99"; exit 0;; esac
echo "stand-in assembler: not for real builds" >&2; exit 1
SH
chmod +x "$work/bin/nasm"
# python3 on PATH must be the >= 3.12 one for the decomp's scripts.
ln -sf "$(command -v "$PYTHON3")" "$work/bin/python3"

cat > "$work/cross.ini" <<INI
[binaries]
c = 'clang'
cpp = 'clang++'
nasm = '$work/bin/nasm'
ar = 'ar'
strip = 'strip'

[host_machine]
system = 'bare metal'
cpu_family = 'arm'
cpu = 'arm'
endian = 'little'
INI
cat > "$work/native.ini" <<INI
[binaries]
c = 'clang'
cpp = 'clang++'
ar = 'ar'
INI

export PATH="$work/bin:$PATH"
cd "$DECOMP"
# What the decomp's Makefile does before configuring.
python3 tools/scripts/lsfgen.py platinum.us/main.lsf.in --prefix-dir main.nef.p/ \
    --dest-lsf platinum.us/main.lsf --dest-c src/meson.build
# Configure checks for the Metrowerks runtime libraries; they are never used here.
for f in tools/metroskrew/lib/metroskrew/sdk/ds/2.0/sp2/msl/MSL_C/MSL_ARM/Lib/MSL_C_NITRO_Ai_LE.a \
         tools/metroskrew/lib/metroskrew/sdk/ds/2.0/sp2/msl/MSL_C++/MSL_ARM/Lib/MSL_CPP_NITRO_Ai_LE.a; do
    [ -f "$f" ] || { mkdir -p "$(dirname "$f")"; : > "$f"; }
done

if [ ! -f "$OUT/build.ninja" ]; then
    python3 "$MESON_DIR/meson.py" setup "$OUT" --cross-file "$work/cross.ini" --native-file "$work/native.ini"
fi
cd "$OUT"
ninja -t targets all | cut -d: -f1 | grep -E '\.(h|naix)$' | grep -v '\.p/' > "$work/targets"
# Stand-in toolchain paths are only valid for this checkout.
ninja $(cat "$work/targets")
echo "generated headers are in $OUT"
