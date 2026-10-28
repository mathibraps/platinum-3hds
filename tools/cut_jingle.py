#!/usr/bin/env python3
"""Cuts the HOME menu banner's jingle (at most 3 seconds) out of a WAV the
desktop harness wrote (--wav), with short fades, raised to a peak of -1 dB.

usage: cut_jingle.py <in.wav> <out.wav> <start seconds> <seconds>
"""
import array
import struct
import sys


def main():
    src, dst, start, length = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
    data = open(src, "rb").read()
    rate = struct.unpack_from("<I", data, 24)[0]
    pcm = array.array("h")
    pcm.frombytes(data[44:])
    first, n = int(start * rate), int(min(length, 3.0) * rate)
    clip = pcm[2 * first:2 * (first + n)]
    n = len(clip) // 2
    fade_in, fade_out = int(0.05 * rate), int(0.4 * rate)
    peak = max(1, max(abs(v) for v in clip))
    gain = 0.89 * 32767 / peak
    out = array.array("h", [0] * len(clip))
    for i in range(n):
        k = gain * min(1.0, i / fade_in, (n - i) / fade_out)
        out[2 * i] = int(clip[2 * i] * k)
        out[2 * i + 1] = int(clip[2 * i + 1] * k)
    body = out.tobytes()
    header = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16)
    with open(dst, "wb") as f:
        f.write(header + b"data" + struct.pack("<I", len(body)) + body)


if __name__ == "__main__":
    main()
