#!/bin/sh
# Syntax-checks decomp sources against the port (wasm32 target) in parallel and
# prints all diagnostics, prefixed with the file. Development aid.
#   tools/trial_compile.sh [file with list of src-relative paths] > report.txt
W=${WASI_SDK:-../tools-host/wasi-sdk-34.0-arm64-macos}
D=${DECOMP:-../decomp}
export W D
# Same include paths as sources.mk (decomp-generated folders, Wi-Fi headers).
if [ -z "$NTR_INC" ]; then
    NTR_INC=$(make -s -f Makefile.host print-includes 2>/dev/null)
    NTR_CFLAGS=$(make -s -f Makefile.host print-game-cflags 2>/dev/null)
    export NTR_INC NTR_CFLAGS
fi
if [ "$1" = "--one" ]; then
    "$W/bin/clang" --target=wasm32-wasip1 --sysroot="$W/share/wasi-sysroot" -fsyntax-only -ferror-limit=50 -w \
        $NTR_CFLAGS \
        $NTR_INC \
        -include "$D/include/pch/global_pch.h" "$D/src/$2" 2>&1 | sed "s|^|$2: |"
    exit 0
fi
LIST=$1
if [ -z "$LIST" ]; then
    LIST=$(mktemp)
    # The files the decomp's build actually compiles (its generated src/meson.build).
    grep -oE "'[^']+\.c'" "$D/src/meson.build" | tr -d "'" > "$LIST"
fi
xargs -P 10 -n 1 sh "$0" --one < "$LIST"
