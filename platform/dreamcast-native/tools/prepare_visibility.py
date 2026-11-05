"""Precompile authored scenery selection tables/cell ranges as DNV1.

Owned daytona93 images + complete cell source export. Output NEW/local only.
No recorded camera, player pose, machine scheduler or per-frame ROM decoding.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
from inspect_course import IMAGES, MAIN_BASE, road_profiles, words
from prepare_models import prepare as prepare_models, rows, world_cells

SCENERY_IMAGES=dict(IMAGES, **{'polygons.bin':(0x1000000,'e864d0f82cd11ae180ef5c3395b9d118e598cf5ea958ba6d588a4f33fa6a06fb')})

def drawable_model(polygons, address):
    # Same strip topology as export_models.hpp: two initial points, then
    # attribute/normal/p2/optional p3. Link0 advances but emits no face.
    # Empty placeholder objects are REAL table entries, absent from DNM.
    if not 0x800000<=address<0xc00000:
        raise ValueError('non-ROM scenery model')
    at=(address&0x3fffff)+6
    while 4*(at+1)<=len(polygons):
        attribute=struct.unpack_from('<I',polygons,4*at)[0]
        if not attribute&3: return False
        if (attribute>>8)&3: return True
        at+=10 if attribute&1 else 7
    raise ValueError('unterminated scenery placeholder')


def prepare(program, main, polygons, slot, sources):
    if slot not in range(4):
        raise ValueError("invalid scenery course")
    geometry, info=prepare_models(sources)
    world_cells(geometry,sources) # Full cell coverage/common-context validation.
    # Export manifest describes geometry, not course identity: verify every
    # cell's ordered ROM model ownership against the selected course table.
    from inspect_course import courses
    authored=courses(program,main)[slot]
    instances=list(rows(sources/'instances.txt'))
    ranges=list(rows(sources/'cells.txt'))
    for fields in ranges:
        cell,first,count=map(int,fields)
        if [int(v[1]) for v in instances[first:first+count]] != [o['model'] for o in authored['cells'][cell]['objects'] if drawable_model(polygons,o['model'])]:
            raise ValueError("cell model ownership disagrees with selected course")
    count=len(road_profiles(program,main)[slot]['profiles'])
    orders=program[0x17104:0x17136]
    if len(orders)!=50 or any(sorted(orders[25*i:25*i+25])!=list(range(25)) for i in range(2)):
        raise ValueError("invalid source scenery order")
    if struct.unpack_from('<25b',program,0x17136)!=tuple(i%5-2+16*(i//5-2) for i in range(25)):
        raise ValueError("unsupported cell grid offsets")
    probes=bytearray()
    for pointer in words(program,0,0x17658,4):
        for i in range(12):
            raw=words(main,MAIN_BASE,pointer+80*(i//6)+12*(i%6),3)
            x,z=struct.unpack('<2f',struct.pack('<2I',*raw[:2]))
            if not all(math.isfinite(v) and abs(v)<=384 for v in (x,z)) or raw[2]>>25:
                raise ValueError("invalid scenery probe")
            probes+=struct.pack('<3I',*raw)
    exclusions=[]
    pointer=words(program,0,0x17150+4*slot,1)[0]
    previous=-1
    for _ in range(65):
        lo,hi=struct.unpack('<2H',struct.pack('<I',*words(program,0,pointer,1)))
        if lo>hi or lo<=previous:
            raise ValueError("unordered scenery profile ranges")
        if lo>=count:
            break
        if hi>=count or len(exclusions)==64:
            raise ValueError("scenery profile range exceeds course")
        exclusions.append(struct.pack('<9I',*words(program,0,pointer,9)))
        previous=hi; pointer+=36
    else:
        raise ValueError("scenery profile range guard")
    cells=b''.join(struct.pack('<2H',int(r[1]),int(r[2])) for r in ranges)
    payload=orders+bytes(2)+probes+cells+b''.join(exclusions)
    assert len(payload)==1652+36*len(exclusions)
    return struct.pack('<8I',0x31564e44,32+len(payload),slot,count,len(exclusions),info['instances'],0,0)+payload


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('images',type=Path)
    parser.add_argument('slot',type=int,choices=range(4))
    parser.add_argument('sources',type=Path)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    images={}
    for name,(size,digest) in SCENERY_IMAGES.items():
        data=(args.images/name).read_bytes()
        if len(data)!=size or hashlib.sha256(data).hexdigest()!=digest:
            raise ValueError(f'unrecognized daytona93 image: {name}')
        images[name]=data
    data=prepare(images['program.bin'],images['main_data.bin'],images['polygons.bin'],args.slot,args.sources)
    with args.output.open('xb') as out: out.write(data)
    print(json.dumps(dict(bytes=len(data),sha256=hashlib.sha256(data).hexdigest()),indent=2))
