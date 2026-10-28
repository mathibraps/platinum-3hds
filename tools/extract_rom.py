#!/usr/bin/env python3
"""Extract the NitroFS file system (plus arm9/arm7 binaries) from an .nds ROM.

Usage: extract_rom.py <rom.nds> <out_dir>
Files land at <out_dir>/<path as named in the ROM>, e.g. demo/title/titledemo.narc.
The ARM9/ARM7 binaries are not extracted: the port runs the decompiled C code instead.
"""
import os
import struct
import sys


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def walk_fnt(rom, fnt_off, fat_off, dir_id, prefix, out):
    entry = fnt_off + (dir_id & 0xFFF) * 8
    sub_off = u32(rom, entry)
    file_id = u16(rom, entry + 4)
    p = fnt_off + sub_off
    while True:
        tl = rom[p]
        p += 1
        if tl == 0:
            break
        name = rom[p:p + (tl & 0x7F)].decode("ascii")
        p += tl & 0x7F
        if tl & 0x80:
            child = u16(rom, p)
            p += 2
            walk_fnt(rom, fnt_off, fat_off, child, prefix + name + "/", out)
        else:
            start = u32(rom, fat_off + file_id * 8)
            end = u32(rom, fat_off + file_id * 8 + 4)
            out.append((prefix + name, start, end))
            file_id += 1


def main():
    rom_path, out_dir = sys.argv[1], sys.argv[2]
    with open(rom_path, "rb") as f:
        rom = f.read()
    if rom[0x0C:0x10] not in (b"CPUE",):
        sys.exit(f"unexpected game code {rom[0x0C:0x10]!r}; expected CPUE (Platinum US)")

    fnt_off, fat_off = u32(rom, 0x40), u32(rom, 0x48)

    files = []
    walk_fnt(rom, fnt_off, fat_off, 0xF000, "", files)

    os.makedirs(out_dir, exist_ok=True)
    real_out = os.path.realpath(out_dir)
    for path, start, end in files:
        dst = os.path.realpath(os.path.join(out_dir, path))
        if not dst.startswith(real_out + os.sep):
            sys.exit(f"refusing path outside output dir: {path}")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(rom[start:end])

    print(f"extracted {len(files)} files to {out_dir}")


if __name__ == "__main__":
    main()
