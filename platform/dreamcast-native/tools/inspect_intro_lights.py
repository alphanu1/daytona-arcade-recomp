"""Check compact offline light colours against M2T proofs; no target pack.

All lumas share each header's unchanged source indices. Palette/translation
state is fixed to the export; this is not proof it stays fixed throughout intro.
"""
import argparse
import json
from pathlib import Path
import struct

from prepare_models import rows
from prepare_materials import reachable_luma_bounds
from indexed_vq import layout


def read_lights(path):
    lights = {}
    for fields in rows(path):
        values = tuple(map(int, fields))
        if len(values) not in (6, 21):
            raise ValueError('invalid light row length')
        header, luma, colors = values[:4], values[4], values[5:]
        textured = bool(header[0] & 0x4000)
        if (any(not 0 <= h <= 65535 for h in header) or not 0 <= luma <= 255 or
                len(colors) != (16 if textured else 1) or
                any(not 0 <= c <= 0xffffffff for c in colors)):
            raise ValueError('invalid light identity/colours')
        for i, color in enumerate(colors):
            alpha = 0 if textured and (header[0] >> 13) & 3 == 3 and i == 15 else 255
            if color >> 24 != alpha:
                raise ValueError('source alpha changed')
        key = (*header, luma)
        if key in lights:
            raise ValueError('duplicate light binding')
        lights[key] = colors
    headers = {key[:4] for key in lights}
    if not headers or len(lights) != len(headers)*256:
        raise ValueError('incomplete light coverage')
    return lights


def inspect(source):
    lights = read_lights(source/'material_lights.txt')
    bounds = reachable_luma_bounds(source)
    if {key[:4] for key in lights} != bounds.keys():
        raise ValueError('light coverage differs from geometry headers')
    indices, proofs = {}, 0
    for fields in rows(source/'materials/materials.txt'):
        if len(fields) != 9:
            raise ValueError('invalid source material row')
        name, fmt, width, height, *key = fields
        width, height = int(width), int(height)
        key = tuple(map(int, key))
        if Path(name).name != name or key not in lights or not key[0]&0x4000:
            raise ValueError('invalid source texture identity')
        if (width,height)!=(32 << (key[0]&7),32 << ((key[0]>>3)&7)):
            raise ValueError('source dimensions disagree')
        proof = (source/'materials'/Path(name).with_suffix('.m2t')).read_bytes()
        if len(proof)!=64+width*height//2 or struct.unpack_from('<16I',proof)!=lights[key]:
            raise ValueError('light palette differs from source proof')
        previous = indices.setdefault(key[:4],proof[64:])
        if previous != proof[64:]:
            raise ValueError('texel indices depend on luma')
        proofs += 1
    for fields in rows(source/'solid_materials.txt'):
        values = tuple(map(int,fields))
        if len(values)!=6 or lights.get(values[:5])!=(values[5],):
            raise ValueError('solid light differs from source proof')
        proofs += 1
    textured = {h for h in bounds if h[0]&0x4000}
    if indices.keys()!=textured:
        raise ValueError('missing source texel indices')
    palettes = {v for k,v in lights.items() if k[:4] in textured}
    reachable = {v for k,v in lights.items() if k[:4] in textured and k[4]>=bounds[k[:4]]}
    layouts = {layout(data,32 << (h[0]&7),32 << ((h[0]>>3)&7)) for h,data in indices.items()}
    return dict(headers=len(bounds),textured_headers=len(textured),luma_bindings=len(lights),
                checked_snapshot_proofs=proofs,unique_palettes=len(palettes),
                reachable_unique_palettes=len(reachable),
                unique_index_bytes=sum(len(v) for v in set(indices.values())),
                exact_vq_layouts=sum(bool(p) for w,h,p,i in layouts),
                raw16_layouts=sum(not p for w,h,p,i in layouts),
                layout_payload_bytes=sum(2*len(p)+len(i) for w,h,p,i in layouts),
                exact_palette_bytes=64*len(palettes),reachable_palette_bytes=64*len(reachable),
                caveat='Offline exact colour coverage at exported palette state; no simultaneous bank/VRAM fit or target format claim.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    print(json.dumps(inspect(parser.parse_args().source),indent=2))
