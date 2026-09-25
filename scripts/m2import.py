#!/usr/bin/env python3
"""Build the i960's memory images from the user's own ROM set.

  m2import.py ROMS.zip OUTDIR

Writes OUTDIR/program.bin (the i960 program region, 0x200000 bytes, mapped at
0x00000000), OUTDIR/main_data.bin (0x2000000 bytes, mapped at 0x02000000;
its upper half also at 0x06000000), OUTDIR/copro_data.bin (0x800000, the
TGP's banked data ROM), OUTDIR/copro_tables.bin (0x40000, the CPU board's
TGP function tables), OUTDIR/polygons.bin and OUTDIR/textures.bin (the
geometrizer's model ROM and the rasterizer's texture ROM, 0x1000000 each)
and OUTDIR/tgp_program.bin (the TGP program the i960
uploads at boot, cut from main_data), laid out as MAME's ROM_START(daytona93)
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
    ("mpr-16537.ic28", 0x36B7C35A, "copro_data", 0x000000, 0x200000),
    ("mpr-16536.ic29", 0x6D6AFED9, "copro_data", 0x000002, 0x200000),
    ("mpr-16523.ic16", 0x2F484D42, "polygons", 0x000000, 0x200000),
    ("mpr-16518.ic20", 0xDF683BF7, "polygons", 0x000002, 0x200000),
    ("mpr-16524.ic17", 0x34658BD7, "polygons", 0x400000, 0x200000),
    ("mpr-16519.ic21", 0xFACD1C81, "polygons", 0x400002, 0x200000),
    ("mpr-16525.ic18", 0xFB517521, "polygons", 0x800000, 0x200000),
    ("mpr-16520.ic22", 0xD66BD9BD, "polygons", 0x800002, 0x200000),
    ("epr-16646.ic19", 0x7BA9FD6B, "polygons", 0xC00000, 0x080000),
    ("epr-16645.ic23", 0x78FE0B8A, "polygons", 0xC00002, 0x080000),
    ("mpr-16522.25", 0x55D39A57, "textures", 0x000000, 0x200000),
    ("mpr-16521.24", 0xAF1934FB, "textures", 0x000002, 0x200000),
    ("mpr-16517.27", 0x4705D3DD, "textures", 0x800000, 0x200000),
    ("mpr-16516.26", 0xA260D45D, "textures", 0x800002, 0x200000),
    # MODEL2_CPU_BOARD: copro_tgp_tables
    ("opr-14742a.45", 0x90C6B117, "copro_tables", 0x000000, 0x020000),
    ("opr-14743a.46", 0xAE7F446B, "copro_tables", 0x000002, 0x020000),
]
COPIES = [("main_data", 0x900000, dst, 0x100000) for dst in (0xA00000, 0xB00000, 0xC00000, 0xD00000, 0xE00000, 0xF00000)]
SIZES = {"program": 0x200000, "main_data": 0x2000000, "copro_data": 0x800000, "copro_tables": 0x40000,
         "polygons": 0x1000000, "textures": 0x1000000}
# The TGP program: the i960 copies these words from main_data into the TGP's
# program RAM at boot (found by matching MAME's upload; checked by CRC here).
TGP_PROGRAM = ("main_data", 0x860020, 2024 * 4, 0xD6D611DD)


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
    region, off, size, crc = TGP_PROGRAM
    tgp = bytes(regions[region][off:off + size])
    if zlib.crc32(tgp) & 0xFFFFFFFF != crc:
        sys.exit("m2import: TGP program not where expected; refusing")
    os.makedirs(out, exist_ok=True)
    for name, data in list(regions.items()) + [("tgp_program", tgp)]:
        with open(os.path.join(out, name + ".bin"), "wb") as f:
            f.write(data)
    print(f"m2import: wrote {', '.join(n + '.bin' for n in list(regions) + ['tgp_program'])} to {out}")


if __name__ == "__main__":
    main()
