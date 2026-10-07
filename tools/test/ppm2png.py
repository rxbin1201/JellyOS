#!/usr/bin/env python3
"""Convert a binary PPM (QEMU screendump) into a PNG.

usage: ppm2png.py INPUT.ppm OUTPUT.png
"""

import struct
import sys
import zlib


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[2])
    data = open(sys.argv[1], "rb").read()
    parts = data.split(b"\n", 3)
    width, height = map(int, parts[1].split())
    pixels = parts[3]
    rows = b"".join(b"\x00" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    open(sys.argv[2], "wb").write(png)
    print(f"screenshot {width}x{height} -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
