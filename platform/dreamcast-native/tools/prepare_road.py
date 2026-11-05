"""Pack authored road profiles/maps for daytona93, folding immutable flags offline.

Not sampled poses or collision/render meshes. Output must be NEW and ignored.
"""
import argparse
import hashlib
import math
from pathlib import Path
import struct
from inspect_course import IMAGES, MAIN_BASE, road_profiles, race_grid, words
from prepare_drive import f32, tgp_reciprocal

HEADER = struct.Struct("<8I")
MAGIC = 0x37524e44


def prepare(program, main, slot, tables):
    if slot not in range(4):
        raise ValueError("road pack requires course slot0..3")
    laps = words(program,0,0x3950+4*slot,1)[0]
    if not 1 <= laps <= 255:
        raise ValueError("race lap count outside byte range")
    courses = road_profiles(program, main)
    course = courses[slot]
    grid = race_grid(program,main,courses)[slot]["cars"]
    count = len(course["profiles"])
    pointer = words(main,MAIN_BASE,0x2862444+4*slot,1)[0]
    offset = pointer-MAIN_BASE
    if pointer%2 or offset < 0 or offset+2*count > len(main):
        raise ValueError("profile flags outside source image")
    flags = struct.unpack_from(f"<{count}H",main,offset)
    profiles = struct.pack(f"<{41*count}I",*words(main,MAIN_BASE,course["profile_address"],41*count))
    mapping = b"".join(struct.pack("<2H",r["profile"],r["flags"]|flags[r["profile"]]) for r in course["mapping"])
    pointer = words(main,MAIN_BASE,0x2872974+4*slot,1)[0]
    checkpoint_count = words(main,MAIN_BASE,pointer,1)[0]
    if checkpoint_count > 31:
        raise ValueError("checkpoint count exceeds32-bit visited mask")
    checkpoints = [v&65535 for v in words(main,MAIN_BASE,pointer+4,checkpoint_count)] if checkpoint_count else []
    if any(v >= count for v in checkpoints):
        raise ValueError("checkpoint profile outside course")
    directory = words(main,MAIN_BASE,0x2870304+4*slot,1)[0]
    awards = b"".join(struct.pack("<257I",*words(main,MAIN_BASE,pointer,257))
                      for pointer in words(main,MAIN_BASE,directory,4))
    for table in (0x34fb0,0x34f70): # Subtract penalty, then add player-flag adjustment.
        pointer = words(program,0,table+4*slot,1)[0]
        awards += struct.pack("<4I",*words(program,0,pointer-0x200000,4))
    pointer = words(program,0,0x34e38+4*slot,1)[0]
    awards += struct.pack("<16I",*words(program,0,pointer-0x200000,16))
    pointer = words(program,0,0x34e28+4*slot,1)[0]-0x200000
    # Source uses32-bit reads at a2-byte stride but stores only the low byte.
    if pointer < 0 or pointer%2 or pointer+34 > len(program):
        raise ValueError("award signal table outside source image")
    awards += bytes(program[pointer+2*i] for i in range(16))
    assert len(awards) == 4224
    payload = profiles+mapping+awards+struct.pack(f"<{checkpoint_count}H",*checkpoints)
    payload += bytes((-len(payload))%32)
    for car in grid:
        payload += struct.pack("<5Ii2I",car["speed_parameter_bits"],car["profile"],car["previous"],
            (car["profile"]*28)&65535,car["lateral"],car["initial_lap"],car["route"],car["style"])
    def scalar(address):
        return struct.unpack("<f",struct.pack("<I",words(main,MAIN_BASE,address,1)[0]))[0]
    scale, default, rolling = (scalar(a) for a in (0x28b573c,0x28b57a4,0x28b574c))
    models = words(main,MAIN_BASE,0x286fab0,4)
    if not all(math.isfinite(v) for v in (scale,default,rolling)) or not all(models):
        raise ValueError("invalid race start constants")
    speeds = []
    for car in grid:
        raw = struct.unpack("<f",struct.pack("<I",car["speed_parameter_bits"]))[0]
        speed = struct.pack("<f",raw*scale) # One float32 TGP multiply, folded offline.
        if not math.isfinite(struct.unpack("<f",speed)[0]):
            raise ValueError("invalid folded grid speed")
        payload += speed
        speeds.append(struct.unpack("<f",speed)[0])
    payload += bytes((-len(payload))%32)
    inverse=tgp_reciprocal(tables,words(main,MAIN_BASE,0x28b57a4,1)[0])
    for speed in speeds:
        guided=f32(speed*inverse)
        if not math.isfinite(guided): raise ValueError("invalid folded guided scale")
        payload += struct.pack("<f",guided)
    payload += bytes((-len(payload))%32)
    payload += struct.pack("<4I2f2I",*models,default,rolling,0,0)
    return HEADER.pack(MAGIC,32+len(payload),slot,count,len(course["mapping"]),checkpoint_count,laps,len(grid))+payload


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images",type=Path)
    parser.add_argument("slot",type=int,choices=range(4))
    parser.add_argument("output",type=Path)
    args = parser.parse_args()
    images = {}
    expected=dict(IMAGES,**{"copro_tables.bin":(0x40000,"3163343896150d46e3280f71eb276cfeabd9190602c0bfdd53a6cb18d91f0b04")})
    for name,(size,digest) in expected.items():
        data = (args.images/name).read_bytes()
        if len(data) != size or hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"unrecognized daytona93 image: {name}")
        images[name] = data
    data = prepare(images["program.bin"],images["main_data.bin"],args.slot,images["copro_tables.bin"])
    with args.output.open("xb") as f:
        f.write(data)
    print(f"DNR7 slot={args.slot} bytes={len(data)} payload={len(data)-32} VRAM=0 SHA256={hashlib.sha256(data).hexdigest()}")
