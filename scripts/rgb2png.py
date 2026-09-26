#!/usr/bin/env python3
"""Convert a raw 32-bit frame dump (little-endian 0xAARRGGBB words, row by
row) to PNG, standard library only.

  rgb2png.py IN.rgb WIDTH OUT.png

Frame dumps are game output: keep them in git-ignored traces/.
"""
import struct
import sys
import zlib


def main():
    src, width, dst = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    data = open(src, "rb").read()
    height = len(data) // (4 * width)
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        row = data[y * width * 4:(y + 1) * width * 4]
        for x in range(width):
            b, g, r = row[x * 4], row[x * 4 + 1], row[x * 4 + 2]
            rows += bytes((r, g, b))

    def chunk(kind, body):
        c = struct.pack(">I", len(body)) + kind + body
        return c + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)


if __name__ == "__main__":
    main()
