#!/bin/sh
# Makes the HOME menu art: the desktop harness plays to the title screen (3D
# Giratina on the bottom screen, the title music), then
# tools/make_app_art_from.py (with art/cover.png and art/logo.png, pictures
# you supply) or else tools/make_app_art.py (from the title screen) and
# tools/cut_jingle.py write build-3ds/meta/{icon.png,banner.png,jingle.wav}.
# Run by `make meta`.
set -e
cd "$(dirname "$0")/.."
OUT=build-3ds/meta
mkdir -p "$OUT/run"
make -s -f Makefile.host
printf 'frames 3600\ndump 3600\npress START 2500\n' > "$OUT/run/title.txt"
tools/run_host.sh build-host/pokeplat_host.wasm romfs "$OUT/run" --script "$OUT/run/title.txt" --widescreen \
    --wav "$OUT/run/title.wav" > "$OUT/run/log.txt"
if [ -f art/cover.png ] && [ -f art/logo.png ]; then
    python3 -I tools/make_app_art_from.py art/cover.png art/logo.png "$OUT/icon.png" "$OUT/banner.png"
else
    python3 -I tools/make_app_art.py "$OUT/run/frame3600_bottom.png" "$OUT/icon.png" "$OUT/banner.png" "${APP_NAME:-Platinum 3HDS}"
fi
python3 -I tools/cut_jingle.py "$OUT/run/title.wav" "$OUT/jingle.wav" 43.4 3.0
echo "HOME menu art in $OUT"
