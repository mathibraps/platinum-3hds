#!/bin/sh
# Regenerates game/stubs/: links the host build without the stubs, collects
# the undefined symbols and writes a stub for each (tools/gen_stubs.py).
# Run after adding or removing real implementations.
set -e
cd "$(dirname "$0")/.."
rm -f game/stubs/*.c
make -s -f Makefile.host -k -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null 2>&1 || true
LINK=$(make -s -f Makefile.host -n build-host/pokeplat_host.wasm | grep -- "-o build-host/pokeplat_host.wasm" | tail -1)
UNDEF=$(mktemp)
sh -c "$LINK -Wl,--error-limit=0" 2>&1 | grep -oE "undefined symbol: [A-Za-z0-9_]+" | sed 's/undefined symbol: //' | sort -u > "$UNDEF" || true
echo "$(wc -l < "$UNDEF") undefined symbols"
CC=$(make -s -f Makefile.host print-cc)
python3 -I tools/gen_stubs.py "$UNDEF" game/stubs $CC $(make -s -f Makefile.host print-includes) $(make -s -f Makefile.host print-game-cflags)
rm -f "$UNDEF"
