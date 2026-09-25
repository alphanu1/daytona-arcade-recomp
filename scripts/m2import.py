#!/usr/bin/env python3
"""Build the i960's memory images from the user's own ROM set.

  m2import.py ROMS.zip OUTDIR

Writes OUTDIR/program.bin (the i960 program region, 0x200000 bytes, mapped at
0x00000000) and OUTDIR/main_data.bin (0x2000000 bytes, mapped at 0x02000000;
its upper half also at 0x06000000), laid out as MAME's ROM_START(daytona93)
loads them: ROM_LOAD32_WORD pairs interleave 16-bit words, then ROM_COPY
mirrors. Every file is checked against the CRC32 in that ROM_START; anything
else is refused.

Output is game data: OUTDIR must be git-ignored (build/ is). Never commit it.
"""

import os
import sys
import zipfile
import zlib

# (file, crc32, region, offset, size) from MAME model2.cpp ROM_START(daytona93)
# at dddd7368. LOAD32_WORD: offset 0 takes bytes 0-1 of each dword, offset 2
# bytes 2-3.
LOADS = [
    ("epr-16530a.12", 0x39E962B5, "program", 0x000000, 0x020000),
    ("epr-16531a.13", 0x693126EB, "program", 0x000002, 0x020000),
    ("mpr-16528.10", 0x9CE591F6, "main_data", 0x000000, 0x200000),
    ("mpr-16529.11", 0xF7095EAF, "main_data", 0x000002, 0x200000),
    ("mpr-16526.8", 0x5273B8B5, "main_data", 0x400000, 0x200000),
    ("mpr-16527.9", 0xFC4CB0EF, "main_data", 0x400002, 0x200000),
    ("epr-16534a.6", 0x1BB0D72D, "main_data", 0x800000, 0x100000),
    ("epr-16535a.7", 0x459A8BFB, "main_data", 0x800002, 0x100000),
]
COPIES = [("main_data", 0x900000, dst, 0x100000) for dst in (0xA00000, 0xB00000, 0xC00000, 0xD00000, 0xE00000, 0xF00000)]
SIZES = {"program": 0x200000, "main_data": 0x2000000}


def main():
    zpath, out = sys.argv[1], sys.argv[2]
    z = zipfile.ZipFile(zpath)
    regions = {name: bytearray(size) for name, size in SIZES.items()}
    for fname, crc, region, off, size in LOADS:
        data = z.read(fname)
        got = zlib.crc32(data) & 0xFFFFFFFF
        if len(data) != size or got != crc:
            sys.exit(f"m2import: {fname}: size {len(data):#x} crc {got:08x}, expected {size:#x} {crc:08x}; refusing")
        r = regions[region]
        for w in range(size // 2):
            r[off + w * 4] = data[w * 2]
            r[off + w * 4 + 1] = data[w * 2 + 1]
    for region, src, dst, size in COPIES:
        r = regions[region]
        r[dst:dst + size] = r[src:src + size]
    os.makedirs(out, exist_ok=True)
    for name, data in regions.items():
        with open(os.path.join(out, name + ".bin"), "wb") as f:
            f.write(data)
    print(f"m2import: wrote program.bin ({SIZES['program']:#x}) and main_data.bin ({SIZES['main_data']:#x}) to {out}")


if __name__ == "__main__":
    main()
