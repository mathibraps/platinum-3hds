#!/bin/sh
# Azahar's own emulation speed during the outdoor walk, with extra script
# lines (e.g. "skip mix"): the mean of the one-second samples in the
# overworld (from 32 s on), for comparing what parts of the port cost the
# emulator on this computer.
#   tools/emu_speed_run.sh <out.csv> ["script line"...]
cd "$(dirname "$0")/.."
SD="$HOME/Library/Application Support/Azahar/sdmc"
RPC=../tools-host/azahar/azahar-macos-arm64-2126.2/scripting
out=$1
shift
cp tests/saves/twinleaf_outside.sav "$SD/pokeplat_test_save.sav"
cp tests/scripts/outside.txt "$SD/pokeplat_script.txt"
for line in "$@"; do echo "$line" >> "$SD/pokeplat_script.txt"; done
tools/emu_run.sh 2 > /dev/null
python3 -I tools/emu_speed.py "$RPC" 98 "$out" > /dev/null
pkill -9 -f "Azahar.app/Contents/MacOS/azahar"
rm -f "$SD/pokeplat_script.txt" "$SD/pokeplat_test_save.sav"
awk -F, 'NR>1 && $1>32 {n++; s+=$10; f+=$3; if ($10<0.985) b++} END {printf "mean speed %.1f%%, game fps %.1f, %d of %d seconds below 98.5%%\n", s/n*100, f/n, b, n}' "$out"
