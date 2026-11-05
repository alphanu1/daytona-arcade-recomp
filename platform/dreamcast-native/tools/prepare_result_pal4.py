"""Offline result glyph lighting, reserving palette banks0/1.

Four four-colour variants share each PAL4 bank. Four index-offset copies of
each glyph avoid runtime texture/palette updates. Output is local ROM data.
Usage: prepare_result_pal4.py SOURCE_MODELS NEW_DIRECTORY
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from inspect_intro_lights import inspect, read_lights
from prepare_materials import reachable_luma_bounds
from prepare_material_crops import block_index
from prepare_models import rows
from prepare_scene import check_pal4, read_tga


def prepare(source):
    proof = inspect(source)
    lights = read_lights(source/'material_lights.txt')
    bounds = reachable_luma_bounds(source)
    sources = {}
    for row in rows(source/'materials/materials.txt'):
        name, fmt, width, height, *key = row
        key = tuple(map(int, key))
        if fmt != 'ARGB1555' or (int(width), int(height)) != (32, 32):
            raise ValueError('result glyph must be 32x32 binary alpha')
        data = (source/'materials'/Path(name).with_suffix('.m2t')).read_bytes()
        pixels = [0]*1024
        for i, (quad,) in enumerate(struct.iter_unpack('<H', data[64:])):
            x, y = (i % 16)*2, (i//16)*2
            for n in range(4):
                pixels[(y+n//2)*32+x+n%2] = (quad >> (n*4)) & 15
        used = tuple(sorted(set(pixels)))
        if len(used) != 4:
            raise ValueError('result packing requires four source indices')
        expected = [struct.pack('<I', lights[key][i]) for i in pixels]
        if read_tga(source/'materials'/name, 32, 32) != expected:
            raise ValueError('result M2T indices disagree with source TGA')
        record = (used, tuple(pixels))
        if sources.setdefault(key[:4], record) != record:
            raise ValueError('glyph indices change with lighting')
    if len(sources) != 8 or sources.keys() != bounds.keys():
        raise ValueError('expected eight result material headers')
    palettes, textures, bindings = {}, {}, []
    for header in sorted(sources):
        used, pixels = sources[header]
        table = [(255, 255)]*256
        for luma in range(bounds[header], 256):
            colors = tuple(lights[(*header, luma)][i] for i in used)
            palette = palettes.setdefault(colors, len(palettes))
            bank, slot = divmod(palette, 4)
            if bank+2 >= 64:
                raise ValueError('result palettes exceed banks2..63')
            remap = {pen: i+slot*4 for i, pen in enumerate(used)}
            payload = bytearray(512)
            for y in range(32):
                for x in range(32):
                    at = block_index(x, y, 32, 32)
                    payload[at//2] |= remap[pixels[y*32+x]] << ((at & 1)*4)
            blob = bytearray(32)+payload
            struct.pack_into('<4sI', blob, 0, b'DcTx', len(blob))
            blob[11] = 15  # Bank-relative pens can reach15, including offset copies.
            struct.pack_into('<HHI', blob, 12, 32, 32, 0x28000012)
            texture = textures.setdefault(bytes(blob), len(textures))
            if texture >= 255:
                raise ValueError('texture index exceeds byte binding')
            table[luma] = (texture, bank+2)
        bindings.append(table)
    colors = [color for palette in palettes for color in palette]
    colors += [0]*(-len(colors) % 16)
    palette_blob = b'DPAL'+struct.pack('<I', len(colors))+struct.pack(f'<{len(colors)}I', *colors)
    blobs = list(textures)
    checked = 0
    # Existing independent decoder checks the actual binding/bank/texture
    # against every reachable source texel, not an encoder preview image.
    for header, table in zip(sorted(sources), bindings):
        _, pixels = sources[header]
        for luma in range(bounds[header], 256):
            texture, bank = table[luma]
            at = (bank-2)*16
            selected = b'DPAL'+struct.pack('<17I', 16, *colors[at:at+16])
            expected = [struct.pack('<I', lights[(*header, luma)][i]) for i in pixels]
            check_pal4(blobs[texture], selected, expected, 32)
            checked += 1024
    lines = ['/* Local offline result assets. Missing binding is {255,255}. */',
             '#include <stdint.h>', '#include <stdalign.h>',
             f'enum {{ NM_RESULT_TEXTURE_COUNT={len(blobs)}, NM_RESULT_PALETTE_FIRST=32, NM_RESULT_PALETTE_COLORS={len(colors)} }};',
             'static const uint16_t nm_result_material_headers[8][4] = {']
    lines += ['    {'+','.join(map(str, h))+'},' for h in sorted(sources)]
    lines += ['};', 'alignas(32) static const uint8_t nm_result_pal4_bindings[8][256][2] = {']
    for table in bindings:
        lines += ['    {'+','.join('{%d,%d}' % pair for pair in table)+'},']
    lines += ['};', '']
    metadata = dict(textures=len(blobs), texture_payload_bytes=len(blobs)*512,
        texture_file_bytes=sum(map(len, blobs)), palette_colors=len(colors),
        palette_first=32, palette_banks=len(colors)//16,
        compact_palettes=len(palettes), binding_bytes=8*256*2, checked_texels=checked,
        source_proofs=proof['checked_snapshot_proofs'],
        caveat='Exact at exported palette state. Live scene palette ownership, lighting calculation and hardware sampling remain unverified.')
    return blobs, palette_blob, '\n'.join(lines), metadata


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    blobs, palette, header, metadata = prepare(args.source)
    args.output.mkdir(exist_ok=False)
    for i, blob in enumerate(blobs):
        (args.output/f'texture_{i}.dt').write_bytes(blob)
    (args.output/'palettes.dpal').write_bytes(palette)
    (args.output/'result_pal4_data.h').write_text(header)
    metadata['palette_sha256'] = hashlib.sha256(palette).hexdigest()
    (args.output/'manifest.json').write_text(json.dumps(metadata, indent=2)+'\n')
    print(json.dumps(metadata, indent=2))
