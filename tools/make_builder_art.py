#!/usr/bin/env python3
"""Makes the HOME menu banner, placeholder icon and banner jingle of the
builder's templates (builder/): original art, as nothing of the game may go
in the release. The builder replaces the icon with one made from the ROM's own
DS icon.

usage: make_builder_art.py <out dir>   (writes banner.png, icon.png, jingle.wav)
"""
import math
import os
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from make_app_art import FONT, write_png  # noqa: E402


def backdrop(w, h):
    """Deep violet fading to black from the middle, faint rays."""
    img = []
    for y in range(h):
        row = []
        for x in range(w):
            dx, dy = (x - w / 2) / (w / 2), (y - h / 2) / (h / 2)
            d = min(1.0, math.hypot(dx, dy * 0.8))
            ray = 0.5 + 0.5 * math.cos(math.atan2(dy, dx) * 9)
            k = (1 - d) ** 1.6
            r = 18 + 70 * k + 14 * ray * k
            g = 10 + 28 * k + 6 * ray * k
            b = 28 + 90 * k + 18 * ray * k
            row.append((int(r), int(g), int(b), 255))
        img.append(row)
    return img


def text(img, s, cx, y, scale, top, bottom):
    """Letters in a vertical gradient from top to bottom, outlined in black."""
    w = len(s) * 6 * scale - scale
    x0 = cx - w // 2
    mask = set()
    for i, ch in enumerate(s):
        for gy, line in enumerate(FONT[ch]):
            for gx, bit in enumerate(line):
                if bit == "1":
                    for sy in range(scale):
                        for sx in range(scale):
                            mask.add((x0 + (i * 6 + gx) * scale + sx, y + gy * scale + sy))
    h, wd = len(img), len(img[0])
    for (x, yy) in mask:
        for dx in range(-2, 3):
            for dy in range(-2, 3):
                px, py = x + dx, yy + dy
                if (px, py) not in mask and 0 <= py < h and 0 <= px < wd:
                    img[py][px] = (6, 2, 12, 255)
    for (x, yy) in mask:
        t = (yy - y) / (7 * scale)
        img[yy][x] = tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,)


def jingle(path, rate=32000):
    """A rising four-note chime, two seconds."""
    notes = [659.26, 830.61, 987.77, 1318.51]
    n = int(rate * 2.0)
    out = []
    for i in range(n):
        t = i / rate
        v = 0.0
        for k, f in enumerate(notes):
            t0 = k * 0.16
            if t >= t0:
                a = math.exp(-(t - t0) * 2.6)
                v += a * (math.sin(2 * math.pi * f * (t - t0)) + 0.25 * math.sin(4 * math.pi * f * (t - t0)))
        fade = min(1.0, (2.0 - t) / 0.3)
        out.append(int(max(-1.0, min(1.0, v * 0.22 * fade)) * 32767))
    data = struct.pack(f"<{n}h", *out)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
                + b"data" + struct.pack("<I", len(data)) + data)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    banner = backdrop(256, 128)
    text(banner, "PLATINUM", 128, 30, 4, (255, 255, 255), (150, 160, 190))
    text(banner, "3HDS", 128, 72, 3, (255, 215, 120), (200, 70, 60))
    write_png(os.path.join(out, "banner.png"), 256, 128, banner)
    icon = backdrop(48, 48)
    text(icon, "P", 24, 10, 4, (255, 255, 255), (150, 160, 190))
    write_png(os.path.join(out, "icon.png"), 48, 48, icon)
    jingle(os.path.join(out, "jingle.wav"))


if __name__ == "__main__":
    main()
