#!/bin/sh
# Restarts the Azahar emulator on pokeplatinum.3dsx (for tools/emu_perf.py).
#   tools/emu_run.sh [seconds to wait]
cd "$(dirname "$0")/.."
APP=${AZAHAR_APP:-../tools-host/azahar/azahar-macos-arm64-2126.2/Azahar.app}
pkill -9 -f "Azahar.app/Contents/MacOS/azahar" 2>/dev/null
sleep 1
open -n "$APP" --args -w "$PWD/pokeplatinum.3dsx"
sleep "${1:-25}"
