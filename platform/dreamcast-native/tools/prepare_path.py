"""Repack one authored daytona93 route, including its eight guard records.

Not a recorded trajectory. Keep ROM-derived output in ignored build/.
Only slots0..2 have the verified layout; slot3 has disabled/sentinel entries.
"""
import argparse
import hashlib
import math
from pathlib import Path
import struct
from inspect_course import IMAGES, MAIN_BASE, words

HEADER = struct.Struct("<8I")
MAGIC = 0x31504e44


def prepare(program, main, slot, route):
    if slot not in range(3) or route not in range(8):
        raise ValueError("path requires course0..2 and route0..7")
    descriptor = words(program,0,0x3a48+4*slot,1)[0]
    table = words(main,MAIN_BASE,descriptor+16,1)[0]
    address, count = words(main,MAIN_BASE,table+8*route,2)
    if not 0 < count <= 1162:
        raise ValueError("path count exceeds signed byte-offset domain")
    if route < 7:
        end = words(main,MAIN_BASE,table+8*(route+1),1)[0]
    else:
        next_desc = words(program,0,0x3a48+4*(slot+1),1)[0]
        end = words(main,MAIN_BASE,next_desc+16,1)[0]
    if address+28*(count+8) != end:
        raise ValueError("unexpected path guard-record extent")
    raw = struct.pack(f"<{7*(count+8)}I",*words(main,MAIN_BASE,address,7*(count+8)))
    for i in range(count+8):
        if not all(math.isfinite(v) for v in struct.unpack_from("<4f",raw,28*i)):
            raise ValueError("nonfinite path position or speed")
    return HEADER.pack(MAGIC,32+len(raw),slot,route,count,count+8,0,0)+raw


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images",type=Path)
    parser.add_argument("slot",type=int,choices=range(3))
    parser.add_argument("route",type=int,choices=range(8))
    parser.add_argument("output",type=Path)
    args = parser.parse_args()
    images = {}
    for name,(size,digest) in IMAGES.items():
        path = args.images/name
        if path.stat().st_size != size:
            raise ValueError(f"incorrect daytona93 image size: {name}")
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"unrecognized daytona93 image: {name}")
        images[name] = data
    data = prepare(images["program.bin"],images["main_data.bin"],args.slot,args.route)
    with args.output.open("xb") as f:
        f.write(data)
    print(f"DNP1 course={args.slot} route={args.route} bytes={len(data)} VRAM=0 SHA256={hashlib.sha256(data).hexdigest()}")
