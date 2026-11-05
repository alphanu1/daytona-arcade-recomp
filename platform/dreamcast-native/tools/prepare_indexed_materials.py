"""DNT5 CPU-backed indexed layouts and prequantized palettes, local assets only.

Header <4s7I>: magic,size,resources,bindings,expanded_payload,max_decoded,
layouts,palettes. Resource <IIHHI>: layout_id,decoded_size,width,height,palette_id.
Bindings unchanged <6HI> (crop origin0 or solid ARGB). After32-byte padding:
64-byte layouts (DcTx header32,pattern_offset,index_offset,index_bytes,entries,
four zero words), palettes32 each, aligned layout patterns/indices. Offsets are
absolute. No pointer/native descriptor is serialized. DNT1..4 are unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from indexed_vq import layout, texture
from inspect_intro_lights import inspect, read_lights
from prepare_materials import HEADER, RESOURCE, BINDING, reachable_luma_bounds
from prepare_models import rows


def quantize(colors, fmt):
    if fmt not in ('RGB565','ARGB1555') or len(colors)!=16:
        raise ValueError('invalid indexed palette')
    result=[]
    for color in colors:
        if not 0<=color<=0xffffffff or color>>24 not in (0,255):
            raise ValueError('invalid indexed colour')
        b,g,r,a=struct.pack('<I',color)
        if fmt=='RGB565':
            if a!=255: raise ValueError('transparent RGB source')
            result.append((r>>3)<<11|(g>>2)<<5|(b>>3))
        else: result.append((a>>7)<<15|(r>>3)<<10|(g>>3)<<5|(b>>3))
    return struct.pack('<16H',*result)


def prepare(source):
    proof=inspect(source)
    lights=read_lights(source/'material_lights.txt')
    bounds=reachable_luma_bounds(source)
    images={}
    for fields in rows(source/'materials/materials.txt'):
        name,fmt,w,h,*key=fields
        header=tuple(map(int,key[:4]))
        indices=(source/'materials'/Path(name).with_suffix('.m2t')).read_bytes()[64:]
        images[header]=(fmt,layout(indices,int(w),int(h)))
    layouts={}; layout_headers=[]; palettes={}; resources={}; bindings=[]
    for key,colors in sorted(lights.items(),key=lambda item:struct.pack('<5H',*item[0])):
        header,luma=key[:4],key[4]
        if luma<bounds[header]: continue
        if not header[0]&0x4000:
            bindings.append(BINDING.pack(*key,65535,colors[0]));continue
        fmt,prepared=images[header]
        identity=(fmt,prepared)
        if identity not in layouts:
            layouts[identity]=len(layouts)
            layout_headers.append(texture(prepared,colors,fmt)[:32])
        li=layouts[identity]
        palette=quantize(colors,fmt)
        pi=palettes.setdefault(palette,len(palettes))
        ri=resources.setdefault((li,pi),len(resources))
        bindings.append(BINDING.pack(*key,ri,0))
    if not 1<=len(resources)<65535 or not 1<=len(bindings)<65535:
        raise ValueError('DNT5 exceeds16-bit identity bounds')
    if not 1<=len(layouts)<=256 or not 1<=len(palettes)<=16384:
        raise ValueError('DNT5 layout/palette bound')
    data=bytearray(32+16*len(resources))+b''.join(bindings)
    data+=bytes((-len(data))&31)
    layout_start=len(data)
    data+=bytes(64*len(layouts))+b''.join(palettes)
    sizes=[]
    for (fmt,prepared),li in layouts.items():
        w,h,patterns,indices=prepared
        pattern_offset=len(data)
        data+=struct.pack(f'<{len(patterns)}H',*patterns)
        data+=bytes((-len(data))&31)
        index_offset=len(data)
        data+=indices;data+=bytes((-len(data))&31)
        at=layout_start+64*li
        data[at:at+32]=layout_headers[li]
        struct.pack_into('<4I',data,at+32,pattern_offset,index_offset,len(indices),len(patterns))
        sizes.append(struct.unpack_from('<I',layout_headers[li],4)[0])
    for (li,pi),ri in resources.items():
        w,h=struct.unpack_from('<HH',layout_headers[li],12)
        RESOURCE.pack_into(data,32+16*ri,li,sizes[li],w,h,pi)
    payload=sum(sizes[li]-32 for li,pi in resources)
    # Intermediate file budget (key_pack.py and prepare_palette_materials.py turn it into the DNT6 the console
    # loads; the heap budget of make check bounds that one). The race cars with the hoods: 2,134,880 bytes.
    if len(data)>2304*1024: raise ValueError('DNT5 exceeds its 2.25 MiB file budget')
    HEADER.pack_into(data,0,b'DNT5',len(data),len(resources),len(bindings),payload,max(sizes),len(layouts),len(palettes))
    return bytes(data),dict(bytes=len(data),resources=len(resources),bindings=len(bindings),
        layouts=len(layouts),palettes=len(palettes),palette_bytes=32*len(palettes),
        metadata_bytes=16*(len(resources)+len(bindings)),expanded_payload=payload,
        staging_bytes=max(sizes),source_proofs=proof['checked_snapshot_proofs'],
        sha256=hashlib.sha256(data).hexdigest(),
        caveat='CPU-backed candidate; native DNT5 reader/cache integration pending, not resident GPU bytes.')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args()
    data,report=prepare(args.source)
    with args.output.open('xb') as out: out.write(data)
    print(json.dumps(report,indent=2))
