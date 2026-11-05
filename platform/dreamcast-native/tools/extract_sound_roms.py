#!/usr/bin/env python3
"""The sound board images of a daytona93 ROM set, for tools/prepare_sfx.cpp.

    python3 extract_sound_roms.py ROMSET.zip OUTDIR

Writes OUTDIR/sound_program.bin (the 68000 program, 256 KiB, big-endian words:
the EPROMs hold byte-swapped words), OUTDIR/pcm1.bin and OUTDIR/pcm2.bin (the
two MultiPCMs' sample ROMs, 4 MiB each), as src/runtime/rom_import.cpp lays
them out. Every member is checked against its CRC-32 first. The sound program
is Revision A's (epr-16720/16721): MAME's daytona93 set carries it, not the
set's own epr-16489/16490 (src/runtime/native_sound_sequencer.cpp finds either
layout). The outputs are ROM data: keep OUTDIR under build/.
"""
import sys
import zipfile
import zlib
from pathlib import Path

# name, CRC-32, image, offset (rom_import.cpp kDaytona93/SHARED_LOADS)
MEMBERS = [
    ("epr-16720.7", 0x8E73CFFD, "sound_program", 0x000000),
    ("epr-16721.8", 0x1BB3B7B7, "sound_program", 0x020000),
    ("mpr-16491.32", 0x89920903, "pcm1", 0x000000),
    ("mpr-16492.33", 0x459E701B, "pcm1", 0x200000),
    ("mpr-16493.4", 0x9990DB15, "pcm2", 0x000000),
    ("mpr-16494.5", 0x600E1D6C, "pcm2", 0x200000),
]
SIZES = {"sound_program": 0x40000, "pcm1": 0x400000, "pcm2": 0x400000}


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    archive, out = Path(sys.argv[1]), Path(sys.argv[2])
    images = {name: bytearray(size) for name, size in SIZES.items()}
    with zipfile.ZipFile(archive) as z:
        members = {Path(n).name: n for n in z.namelist()}
        for name, crc, image, offset in MEMBERS:
            if name not in members:
                sys.exit(f"{archive}: no {name} (the sound ROMs of MAME's daytona93 set)")
            data = z.read(members[name])
            if zlib.crc32(data) != crc:
                sys.exit(f"{archive}: {name} has CRC-32 {zlib.crc32(data):08x}, expected {crc:08x}")
            if image == "sound_program":
                data = bytes(b for i in range(0, len(data), 2) for b in (data[i + 1], data[i]))
            images[image][offset:offset + len(data)] = data
    out.mkdir(parents=True, exist_ok=True)
    for name, data in images.items():
        (out / f"{name}.bin").write_bytes(data)
    print(f"sound images in {out}: " + ", ".join(f"{n}.bin {len(d)} bytes" for n, d in images.items()))


if __name__ == "__main__":
    main()
