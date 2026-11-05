"""Prepare one daytona93 course's wheel-contact data, not render triangles.

DNC3 retains ordered candidates, surface IDs, heading and recovery height/center.
The static vertex-center sum is folded offline with float32 rounding per step.
Output is user-owned ROM data: keep it under ignored build/, never distribute.
"""
import argparse
import hashlib
import math
from pathlib import Path
import struct

from inspect_course import IMAGES, MAIN_BASE, words

HEADER = struct.Struct("<8I")
QUAD = struct.Struct("<12f2I4f")
MAGIC = 0x33434e44
MAX_CANDIDATES = 33


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def prepare(program, main, copro, slot):
    if slot not in range(4):
        raise ValueError("course slot must be 0..3")
    descriptor = words(program, 0, 0x3a48+4*slot, 1)[0]
    pointers = words(main, MAIN_BASE, descriptor, 8)
    grid_address = words(main, MAIN_BASE, pointers[2], 1)[0]
    surface_address = words(main, MAIN_BASE, pointers[3], 1)[0]
    grid_bytes = struct.pack("<8192I", *words(main, MAIN_BASE, grid_address, 8192))
    grid = struct.unpack("<16384H", grid_bytes)
    list_base = words(copro, 0, (16+slot)*4, 1)[0]*4
    quad_base = words(copro, 0, (32+slot)*4, 1)[0]*4
    offsets, lists, ids, peak = {}, [], set(), 0
    for offset in sorted(set(grid)):
        count = words(copro, 0, list_base+4*offset, 1)[0]
        if count > MAX_CANDIDATES:
            raise ValueError("contact candidate count exceeds DNC3 bound")
        candidates = words(copro, 0, list_base+4*(offset+1), count) if count else ()
        if any(i >= 8192 for i in candidates):
            raise ValueError("contact surface ID exceeds DNC3 bound")
        offsets[offset] = len(lists)
        lists.extend((count, *candidates))
        ids.update(candidates)
        peak = max(peak, count)
    if not ids or ids != set(range(max(ids)+1)) or len(lists) > 65536:
        raise ValueError("noncontiguous surface IDs or oversized candidate table")
    count = len(ids)
    surface_offset = surface_address-MAIN_BASE
    if surface_offset < 0 or surface_offset+count > len(main):
        raise ValueError("out-of-image surface types")
    quads = bytearray()
    for i in range(count):
        xyz = struct.pack("<12I", *words(copro, 0, quad_base+64*i, 12))
        if not all(math.isfinite(v) for v in struct.unpack("<12f", xyz)):
            raise ValueError("nonfinite collision vertex")
        heading = words(copro, 0, quad_base+64*i+60, 1)[0]
        height = struct.pack("<I", words(copro, 0, quad_base+64*i+52, 1)[0])
        if not math.isfinite(struct.unpack("<f", height)[0]):
            raise ValueError("nonfinite recovery height")
        vertices = struct.unpack("<12f", xyz)
        try:
            center = [f32(f32(f32(vertices[a]+vertices[a+3])+vertices[a+6])+vertices[a+9])*0.25
                      for a in range(3)]
        except OverflowError as error:
            raise ValueError("nonfinite recovery center") from error
        if not all(math.isfinite(v) for v in center):
            raise ValueError("nonfinite recovery center")
        quads.extend(xyz + struct.pack("<2I", main[surface_offset+i], heading) + height + struct.pack("<3f", *center))
    payload = struct.pack("<16384H", *(offsets[v] for v in grid))
    payload += struct.pack(f"<{len(lists)}H", *lists)
    payload += bytes((-len(payload)) % 4)  # Float records remain aligned.
    payload += quads
    return HEADER.pack(MAGIC, HEADER.size+len(payload), slot, len(lists), count, peak, 0, 0)+payload


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", type=Path)
    parser.add_argument("slot", type=int, choices=range(4))
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    expected = dict(IMAGES, **{"copro_data.bin": (0x800000,
        "3b45d4d00af9dfdfb8789eeb18d2f0d527cf878d1f0d62c512bb17f7f300ac7e")})
    images = {}
    for name, (size, digest) in expected.items():
        path = args.images/name
        if path.stat().st_size != size:
            raise ValueError(f"incorrect daytona93 image size: {name}")
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"unrecognized daytona93 image: {name}")
        images[name] = data
    data = prepare(images["program.bin"], images["main_data.bin"], images["copro_data.bin"], args.slot)
    with args.output.open("xb") as output:
        output.write(data)
    h = HEADER.unpack_from(data)
    print(f"DNC3 course={h[2]} bytes={h[1]} list_words={h[3]} quads={h[4]} max_candidates={h[5]} VRAM=0 SHA256={hashlib.sha256(data).hexdigest()}")
