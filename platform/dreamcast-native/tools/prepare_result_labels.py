"""Extract authored result labels/font pens offline from verified daytona93.

Usage: prepare_result_labels.py ROM_CACHE NEW_DIRECTORY
Default output is local ROM-derived pen planes. --colors SOURCE_ORACLE_FILE
also creates lossless PAL4 candidates using actual colour-translation/gamma,
never guessed RGB555. Full result scene context remains a separate check.
--name-scene extracts four static name-screen blocks and countdown glyphs; --colors must then come
from --export-name-label-colors. Full scene context/animated models are separate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from inspect_course import IMAGES, MAIN_BASE, words
from prepare_material_crops import block_index
from prepare_scene import check_pal4, tga


def extract(program, main, name_scene=False):
    for name, blob in [('program.bin', program), ('main_data.bin', main)]:
        size, digest = IMAGES[name]
        if len(blob) != size or hashlib.sha256(blob).hexdigest() != digest:
            raise ValueError(f'unrecognized {name}')

    def read(address, size):
        at = address-MAIN_BASE
        if at < 0 or size < 0 or at+size > len(main):
            raise ValueError('source outside main data')
        return main[at:at+size]

    def word(address):
        return words(main, MAIN_BASE, address, 1)[0]

    # Shared5454, followed by the optional phase29/5460 overlay in source order.
    chars, present = bytearray(0x80000), bytearray(0x80000)
    palette, palette_present = bytearray(0x4000), bytearray(0x4000)
    for root_address in ((0x2800000, 0x2800014) if name_scene else (0x2800000,)):
        root = word(root_address)
        address = word(root)
        for _ in range(128):
            source = word(address);address += 4
            if not source:
                break
            destination = word(address);address += 4
            size = word(source)*32
            offset = destination-0x1080000
            if offset < 0 or offset+size > len(chars):
                raise ValueError('character destination out of bounds')
            chars[offset:offset+size] = read(source+4, size)
            present[offset:offset+size] = bytes([1])*size
        else:
            raise ValueError('unterminated character list')

        address = word(root+4)
        for _ in range(32):
            destination = word(address);address += 4
            if not destination:
                break
            size = word(address)*2;address += 4
            offset = destination-0x1800000
            if size%4 or offset < 0 or offset+size > len(palette):
                raise ValueError('palette destination/length invalid')
            palette[offset:offset+size] = read(address, size)
            palette_present[offset:offset+size] = bytes([1])*size
            address += size
        else:
            raise ValueError('unterminated palette list')

    assets = {}

    def asset(name, width, height, tiles):
        missing = sorted({tile for tile in tiles if not all(present[(tile&0x3fff)*32:((tile&0x3fff)+1)*32])})
        if missing:
            assets[name] = dict(width=width*8, height=height*8, tiles=tiles, missing_tiles=missing)
            return name  # Inventory unresolved art; never invent zero-filled pixels.
        pens = []
        for y in range(height*8):
            for x in range(width*8):
                tile = tiles[(y//8)*width+x//8]
                offset = ((tile&0x3fff)*32+(y%8)*4+(x%8)//2)^1
                if not present[offset]:
                    raise ValueError(f'{name}: tile {tile:04x} at {x},{y} not supplied by loaders')
                packed = chars[offset]
                pen = (packed&15) if x&1 else packed>>4
                color = ((tile>>7)&255)*16+pen
                if pen and not all(palette_present[2*color:2*color+2]):
                    raise ValueError(f'{name}: palette not supplied by loaders')
                pens.append(color if pen else 0)  # Index0 is transparent.
        colors = {struct.unpack_from('<H', palette, 2*p)[0] if p else -1 for p in pens}
        assets[name] = dict(width=width*8, height=height*8, tiles=tiles,
                            pens=pens, used_pens=len(set(pens)), raw_colors=len(colors))
        return name

    def block(address):
        name = f'block-{address:08x}'
        if name in assets:
            return name
        base, wide, height, width = struct.unpack('<hhII', read(address, 12))
        if wide not in (0, 1) or not 1 <= width <= 64 or not 1 <= height <= 64:
            raise ValueError('invalid authored label header')
        size = width*height
        values = struct.unpack('<'+('h' if wide else 'b')*size, read(address+12, size*(2 if wide else 1)))
        return asset(name, width, height, [(base+v)&65535 for v in values])

    if name_scene:
        # Original227a4c: rank/time and animated3D are separate from these blocks.
        fixed = [block(a) for a in (0x2883100, 0x288484c, 0x2884a0a, 0x2884a4a)]
        countdown=[]
        for glyph in (0x20,*range(0x30,0x3a)):
            first=((glyph-32)*6+0x8000+5*128)&65535
            countdown.append(asset(f'glyph-{glyph:02x}',2,3,[(first+i)&65535 for i in range(6)]))
        time_glyphs=[asset(f'time-{i:02d}',1,2,[0x8100+2*i,0x8101+2*i]) for i in range(12)]
        rank_glyphs=[asset(f'rank-{i:02d}',2,4,[0x8580+8*i+j for j in range(8)])
                     for i in (*range(10),11,12,13,14)]
        return assets, bytes(chars), bytes(palette), dict(fixed=fixed,
            countdown=countdown,time_glyphs=time_glyphs,rank_glyphs=rank_glyphs,
            tilemap_destinations=[0x1004000, 0x100011e, 0x100150a, 0x1001542],
            caveat='Shared5454 plus name5460; static blocks, countdown, small-time and highscore-rank fonts.')
    fixed = [block(a) for a in (0x2878912, 0x287887e, 0x2878596, 0x2872cae)]
    ranks = []
    # 42 records: following words at2344ec are audio commands, not rank42.
    for rank in range(42):
        row = words(program, 0x200000, 0x23424c+rank*16, 4)
        if row[3] or not row[1] or not row[2]:
            raise ValueError('invalid result rank directory')
        ranks.append([block(a) if a else None for a in row[:3]])
    # Shared font defaults established by5454: bank5, base8000.
    for glyph in (0x20, 0x22, 0x27, *range(0x30, 0x3a)):
        first = ((0x8000+glyph-32)*6+0x8000+5*128)&65535
        asset(f'glyph-{glyph:02x}', 2, 3, [(first+i)&65535 for i in range(6)])
    metadata = dict(fixed=fixed, ranks=ranks, font_bank=5, font_base=0x8000,
                    normal_offsets=[0x29e,0x119a,0x11c6,0x724,0x738,0xacc,0x1416,0x1442],
                    penalty_offsets=[0x11c,0x6c8,0x696,0xb84,0x948,0x912],
                    caveat='Shared5454 source context; final scene colour translation/gamma and PVR packing pending.')
    return assets, bytes(chars), bytes(palette), metadata


def pack(assets, palette, color_text):
    rows = [line.split() for line in color_text.splitlines() if line and not line.startswith('#')]
    if len(rows) != 4096 or any(len(row) != 3 for row in rows):
        raise ValueError('expected4096 source palette rows')
    colors = []
    for i, row in enumerate(rows):
        index, raw, rgba = map(int, row)
        if index != i or raw != struct.unpack_from('<H', palette, 2*i)[0] or not 0xff000000 <= rgba <= 0xffffffff:
            raise ValueError('source colour index/raw palette/alpha mismatch')
        colors.append(rgba)
    banks, selected, pixels = [], {}, {}
    for name, record in assets.items():
        if 'pens' not in record:
            continue
        pixels[name] = [colors[p] if p else 0 for p in record['pens']]
        used = set(pixels[name])|{0}
        fits = [(len(used-bank), i) for i, bank in enumerate(banks) if len(used|bank) <= 16]
        if fits:
            bank = min(fits)[1];banks[bank] |= used
        else:
            if len(used) > 16 or len(banks) >= 4:
                raise ValueError('lossless labels exceed four reserved PAL4 banks')
            bank = len(banks);banks.append(used)
        selected[name] = bank
    banks = [sorted(bank)+[0]*(16-len(bank)) for bank in banks]
    palette_blob = b'DPAL'+struct.pack('<I', 16*len(banks))+b''.join(struct.pack('<16I', *b) for b in banks)
    textures, bindings, previews = [], {}, {}
    checked = 0
    for name, source in pixels.items():
        record = assets[name];width, height = record['width'], record['height']
        bank = selected[name];lookup = {color: banks[bank].index(color) for color in set(source)|{0}}
        previews[name] = tga(width, height, [struct.pack('<I', p) for p in source])
        bindings[name] = []
        ink = [(i%width,i//width) for i,p in enumerate(source) if p]
        if not ink:
            continue
        left,right = min(x for x,y in ink),max(x for x,y in ink)+1
        top,bottom = min(y for x,y in ink),max(y for x,y in ink)+1
        for y in range(top, bottom, 128):
            for x in range(left, right, 128):
                w, h = min(128,right-x), min(128,bottom-y)
                tw,th = (max(8,1 << (n-1).bit_length()) for n in (w,h))
                expected = [source[(y+dy)*width+x+dx] if dx<w and dy<h else 0
                            for dy in range(th) for dx in range(tw)]
                payload = bytearray(tw*th//2)
                for dy in range(th):
                    for dx in range(tw):
                        at = block_index(dx,dy,tw,th)
                        payload[at//2] |= lookup[expected[dy*tw+dx]] << ((at&1)*4)
                blob = bytearray(32)+payload
                struct.pack_into('<4sI',blob,0,b'DcTx',len(blob));blob[11]=15
                # DcTx stores literal dimensions at 12, but PVR type stores
                # log2(size)-3 in its low six bits. 0x12 is only 32x32.
                struct.pack_into('<HHI',blob,12,tw,th,
                                 0x28000000|((tw.bit_length()-4)<<3)|(th.bit_length()-4))
                pal = b'DPAL'+struct.pack('<17I',16,*banks[bank])
                check_pal4(blob,pal,[struct.pack('<I',p) for p in expected],tw,th)
                bindings[name].append(dict(texture=len(textures),bank=60+bank,x=x,y=y,width=w,height=h,texture_width=tw,texture_height=th))
                textures.append(bytes(blob));checked += tw*th
    return textures,palette_blob,bindings,previews,checked


def render_header(assets, metadata, bindings):
    ids = {name:i for i,name in enumerate(assets)}
    parts, spans = [], []
    for name in assets:
        if name not in bindings:
            spans.append((65535,0))  # Missing art, distinct from blank glyph.
            continue
        spans.append((len(parts),len(bindings[name])))
        for p in bindings[name]:
            parts.append(tuple(p[k] for k in ('texture','bank','x','y','width','height','texture_width','texture_height')))
    lines = ['/* Offline ROM-derived UI bindings; no runtime JSON/ROM parsing. */',
             '#include <stdint.h>', '#include <stdalign.h>',
             'typedef struct { uint16_t texture,bank,x,y,width,height,texture_width,texture_height; } nm_result_label_part_t;',
             '_Static_assert(sizeof(nm_result_label_part_t)==16,"two UI parts per32-byte cache line");',
             f'enum {{ NM_RESULT_LABEL_TEXTURE_COUNT={len(parts)}, NM_RESULT_LABEL_ASSET_COUNT={len(assets)}, '
             f'NM_RESULT_LABEL_PALETTE_FIRST=960, NM_RESULT_LABEL_PALETTE_COLORS={16*(max(p[1] for p in parts)-59)} }};',
             'alignas(32) static const nm_result_label_part_t nm_result_label_parts[] = {']
    lines += ['    {'+','.join(map(str,p))+'},' for p in parts]
    lines += ['};','static const uint16_t nm_result_label_spans[][2] = {']
    lines += ['    {%d,%d},' % p for p in spans]
    lines += ['};','static const uint16_t nm_result_label_fixed[4] = {'+
              ','.join(str(ids[n]) for n in metadata['fixed'])+'};',
              'static const uint16_t nm_result_label_ranks[42][3] = {']
    lines += ['    {'+','.join(str(ids[n]) if n else '65535' for n in row)+'},' for row in metadata['ranks']]
    glyphs=[ids.get(f'glyph-{i:02x}',65535) for i in range(58)]
    lines += ['};','static const uint16_t nm_result_label_glyphs[58] = {'+','.join(map(str,glyphs))+'};',
              'static const uint16_t nm_result_label_normal_offsets[8] = {'+
              ','.join(map(str,metadata['normal_offsets']))+'};',
              'static const uint16_t nm_result_label_penalty_offsets[6] = {'+
              ','.join(map(str,metadata['penalty_offsets']))+'};','']
    return '\n'.join(lines)


def name_render_header(assets, metadata, bindings):
    """Bake authored screen placement and pre/post-3D priority, no tilemap VM."""
    parts=[]
    for name,destination in zip(metadata['fixed'],metadata['tilemap_destinations'],strict=True):
        priorities={int(bool(t&0x8000)) for t in assets[name]['tiles']}
        if len(priorities)!=1:
            raise ValueError('mixed name tile priority requires separate packed planes')
        foreground=priorities.pop()
        offset=(destination-0x1000000)&0x3fff
        x,y=((offset>>1)&63)*8,(offset>>7)*8
        for p in bindings[name]:
            if p['texture']!=len(parts):
                raise ValueError('name texture order differs from authored blocks')
            parts.append((p['bank'],x+p['x'],y+p['y'],p['width'],p['height'],
                          p['texture_width'],p['texture_height'],foreground))
    static_count=len(parts)
    for name in metadata['countdown'][1:]+metadata['time_glyphs']+metadata['rank_glyphs']:
        if len(bindings[name])!=1:
            raise ValueError('name glyph must fit one texture')
        p=bindings[name][0]
        if p['texture']!=len(parts):
            raise ValueError('name glyph order changed')
        parts.append((p['bank'],p['x'],p['y'],p['width'],p['height'],p['texture_width'],p['texture_height'],1))
    lines=['/* Generated from owned daytona93; no runtime tilemap decoding. */',
           '#ifndef DAYTONA_NAME_LABEL_DATA_H','#define DAYTONA_NAME_LABEL_DATA_H',
           '#include "render/ui/name_labels.h"',
           f'enum {{ NM_NAME_LABEL_TEXTURE_COUNT={len(parts)}, NM_NAME_LABEL_STATIC_COUNT={static_count}, NM_NAME_LABEL_PALETTE_COLORS={16*(max(p[0] for p in parts)-59)} }};',
           '_Alignas(32) static const nm_name_label_part_t nm_name_label_parts[]={']
    return '\n'.join(lines+['{'+','.join(map(str,p))+'},' for p in parts]+['};','#endif',''])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--colors', type=Path, help='source-oracle palette export; emits PAL4 candidates')
    parser.add_argument('--name-scene', action='store_true', help='extract name-screen blocks and countdown font')
    args = parser.parse_args()
    color_text = args.colors.read_text() if args.colors else None
    if args.name_scene and color_text is not None and not color_text.startswith('# name5454+5460 palette,'):
        parser.error('--name-scene requires --export-name-label-colors output')
    assets, chars, palette, metadata = extract((args.source/'program.bin').read_bytes(),
                                               (args.source/'main_data.bin').read_bytes(), args.name_scene)
    packed = pack(assets,palette,color_text) if color_text is not None else None
    args.output.mkdir()  # Never overwrite an earlier asset pack.
    if packed:
        textures,pal,bindings,previews,checked = packed
        if args.name_scene:
            (args.output/'name_label_data.h').write_text(name_render_header(assets,metadata,bindings))
        else:
            (args.output/'result_label_data.h').write_text(render_header(assets,metadata,bindings))
        for i, blob in enumerate(textures):
            (args.output/f'label-{i:03d}.dt').write_bytes(blob)
        (args.output/'labels.dp').write_bytes(pal)
        for name, blob in previews.items():
            (args.output/(name+'.tga')).write_bytes(blob)
        metadata['pal4'] = dict(bindings=bindings,palette_first=960,palette_colors=(len(pal)-8)//4,
            payload_bytes=sum(len(b)-32 for b in textures),file_bytes=sum(map(len,textures)),
            checked_texels=checked,color_source_sha256=hashlib.sha256(args.colors.read_bytes()).hexdigest(),
            caveat='Exact at exported loader/current RGB context. Full scene integration remains pending.')
    for name, record in assets.items():
        pixels = record.pop('pens', None)
        if pixels is not None:
            (args.output/(name+'.pens')).write_bytes(struct.pack(f'<{len(pixels)}H', *pixels))
    (args.output/'shared-characters.bin').write_bytes(chars)
    (args.output/'shared-palette.bin').write_bytes(palette)
    metadata['assets'] = assets
    (args.output/'manifest.json').write_text(json.dumps(metadata, indent=2)+'\n')
    missing = sum('missing_tiles' in a for a in assets.values())
    print(f'Extracted {len(assets)-missing} pen planes,{len(metadata.get("ranks", []))} rank layouts; unresolved assets={missing}; '
          f'max pen indices={max(a.get("used_pens", 0) for a in assets.values())}; '
          f'max raw colours={max(a.get("raw_colors", 0) for a in assets.values())}')
