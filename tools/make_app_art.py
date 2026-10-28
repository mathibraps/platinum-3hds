#!/usr/bin/env python3
"""Makes the HOME menu icon (48x48) and banner (256x128) from a desktop
harness snapshot of the title screen, whose bottom screen shows the 3D
Giratina: the banner is a 1:1 crop with the app's name over it, the icon a
crop around Giratina averaged down to 48x48. The art comes from the game
data you extracted, so it is made at build time and not kept in the repo.

usage: make_app_art.py <title bottom screen .png> <icon.png> <banner.png> <name>
"""
import struct
import sys
import zlib

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from png_diff import read_png  # noqa: E402

# Where Giratina is in the 320x240 widescreen bottom screen.
BANNER_CROP = (32, 66)  # 256x128 from here (above the credit line)
ICON_CROP = (126, 58, 126)  # x, y, size of the square averaged down to 48x48

# A blocky 5x7 font for the name (uppercase, digits, space).
FONT = {
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "I": ["11111", "00100", "00100", "00100", "00100", "00100", "11111"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "N": ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "3": ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    " ": ["00000"] * 7,
}


def write_png(path, w, h, pixels):
    """pixels: rows of (r, g, b, a)."""
    raw = b"".join(b"\x00" + bytes(c for p in row for c in p) for row in pixels)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def pixel(rows, bpp, x, y):
    r, g, b = rows[y][x * bpp:x * bpp + 3]
    return (r, g, b, 255)


def draw_text(img, text, cx, y, scale):
    """Silver letters with a black outline, centred on cx."""
    w = len(text) * 6 * scale - scale
    x0 = cx - w // 2
    mask = set()
    for i, ch in enumerate(text):
        for gy, line in enumerate(FONT[ch]):
            for gx, bit in enumerate(line):
                if bit == "1":
                    for sy in range(scale):
                        for sx in range(scale):
                            mask.add((x0 + (i * 6 + gx) * scale + sx, y + gy * scale + sy))
    h = len(img)
    for (x, yy) in mask:
        for dx in (-2, -1, 0, 1, 2):
            for dy in (-2, -1, 0, 1, 2):
                px, py = x + dx, yy + dy
                if (px, py) not in mask and 0 <= py < h and 0 <= px < len(img[0]):
                    img[py][px] = (8, 0, 16, 255)
    for (x, yy) in mask:
        t = (yy - y) / (7 * scale)  # light at the top, cooler below
        c = (int(250 - 70 * t), int(250 - 60 * t), int(255 - 20 * t), 255)
        img[yy][x] = c


def main():
    src, icon_path, banner_path, name = sys.argv[1:5]
    w, h, bpp, rows = read_png(src)

    bx, by = BANNER_CROP
    banner = [[pixel(rows, bpp, bx + x, by + y) for x in range(256)] for y in range(128)]
    # Darken toward the top so the name reads over the wings.
    for y in range(0, 36):
        k = 0.4 + 0.6 * y / 36
        banner[y] = [(int(r * k), int(g * k), int(b * k), a) for (r, g, b, a) in banner[y]]
    draw_text(banner, name.upper(), 128, 8, 2)
    write_png(banner_path, 256, 128, banner)

    ix, iy, size = ICON_CROP
    icon = []
    for y in range(48):
        line = []
        for x in range(48):
            x0, x1 = ix + x * size // 48, ix + (x + 1) * size // 48
            y0, y1 = iy + y * size // 48, iy + (y + 1) * size // 48
            acc = [0, 0, 0]
            n = 0
            for yy in range(y0, y1):
                for xx in range(x0, x1):
                    p = pixel(rows, bpp, xx, yy)
                    acc[0] += p[0]
                    acc[1] += p[1]
                    acc[2] += p[2]
                    n += 1
            line.append((acc[0] // n, acc[1] // n, acc[2] // n, 255))
        icon.append(line)
    write_png(icon_path, 48, 48, icon)


if __name__ == "__main__":
    main()
