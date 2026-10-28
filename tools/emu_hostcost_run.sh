#!/bin/sh
# Host CPU per emulated frame (tools/emu_hostcost.py) in the overworld of the
# outdoor walk, with extra script lines (e.g. "skip mix").
#   tools/emu_hostcost_run.sh ["script line"...]
cd "$(dirname "$0")/.."
SD="$HOME/Library/Application Support/Azahar/sdmc"
RPC=../tools-host/azahar/azahar-macos-arm64-2126.2/scripting
cp tests/saves/twinleaf_outside.sav "$SD/pokeplat_test_save.sav"
cp tests/scripts/outside.txt "$SD/pokeplat_script.txt"
for line in "$@"; do echo "$line" >> "$SD/pokeplat_script.txt"; done
tools/emu_run.sh 2 > /dev/null
python3 -I tools/emu_hostcost.py "$RPC" build-3ds/pokeplatinum.map "$(pgrep -f 'Azahar.app/Contents/MacOS/azahar' | head -1)" 38 88
pkill -9 -f "Azahar.app/Contents/MacOS/azahar"
rm -f "$SD/pokeplat_script.txt" "$SD/pokeplat_test_save.sav"
