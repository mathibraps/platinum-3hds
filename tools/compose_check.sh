#!/bin/sh
# Snapshots through the GPU composition path (--gpu-compose, what the 3DS
# draws) of 2D-heavy screens, to check that changes to layer decoding and
# composition leave the output identical:
#   tools/compose_check.sh out/compose_ref                     # make a reference
#   tools/compose_check.sh out/compose_new out/compose_ref     # compare against it
set -e
cd "$(dirname "$0")/.."
OUT=$1
mkdir -p "$OUT/menus" "$OUT/newgame" "$OUT/outside"
(grep -v "^frames\|^dump" tests/scripts/menus.txt; echo "frames 18400"; echo "dump 15104 15130 15200 15600 15960 15976 16000 16200 16540 16600 17200 17760 17800 18000 18400") > "$OUT/menus/script.txt"
tools/run_host.sh build-host/pokeplat_host.wasm romfs "$OUT/menus" --script "$OUT/menus/script.txt" --widescreen --gpu-compose > "$OUT/menus/log.txt"
(grep -v "^frames\|^dump\|^dumpevery" tests/scripts/new_game.txt; echo "frames 16000"; echo "dump 410 1000 1160 4172 4760 7004 7102 8360 10000 12000 15050 16000") > "$OUT/newgame/script.txt"
tools/run_host.sh build-host/pokeplat_host.wasm romfs "$OUT/newgame" --script "$OUT/newgame/script.txt" --widescreen --gpu-compose > "$OUT/newgame/log.txt"
cp tests/saves/twinleaf_outside.sav "$OUT/outside/test.sav"
(grep -v "^audio\|^frames" tests/scripts/outside.txt; echo "frames 3400"; echo "dump 2200 2600 3000 3400") > "$OUT/outside/script.txt"
tools/run_host.sh build-host/pokeplat_host.wasm romfs "$OUT/outside" --script "$OUT/outside/script.txt" --save "$OUT/outside/test.sav" --widescreen --gpu-compose > "$OUT/outside/log.txt"
if [ -n "$2" ]; then
    n=0; d=0
    for f in "$2"/*/*.png; do
        rel=${f#"$2"/}
        n=$((n + 1))
        if ! cmp -s "$f" "$OUT/$rel"; then
            d=$((d + 1))
            echo "differs: $rel"
        fi
    done
    echo "$n snapshots compared, $d differ"
fi
