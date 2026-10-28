#!/bin/sh
# Plays tests/scripts/battle.txt from tests/saves/twinleaf_outside.sav in
# Azahar several times, for crashes and dropped frames: for each run, the
# frame summary (tools/frames_summary.py), the file troubles
# (tools/emu_fs_watch.py) and any exceptions in Azahar's log, in <out dir>.
#   tools/emu_battle_runs.sh <out dir> [runs]
cd "$(dirname "$0")/.."
OUT=$1
RUNS=${2:-3}
RPC=../tools-host/azahar/azahar-macos-arm64-2126.2/scripting
LOG="$HOME/Library/Application Support/Azahar/log/azahar_log.txt"
mkdir -p "$OUT"
i=1
while [ $i -le $RUNS ]; do
    python3 -I tools/emu_fs_watch.py "$RPC" build-3ds/pokeplatinum.map 20 > "$OUT/fs$i.txt" &
    watch=$!
    tools/emu_script.sh tests/scripts/battle.txt "$OUT/frames$i.csv" tests/saves/twinleaf_outside.sav > "$OUT/run$i.txt" 2>&1
    kill $watch 2>/dev/null
    grep -iE "exception|unmapped|prefetch abort|data abort" "$LOG" | head -20 > "$OUT/exceptions$i.txt"
    echo "run $i: $(tail -1 "$OUT/run$i.txt"); $(python3 -I tools/frames_summary.py "$OUT/frames$i.csv" | head -1)"
    echo "  last frame $(tail -1 "$OUT/frames$i.csv" | cut -d, -f1); $(wc -l < "$OUT/exceptions$i.txt" | tr -d ' ') exception lines; $(tail -1 "$OUT/fs$i.txt")"
    i=$((i + 1))
done
