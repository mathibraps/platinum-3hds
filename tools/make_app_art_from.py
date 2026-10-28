#!/usr/bin/env python3
"""Makes the HOME menu icon (48x48) and banner (256x128) from two pictures
you supply, which are not kept in the repo: a square cover picture, for the
icon (a square of it averaged down), and a logo with transparency, put over a
band of the cover picture (darkened) for the banner. macOS's sips scales the
pictures first.

usage: make_app_art_from.py <cover.png> <logo.png> <icon.png> <banner.png>
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from make_app_art import write_png  # noqa: E402
from png_diff import read_png  # noqa: E402

# The square of the cover the icon shows: left, top and size, in 1/192ths.
ICON_CROP = (0, 0, 192)
# The band of the cover (scaled to 256x256) behind the banner's logo: its top
# row, and how much of its light is kept.
BANNER_TOP = 40
BANNER_LIGHT = 0.45
# The logo's height on the banner.
LOGO_H = 112


def scaled(src, w, h, tmp):
    """src scaled to w x h by sips, read back as rows of (r, g, b, a)."""
    out = os.path.join(tmp, f"{w}x{h}.png")
    subprocess.run(["sips", "-s", "format", "png", "-z", str(h), str(w), src, "--out", out], check=True, capture_output=True)
    pw, ph, bpp, rows = read_png(out)
    return [[tuple(rows[y][x * bpp:x * bpp + 3]) + ((rows[y][x * bpp + 3],) if bpp == 4 else (255,)) for x in range(pw)] for y in range(ph)]


def size_of(src):
    out = subprocess.run(["sips", "-g", "pixelWidth", "-g", "pixelHeight", src], check=True, capture_output=True, text=True).stdout
    vals = [int(line.split(":")[1]) for line in out.splitlines() if "pixel" in line]
    return vals[0], vals[1]


def main():
    cover, logo, icon_path, banner_path = sys.argv[1:5]
    with tempfile.TemporaryDirectory() as tmp:
        # Icon: the crop of a 192x192 cover, 4x4 squares averaged.
        c = scaled(cover, 192, 192, tmp)
        x0, y0, size = ICON_CROP
        step = size / 48
        icon = []
        for y in range(48):
            line = []
            for x in range(48):
                xs = range(int(x0 + x * step), max(int(x0 + (x + 1) * step), int(x0 + x * step) + 1))
                ys = range(int(y0 + y * step), max(int(y0 + (y + 1) * step), int(y0 + y * step) + 1))
                px = [c[yy][xx] for yy in ys for xx in xs]
                line.append(tuple(sum(p[k] for p in px) // len(px) for k in range(3)) + (255,))
            icon.append(line)
        write_png(icon_path, 48, 48, icon)

        # Banner: the darkened band, the logo over it in the middle.
        band = scaled(cover, 256, 256, tmp)[BANNER_TOP:BANNER_TOP + 128]
        banner = [[(int(r * BANNER_LIGHT), int(g * BANNER_LIGHT), int(b * BANNER_LIGHT), 255) for (r, g, b, _) in row] for row in band]
        lw, lh = size_of(logo)
        w = round(lw * LOGO_H / lh)
        mark = scaled(logo, w, LOGO_H, tmp)
        ox, oy = (256 - w) // 2, (128 - LOGO_H) // 2
        for y in range(LOGO_H):
            for x in range(w):
                r, g, b, a = mark[y][x]
                if a and 0 <= ox + x < 256:
                    br, bg, bb, _ = banner[oy + y][ox + x]
                    banner[oy + y][ox + x] = ((r * a + br * (255 - a)) // 255, (g * a + bg * (255 - a)) // 255, (b * a + bb * (255 - a)) // 255, 255)
        write_png(banner_path, 256, 128, banner)


if __name__ == "__main__":
    main()
