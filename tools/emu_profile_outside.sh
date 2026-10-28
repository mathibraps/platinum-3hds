#!/bin/sh
# Profiles the outdoor walk (tests/scripts/outside.txt, looped) in Azahar
# with tools/emu_profile.py: tools/emu_profile_outside.sh <samples> [out.txt] [--file=NAME]
cd "$(dirname "$0")/.."
SD="$HOME/Library/Application Support/Azahar/sdmc"
cp tests/saves/twinleaf_outside.sav "$SD/pokeplat_test_save.sav"
python3 -I - tests/scripts/outside.txt "$SD/pokeplat_script.txt" <<'PY'
import sys
lines = open(sys.argv[1]).read().splitlines()
out = [l.replace("frames 6000", "frames 40000") for l in lines]
for k in range(1, 14):
    for l in lines:
        p = l.split()
        if len(p) >= 3 and p[0] == "press" and p[1] in ("DOWN", "UP", "LEFT", "RIGHT", "B") and int(p[2]) >= 2100:
            n = p[3] if len(p) > 3 and not p[3].startswith("#") else "2"
            out.append(f"press {p[1]} {int(p[2]) + 2600 * k} {n}")
open(sys.argv[2], "w").write("\n".join(out) + "\n")
PY
pkill -9 -f "Azahar.app/Contents/MacOS/azahar"; sleep 1
open -n ../tools-host/azahar/azahar-macos-arm64-2126.2/Azahar.app --args -g 24689 -w "$PWD/pokeplatinum.3dsx"
sleep 12
python3 -I tools/emu_profile.py 24689 build-3ds/pokeplatinum.elf "${1:-1200}" 0.02 35 --lines $3 $4 > "${2:-out/prof_outside.txt}" 2>&1
pkill -9 -f "Azahar.app/Contents/MacOS/azahar"
rm -f "$SD/pokeplat_script.txt" "$SD/pokeplat_test_save.sav"
