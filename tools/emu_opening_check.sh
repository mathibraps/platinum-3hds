#!/bin/sh
# Captures frames of the opening in Azahar (test mode) into <out dir> and
# compares them with a reference capture (tools/png_diff.py --tol 8):
#   tools/emu_opening_check.sh <out dir> [reference dir]
cd "$(dirname "$0")/.."
SD="$HOME/Library/Application Support/Azahar/sdmc"
rm -rf "$SD/pokeplat_dumps"
(grep -v "^dump " tests/scripts/opening.txt; echo "dump 1500 1800 2100 2250 2400 2700"; echo "frames 2750") > "$SD/pokeplat_script.txt"
tools/emu_run.sh 65
mkdir -p "$1"
python3 -I tools/raw_to_png.py "$SD/pokeplat_dumps" "$1" > /dev/null
rm -f "$SD/pokeplat_script.txt"
pkill -9 -f "Azahar.app/Contents/MacOS/azahar"
if [ -n "$2" ]; then
    for f in "$2"/*.png; do
        printf "%s: " "$(basename "$f")"
        python3 -I tools/png_diff.py --tol 8 "$f" "$1/$(basename "$f")" 2>&1 | tail -1
    done
fi
