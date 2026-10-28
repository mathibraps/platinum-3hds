#!/bin/sh
# Plays a test script in Azahar again and again until the game crashes, for
# up to RUNS runs: each run lasts until the script's last frame or the first
# exception in Azahar's log, and a crash is described by
# tools/emu_crash_dump.py (into <out dir>/crash<run>.txt) before Azahar is
# closed.
#   tools/emu_crash_hunt.sh <script.txt> <out dir> [runs] [save.sav]
cd "$(dirname "$0")/.."
SCRIPT=$1
OUT=$2
RUNS=${3:-3}
SD="$HOME/Library/Application Support/Azahar/sdmc"
RPC=../tools-host/azahar/azahar-macos-arm64-2126.2/scripting
APP=${AZAHAR_APP:-../tools-host/azahar/azahar-macos-arm64-2126.2/Azahar.app}
LOG="$HOME/Library/Application Support/Azahar/log/azahar_log.txt"
frames=$(awk '$1 == "frames" {print $2}' "$SCRIPT" | tail -1)
mkdir -p "$OUT"
i=1
while [ $i -le $RUNS ]; do
    rm -f "$SD/pokeplat_test_save.sav"
    [ -n "$4" ] && cp "$4" "$SD/pokeplat_test_save.sav"
    cp "$SCRIPT" "$SD/pokeplat_script.txt"
    pkill -9 -f "Azahar.app/Contents/MacOS/azahar" 2>/dev/null
    sleep 2
    open -n "$APP" --args -w "$PWD/pokeplatinum.3dsx"
    sleep 15
    result=$(python3 -I - "$RPC" build-3ds/pokeplatinum.map "$LOG" "${frames:-0}" <<'PY'
import re, struct, sys, time
sys.path.insert(0, sys.argv[1])
from AzaharRPC import AzaharRPC
addr = next(int(m.group(1), 16) for m in (re.match(r"\s+0x([0-9a-f]+)\s+gPortFrameLog$", l) for l in open(sys.argv[2])) if m)
rpc, frame, quiet = AzaharRPC(timeout=3), 0, 0
while quiet < 30:
    if "LogException" in open(sys.argv[3], errors="replace").read():
        print(f"crash after frame {frame}")
        break
    try:
        header = rpc.read_memory(addr, 8)
        frame, quiet = struct.unpack("<II", header)[1], 0
    except Exception:
        quiet += 1
    if frame >= int(sys.argv[4]) - 700:  # it quits at the end, between two reads
        print(f"no crash ({frame} frames)")
        break
    time.sleep(10)
else:
    print(f"no reply from the emulator after frame {frame}")
PY
)
    echo "run $i: $result"
    case "$result" in
    crash*)
        python3 -I tools/emu_crash_dump.py "$RPC" "$LOG" build-3ds/pokeplatinum.elf > "$OUT/crash$i.txt" 2>&1
        cat "$OUT/crash$i.txt"
        pkill -9 -f "Azahar.app/Contents/MacOS/azahar"
        break
        ;;
    esac
    pkill -9 -f "Azahar.app/Contents/MacOS/azahar"
    i=$((i + 1))
done
rm -f "$SD/pokeplat_script.txt" "$SD/pokeplat_test_save.sav"
