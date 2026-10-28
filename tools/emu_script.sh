#!/bin/sh
# Frame timing of a whole test script in Azahar: installs the script (and a
# save, if given) on the emulator's SD card, runs the 3DS build until the
# script ends, and writes every frame's times to a CSV file
# (tools/emu_frames_poll.py; summarize with tools/frames_summary.py).
#   tools/emu_script.sh <script.txt> <frames.csv> [save.sav]
cd "$(dirname "$0")/.."
SD="$HOME/Library/Application Support/Azahar/sdmc"
RPC=../tools-host/azahar/azahar-macos-arm64-2126.2/scripting
APP=${AZAHAR_APP:-../tools-host/azahar/azahar-macos-arm64-2126.2/Azahar.app}
rm -f "$SD/pokeplat_test_save.sav"
[ -n "$3" ] && cp "$3" "$SD/pokeplat_test_save.sav"
cp "$1" "$SD/pokeplat_script.txt"
pkill -9 -f "Azahar.app/Contents/MacOS/azahar" 2>/dev/null
sleep 1
open -n "$APP" --args -w "$PWD/pokeplatinum.3dsx"
frames=$(awk '$1 == "frames" {print $2}' "$1" | tail -1)
python3 -I tools/emu_frames_poll.py "$RPC" build-3ds/pokeplatinum.map "$2" 10 "${frames:-0}"
pkill -9 -f "Azahar.app/Contents/MacOS/azahar"
rm -f "$SD/pokeplat_script.txt" "$SD/pokeplat_test_save.sav"
