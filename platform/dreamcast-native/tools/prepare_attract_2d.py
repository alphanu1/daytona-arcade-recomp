"""The attract's 2D layer (tile layer A over the 3D picture) from verified daytona93, offline.

What the attract draws on layer A (1000000, 64 tile words a row, 8x8 tiles) from its phase handlers and objects
(include/attract_2d.h has the native ports): the footer 5674 (the SEGA block 2872a2a, the copyright block 2876a84,
"CREDIT", the credit count), the blinking line of 5588, the AM2 block 287659c of phase 2, the ranking 2266f8 of
phases 4/5 (its title block 2875eae, the course's name block 2268ac[501460], five records with the fonts 18394/18478
and the large digits 18ad0, the blocks 2876130 and 2876112 by the record's flags) and the V.R. demo's 224e64 (the
text blocks 2367e0/2367f0[53e59a & 1], the view boxes 2882406 / 224f78[501710]). The characters come from the shared
loader lists of 5454 (prepare_result_labels.py's reading), the colours from the board's final pens of an attract
frame (m2segment --dump-frames: pens_N.bin, colour translation and gamma applied; constant through the attract).

usage: prepare_attract_2d.py IMAGES PENS NEW_ASSET_DIR NEW_HEADER [--check-tiles TILES_N.BIN ...]

Writes NEW_ASSET_DIR/attract-2d.dt (one DcTx ARGB1555 twiddled atlas: every tile word the layer can hold that has
ink, 32 tiles a row; pen 0 transparent) and NEW_HEADER (attract_2d_data.h: the atlas keys, the blocks, the program's
tables and strings the ports read). The atlas is direct colour: the 3D scenes use every palette bank of the PVR
(render/materials/palette_banks.h); each colour keeps its top five bits a channel (checked: the error is printed).
--check-tiles: layer A of these tile RAM dumps must hold only words of the atlas or blank ones.
ROM-derived output: write only below ignored build/ directories.
"""
import argparse
import hashlib
from pathlib import Path
import struct

from inspect_course import IMAGES, MAIN_BASE, words
from prepare_material_crops import block_index

PROGRAM_BASE = 0x200000
FONT_BASE = 0x8000 | 5 << 7        # 501338/50133c as 5454 leaves them: the 2x3 font, palette 5
DIGITS_BASE = 0x8500               # 18ad0: 3x4 tiles a digit
FOOTER_BLOCKS = (0x2872a2a, 0x2876a84)
AM2_BLOCK = 0x287659c
RANKING_BLOCK, TA_BLOCK, R_BLOCK = 0x2875eae, 0x2876130, 0x2876112
COURSE_BLOCKS, COURSE_PLACES, SUFFIXES = 0x2268ac, 0x2268bc, 0x2268a0
DEMO_TEXT, DEMO_BOXES, DEMO_BOX_PLACES, DEMO_BOXES_DIM = (0x2367e0, 0x2367f0), 0x224f78, 0x224f70, 0x2882406
STRINGS = {'credit': 0x56cc, 'credits': 0x56f0, 'free_play': 0x5714, 'insert_coin': 0x5624, 'insert_more': 0x5638,
           'push_start': 0x564c, 'blank_line': 0x5660}


def check_image(name, blob):
    size, digest = IMAGES[name]
    if len(blob) != size or hashlib.sha256(blob).hexdigest() != digest:
        raise ValueError(f'unrecognized {name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('images', type=Path)
    parser.add_argument('pens', type=Path)
    parser.add_argument('asset_dir', type=Path)
    parser.add_argument('header', type=Path)
    parser.add_argument('--check-tiles', type=Path, nargs='*', default=[])
    args = parser.parse_args()
    program = (args.images/'program.bin').read_bytes()
    main_data = (args.images/'main_data.bin').read_bytes()
    check_image('program.bin', program)
    check_image('main_data.bin', main_data)
    pens = struct.unpack('<8192I', args.pens.read_bytes())

    def main_read(address, size):
        at = address-MAIN_BASE
        if at < 0 or size < 0 or at+size > len(main_data):
            raise ValueError('source outside main data')
        return main_data[at:at+size]

    def program_read(address, size):
        at = address-PROGRAM_BASE if address >= PROGRAM_BASE else address
        if at < 0 or at+size > len(program):
            raise ValueError('source outside the program')
        return program[at:at+size]

    def main_word(address):
        return words(main_data, MAIN_BASE, address, 1)[0]

    # The characters of the shared loader lists 5454 (2800000): as prepare_result_labels.extract.
    chars, present = bytearray(0x80000), bytearray(0x80000)
    root = main_word(0x2800000)
    address = main_word(root)
    for _ in range(128):
        source = main_word(address); address += 4
        if not source:
            break
        destination = main_word(address); address += 4
        size = main_word(source)*32
        offset = destination-0x1080000
        if offset < 0 or offset+size > len(chars):
            raise ValueError('character destination out of bounds')
        chars[offset:offset+size] = main_read(source+4, size)
        present[offset:offset+size] = bytes([1])*size
    else:
        raise ValueError('unterminated character list')

    blocks = {}

    def block(address):
        if address in blocks:
            return
        base, wide, height, width = struct.unpack('<hhII', main_read(address, 12))
        if wide not in (0, 1) or not 1 <= width <= 64 or not 1 <= height <= 64:
            raise ValueError(f'invalid block header {address:08x}')
        size = width*height
        values = struct.unpack('<'+('h' if wide else 'b')*size, main_read(address+12, size*(2 if wide else 1)))
        blocks[address] = (base, width, height, list(values))

    for a in (*FOOTER_BLOCKS, AM2_BLOCK, RANKING_BLOCK, TA_BLOCK, R_BLOCK, DEMO_BOXES_DIM):
        block(a)
    course_blocks = list(struct.unpack('<4I', program_read(COURSE_BLOCKS, 16)))
    course_places = list(struct.unpack('<8I', program_read(COURSE_PLACES, 32)))
    suffixes = list(program_read(SUFFIXES, 12))
    for a in course_blocks:
        block(a)
    demo_text = []
    for table in DEMO_TEXT:
        rows = []
        for country in range(2):
            x, y, b = struct.unpack('<HHI', program_read(table+8*country, 8))
            block(b); rows.append((x, y, b))
        demo_text.append(rows)
    demo_boxes = list(struct.unpack('<4I', program_read(DEMO_BOXES, 16)))
    for a in demo_boxes:
        block(a)
    demo_places = [struct.unpack('<HH', program_read(DEMO_BOX_PLACES+4*c, 4)) for c in range(2)]

    strings = {}
    for name, at in STRINGS.items():
        text = bytearray()
        while program_read(at+len(text), 1)[0]:
            text += program_read(at+len(text), 1)
            if len(text) > 40:
                raise ValueError(f'unterminated string {name}')
        strings[name] = bytes(text)

    # Every tile word the ports can write.
    candidates = {0x8020, 0x0020}
    for base, width, height, values in blocks.values():
        candidates |= {(base+v) & 0xffff for v in values}
    for glyph in range(0x20, 0x60):
        candidates |= {((glyph-32)*6+FONT_BASE+k) & 0xffff for k in range(6)}
    for digit in range(10):
        candidates |= {(DIGITS_BASE+12*digit+k) & 0xffff for k in range(12)}

    def pixels(word):
        out = []
        for y in range(8):
            for x in range(8):
                offset = ((word & 0x3fff)*32+y*4+x//2) ^ 1
                if not present[offset]:
                    raise ValueError(f'tile {word:04x} not supplied by the loader lists')
                packed = chars[offset]
                pen = (packed & 15) if x & 1 else packed >> 4
                out.append(pens[((word >> 7) & 255)*16+pen] if pen else 0)
        return out

    keys = {}
    for word in sorted(candidates):
        key = word & 0x7fff               # bit 15 selects neither the character (0..13) nor the palette (7..14)
        if key in keys:
            continue
        p = pixels(word)
        if any(c >> 24 == 0 and c for c in p):
            raise ValueError(f'tile {word:04x}: a pen with alpha 0')
        keys[key] = p if any(p) else None
    inked = [k for k in sorted(keys) if keys[k]]
    count = len(inked)
    rows = (count+31)//32
    height = 8
    while height < rows*8:
        height *= 2
    width = 256
    atlas = [0]*(width*height)
    for slot, key in enumerate(inked):
        tx, ty = (slot % 32)*8, (slot//32)*8
        for i, c in enumerate(keys[key]):
            atlas[(ty+i//8)*width+tx+i % 8] = c
    worst = 0
    texels = bytearray(width*height*2)
    for y in range(height):
        for x in range(width):
            c = atlas[y*width+x]
            a, r, g, b = c >> 24, (c >> 16) & 255, (c >> 8) & 255, c & 255
            value = (0x8000 if c else 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3
            if c:
                back = [(v << 3) | (v >> 2) for v in ((value >> 10) & 31, (value >> 5) & 31, value & 31)]
                worst = max(worst, *(abs(s-t) for s, t in zip((r, g, b), back)))
            struct.pack_into('<H', texels, 2*block_index(x, y, width, height), value)
    blob = bytearray(32)+texels
    struct.pack_into('<4sIBBBBHHI', blob, 0, b'DcTx', len(blob), 0, 0, 0, 0, width, height,
                     ((width.bit_length()-4) << 3) | (height.bit_length()-4))
    args.asset_dir.mkdir(parents=True, exist_ok=False)
    (args.asset_dir/'attract-2d.dt').write_bytes(bytes(blob))

    for path in args.check_tiles:
        layer = struct.unpack('<4096H', path.read_bytes()[:8192])
        missing = sorted({w for w in layer if (w & 0x7fff) not in keys})
        if missing:
            raise ValueError(f'{path.name}: layer A holds words the atlas lacks: ' +
                             ' '.join(f'{w:04x}' for w in missing[:24]))

    value_list, block_rows = [], []
    for address in sorted(blocks):
        base, w, h, values = blocks[address]
        block_rows.append(f'    {{0x{address:07x}u,{base},{w},{h},{len(value_list)}u}},')
        value_list += values

    def c_string(text):
        return '"' + ''.join(chr(c) if 32 <= c < 127 and c not in (34, 92) else f'\\{c:03o}' for c in text) + '"'

    lines = [
        '/* Generated by tools/prepare_attract_2d.py from verified daytona93: ROM-derived, keep under build/. */',
        '#ifndef DAYTONA_NATIVE_ATTRACT_2D_DATA_H', '#define DAYTONA_NATIVE_ATTRACT_2D_DATA_H', '#include <stdint.h>',
        f'#define NM_A2D_ATLAS_WIDTH {width}u', f'#define NM_A2D_ATLAS_HEIGHT {height}u',
        f'#define NM_A2D_TILES {count}u', f'#define NM_A2D_BLOCKS {len(block_rows)}u',
        f'#define NM_A2D_FONT_BASE 0x{FONT_BASE:04x}u', f'#define NM_A2D_DIGITS_BASE 0x{DIGITS_BASE:04x}u',
        f'#define NM_A2D_COLOUR_ERROR {worst}u /* the largest channel error of the ARGB1555 atlas */',
        '/* The tile words (bit 15 dropped) with ink, ascending; slot i: atlas tile (i % 32, i / 32). */',
        'static const uint16_t nm_a2d_keys[NM_A2D_TILES]={',
        *('    '+','.join(f'0x{k:04x}' for k in inked[i:i+16])+',' for i in range(0, count, 16)),
        '};',
        'typedef struct { uint32_t address; int16_t base; uint8_t width, height; uint32_t first; } nm_a2d_block_t;',
        'static const nm_a2d_block_t nm_a2d_blocks[NM_A2D_BLOCKS]={', *block_rows, '};',
        f'static const int16_t nm_a2d_values[{len(value_list)}]={{',
        *('    '+','.join(str(v) for v in value_list[i:i+24])+',' for i in range(0, len(value_list), 24)),
        '};',
        '/* 2268ac/2268bc: the course name blocks and their places (column, row) by 501460; 2268a0: the rank suffixes. */',
        'static const uint32_t nm_a2d_course_blocks[4]={'+','.join(f'0x{a:07x}u' for a in course_blocks)+'};',
        'static const uint8_t nm_a2d_course_places[4][2]={' +
        ','.join(f'{{{course_places[2*i]},{course_places[2*i+1]}}}' for i in range(4))+'};',
        'static const uint8_t nm_a2d_suffixes[12]={'+','.join(f'0x{c:02x}' for c in suffixes)+'};',
        '/* 2367e0, 2367f0: the demo text blocks (column, row, block) by 53e59a & 1; 224f78 the boxes by 501710,',
        ' * 224f70 their place by 53e59a & 1, 2882406 the boxes without a lit one. */',
        'typedef struct { uint8_t column, row; uint32_t block; } nm_a2d_place_t;',
        'static const nm_a2d_place_t nm_a2d_demo_text[2][2]={' +
        ','.join('{'+','.join(f'{{{x},{y},0x{b:07x}u}}' for x, y, b in rows)+'}' for rows in demo_text)+'};',
        'static const uint32_t nm_a2d_demo_boxes[4]={'+','.join(f'0x{a:07x}u' for a in demo_boxes)+'};',
        f'static const uint32_t nm_a2d_demo_boxes_dim=0x{DEMO_BOXES_DIM:07x}u;',
        'static const uint8_t nm_a2d_demo_box_places[2][2]={'+','.join(f'{{{x},{y}}}' for x, y in demo_places)+'};',
        '/* The footer and the blinking line (5674, 5588). */',
        *(f'static const char nm_a2d_text_{name}[]={c_string(text)};' for name, text in strings.items()),
        '#endif', '']
    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text('\n'.join(lines))
    print(f'attract 2D: {count} tiles with ink of {len(keys)} words ({width}x{height} ARGB1555, '
          f'{len(blob)} bytes), {len(blocks)} blocks, colour error {worst}; {len(args.check_tiles)} tile dumps checked')


if __name__ == '__main__':
    main()
