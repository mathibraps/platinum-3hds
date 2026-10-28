#!/bin/sh
# Snapshots of 3D-heavy scenes on the desktop harness, to check that changes
# to the geometry engine leave the output identical:
#   tools/geo_check.sh out/geo_ref        # make a reference
#   tools/geo_check.sh out/geo_new out/geo_ref   # compare against it
set -e
cd "$(dirname "$0")/.."
OUT=$1
mkdir -p "$OUT/opening" "$OUT/outside" "$OUT/newgame"
tools/run_host.sh build-host/pokeplat_host.wasm romfs "$OUT/opening" --script tests/scripts/opening.txt --widescreen > "$OUT/opening/log.txt"
cp tests/saves/twinleaf_outside.sav "$OUT/outside/test.sav"
(grep -v "^audio\|^frames" tests/scripts/outside.txt; echo "frames 4800"; echo "dump 2200 2600 3000 3400 3800 4200 4600 4800") > "$OUT/outside/script.txt"
tools/run_host.sh build-host/pokeplat_host.wasm romfs "$OUT/outside" --script "$OUT/outside/script.txt" --save "$OUT/outside/test.sav" --widescreen > "$OUT/outside/log.txt"
(grep -v "^frames\|^dump" tests/scripts/new_game.txt; echo "frames 16000"; echo "dump 1500 3000 5000 8000 10000 12000 14000 15050 15600 16000") > "$OUT/newgame/script.txt"
tools/run_host.sh build-host/pokeplat_host.wasm romfs "$OUT/newgame" --script "$OUT/newgame/script.txt" --widescreen > "$OUT/newgame/log.txt"
if [ -n "$2" ]; then
    n=0; d=0
    for f in "$2"/*/*.png; do
        rel=${f#"$2"/}
        n=$((n + 1))
        if ! cmp -s "$f" "$OUT/$rel"; then
            d=$((d + 1)); echo "differs: $rel"
        fi
    done
    echo "$n snapshots compared, $d differ"
fi
