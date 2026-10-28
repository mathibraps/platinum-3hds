#!/bin/sh
# Frame timing of the outdoor walk (tests/scripts/outside.txt) in Azahar:
# installs the script and its save on the emulator's SD card, runs the 3DS
# build, then reports the frames that missed 60 fps (tools/emu_frames.py).
#   [SCRIPT=other.txt] tools/emu_outside.sh [seconds] [csv]
cd "$(dirname "$0")/.."
SD="$HOME/Library/Application Support/Azahar/sdmc"
RPC=../tools-host/azahar/azahar-macos-arm64-2126.2/scripting
cp tests/saves/twinleaf_outside.sav "$SD/pokeplat_test_save.sav"
cp "${SCRIPT:-tests/scripts/outside.txt}" "$SD/pokeplat_script.txt"
tools/emu_run.sh "${1:-85}"
python3 -I tools/emu_frames.py "$RPC" build-3ds/pokeplatinum.map ${2:+"$2"}
pkill -9 -f "Azahar.app/Contents/MacOS/azahar"
rm -f "$SD/pokeplat_script.txt" "$SD/pokeplat_test_save.sav"
