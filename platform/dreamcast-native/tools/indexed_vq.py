"""Palette-independent VQ16 layout for M2T source indices, prepared offline.

At most256 distinct source 2x2 patterns: exact VQ, apart from 16-bit colour
quantization. Otherwise retain raw16, not an unmeasured lossy clustering rule.
The layout is reusable across every source luma. This is not a target loader.
"""
import struct
from prepare_material_crops import block_index


def layout(indices, width, height):
    if (any(n<8 or n>1024 or n&(n-1) for n in (width,height)) or
            len(indices)!=width*height//2):
        raise ValueError('invalid source index dimensions')
    quads = struct.unpack(f'<{len(indices)//2}H',indices)
    patterns = tuple(sorted(set(quads)))
    if len(patterns)<=256:
        count = (len(patterns)+3)&~3  # 32-byte aligned codebook/payload boundary.
        mapping = {q:i+256-count for i,q in enumerate(patterns)}
        pixels = bytearray(width*height//4)
        for y in range(height//2):
            for x in range(width//2):
                pixels[block_index(x,y,width//2,height//2)] = mapping[quads[y*(width//2)+x]]
        return width,height,patterns+(0,)*(count-len(patterns)),bytes(pixels)
    pixels = bytearray(width*height)
    for y in range(height):
        for x in range(width):
            q = quads[(y//2)*(width//2)+x//2]
            pixels[block_index(x,y,width,height)] = (q>>(4*((y&1)*2+(x&1))))&15
    return width,height,(),bytes(pixels)


def texture(prepared, colors, fmt):
    width,height,patterns,indices = prepared
    if fmt not in ('RGB565','ARGB1555') or len(colors)!=16:
        raise ValueError('invalid texture palette/format')
    palette = []
    for color in colors:
        if not 0<=color<=0xffffffff or color>>24 not in (0,255):
            raise ValueError('invalid source colour')
        b,g,r,a = color&255,(color>>8)&255,(color>>16)&255,color>>24
        if fmt=='RGB565':
            if a!=255: raise ValueError('RGB565 cannot preserve transparent source')
            palette.append((r>>3)<<11|(g>>2)<<5|(b>>3))
        else:
            palette.append((a>>7)<<15|(r>>3)<<10|(g>>3)<<5|(b>>3))
    if patterns:
        # Hardware's four word order is TL,BL,TR,BR; M2T is TL,TR,BL,BR.
        words = [palette[(q>>shift)&15] for q in patterns for shift in (0,8,4,12)]
        payload = struct.pack(f'<{len(words)}H',*words)+indices
    else:
        payload = struct.pack(f'<{len(indices)}H',*(palette[i] for i in indices))
    out = bytearray(32)+payload
    out += bytes((-len(out))&31)
    mode = ((fmt=='RGB565')<<27)|(bool(patterns)<<30)|((width.bit_length()-4)<<3)|(height.bit_length()-4)
    struct.pack_into('<4sI',out,0,b'DcTx',len(out))
    out[10] = len(patterns)-1 if patterns else 0
    struct.pack_into('<HHI',out,12,width,height,mode)
    return bytes(out)
