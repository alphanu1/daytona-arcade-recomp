"""The podium's 2D screen ("Congratulations!") from verified daytona93, offline.

The podium object b948 (phase 27 entry 03b84, per tick ba18 in the phases 25, 26 and 28) draws, besides the
cinematic: the banner block 2878aec sliding up on tile layer A (baf4), the full-screen block 2884a8e on layer B
when the timer 5010a8 reaches 384 (bc80, event table bc00), the staff photo model 288cb94 from its tick 1213 on
(befc: T(0, 0, 2) Rx(0x4000) S(0.29 + 0.05 min(n, 26)), n from 40 down, context 5020c0: focus 280), the SEGA
2872a2a and AM2 287659c blocks at its tick 1408 (table 233bac, be48), "PRESENTED BY" (string bae0, font 18394)
at 1409 (ba7c), all of layer A erased at 1570, and a row-scroll wipe of layer A while the timer runs 42..22 (bcf4).
The characters and palettes are those of Shared5454 plus the cleanup's overlay 5460 (2800014, the same as the
name screen's: prepare_result_labels.extract --name-scene); colours from m2segment --export-name-label-colors.
The photo's texture comes from m2segment --export-native-objects of list 288cb94.

usage: prepare_podium_screen.py IMAGES NAME_COLOURS PHOTO_EXPORT NEW_ASSET_DIR NEW_HEADER

Writes NEW_ASSET_DIR/podium-NNN.dt (DcTx PAL4, one per part), cinematic.dp (banks 60..61: banner, photo; written
at the podium's spawn) and static.dp (banks from 2: written after the cleanup, when no car or scenery is drawn),
and NEW_HEADER (podium_screen_data.h: the parts, their banks, places and timing). Every texel is checked
against the source pixel it encodes. ROM-derived output: write only below ignored build/ directories.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from prepare_material_crops import block_index
from prepare_result_labels import extract
from prepare_scene import check_pal4

BANNER, SEGA, AM2, BACKGROUND = 0x2878aec, 0x2872a2a, 0x287659c, 0x2884a8e
TEXT_STRING, TEXT_PLACE = 0xbae0, 0x1000c2a          # ba7c: string bae0 at layer A offset c2a
CINEMATIC_BANK, STATIC_BANK = 60, 2


def colours(path):
    text = Path(path).read_text()
    if not text.startswith('# name5454+5460 palette,'):
        raise ValueError('expected m2segment --export-name-label-colors output')
    rows = [l.split() for l in text.splitlines() if l and not l.startswith('#')]
    if len(rows) != 4096:
        raise ValueError('expected 4096 colour rows')
    return [int(r[2]) for r in rows]


def place(offset):
    """Layer A/B tile address -> pixel (x, y): 64 columns of 2 bytes a row."""
    offset = (offset-0x1000000) & 0x3fff
    return ((offset >> 1) & 63)*8, (offset >> 7)*8


def tiles_to_pixels(chars, colours_of, width, height, tiles):
    pixels = []
    for y in range(height*8):
        for x in range(width*8):
            tile = tiles[(y//8)*width+x//8]
            packed = chars[((tile & 0x3fff)*32+(y % 8)*4+(x % 8)//2) ^ 1]
            pen = (packed & 15) if x & 1 else packed >> 4
            pixels.append(colours_of[((tile >> 7) & 255)*16+pen] if pen else 0)
    return pixels


def block(main, chars, colours_of, address):
    at = address-0x2000000
    base, wide, height, width = struct.unpack_from('<hhII', main, at)
    if wide not in (0, 1) or not 1 <= width <= 64 or not 1 <= height <= 64:
        raise ValueError('invalid block header %x' % address)
    values = struct.unpack_from('<'+('h' if wide else 'b')*(width*height), main, at+12)
    tiles = [(base+v) & 65535 for v in values]
    return width*8, height*8, tiles_to_pixels(chars, colours_of, width, height, tiles)


def text(program, chars, colours_of):
    string = program[TEXT_STRING:TEXT_STRING+64].split(b'\0')[0]
    if not string or any(c < 32 or c > 0x7e for c in string):
        raise ValueError('unexpected podium string')
    width = 16*len(string)
    pixels = [0]*(width*24)
    for i, c in enumerate(string):
        first = ((c-32)*6+0x8000+5*128) & 65535   # 18394: 2 x 3 tiles a glyph, font bank 5
        glyph = tiles_to_pixels(chars, colours_of, 2, 3, [(first+j) & 65535 for j in range(6)])
        for y in range(24):
            pixels[y*width+16*i:y*width+16*i+16] = glyph[y*16:y*16+16]
    return width, 24, pixels, string.decode()


def photo(export):
    """The exported model 288cb94: one quad, the whole 256 x 256 texture; its 32-bit TGA."""
    mesh = (export/'mesh_0.txt').read_text().split()
    if 'face' not in mesh:
        raise ValueError('photo export has no face')
    data = (export/'materials'/'texture_1.tga').read_bytes()
    w, h = struct.unpack_from('<HH', data, 12)
    if data[2] != 2 or data[16] != 32 or (w, h) != (256, 256):
        raise ValueError('photo texture: 256 x 256 32-bit TGA expected')
    top = bool(data[17] & 0x20)
    out = []
    for y in range(h):
        row = y if top else h-1-y
        for x in range(w):
            b, g, r, a = data[18+data[0]+4*(w*row+x):][:4]
            out.append(0xff000000 | r << 16 | g << 8 | b if a else 0)
    if 0 in out:
        raise ValueError('photo texture has transparent texels')
    return w, h, out


def banks_for(assets, first, count_max):
    """Lossless 16-colour banks: an asset joins the first bank its colours fit (pen 0 is transparent)."""
    banks, chosen = [], {}
    for name, (w, h, pixels) in assets.items():
        used = set(pixels) | {0}
        fits = [i for i, b in enumerate(banks) if len(b | used) <= 16]
        if fits:
            banks[fits[0]] |= used
            chosen[name] = fits[0]
        else:
            if len(used) > 16:
                raise ValueError('%s: %d colours do not fit a PAL4 bank' % (name, len(used)))
            chosen[name] = len(banks)
            banks.append(used)
    if len(banks) > count_max:
        raise ValueError('podium screen needs %d banks, %d allowed' % (len(banks), count_max))
    ordered = [[0]+sorted(b-{0}) for b in banks]
    ordered = [b+[0]*(16-len(b)) for b in ordered]
    return ordered, {name: first+i for name, i in chosen.items()}, chosen


def slices(name, w, h, pixels, opaque, size=128):
    """size x size pieces of the inked rectangle (all of it when opaque)."""
    ink = [(i % w, i//w) for i, p in enumerate(pixels) if p or opaque]
    left, right = min(x for x, _ in ink), max(x for x, _ in ink)+1
    top, bottom = min(y for _, y in ink), max(y for _, y in ink)+1
    for y in range(top, bottom, size):
        for x in range(left, right, size):
            pw, ph = min(size, right-x), min(size, bottom-y)
            tw, th = (max(8, 1 << (n-1).bit_length()) for n in (pw, ph))
            yield x, y, pw, ph, tw, th, [pixels[(y+dy)*w+x+dx] if dx < pw and dy < ph else 0
                                         for dy in range(th) for dx in range(tw)]


def encode(source, tw, th, bank):
    lookup = {c: bank.index(c) for c in set(source)}
    payload = bytearray(tw*th//2)
    for dy in range(th):
        for dx in range(tw):
            at = block_index(dx, dy, tw, th)
            payload[at//2] |= lookup[source[dy*tw+dx]] << ((at & 1)*4)
    blob = bytearray(32)+payload
    struct.pack_into('<4sI', blob, 0, b'DcTx', len(blob))
    blob[11] = 15
    struct.pack_into('<HHI', blob, 12, tw, th, 0x28000000 | ((tw.bit_length()-4) << 3) | (th.bit_length()-4))
    check_pal4(blob, b'DPAL'+struct.pack('<17I', 16, *bank), [struct.pack('<I', p) for p in source], tw, th)
    return bytes(blob)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('images', type=Path)
    parser.add_argument('colours', type=Path)
    parser.add_argument('photo', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('header', type=Path)
    args = parser.parse_args()
    program, main_data = (args.images/'program.bin').read_bytes(), (args.images/'main_data.bin').read_bytes()
    _, chars, _, _ = extract(program, main_data, True)
    cols = colours(args.colours)
    tw_, th_, words, string = text(program, chars, cols)
    cinematic = {'banner': block(main_data, chars, cols, BANNER), 'photo': photo(args.photo)}
    static = {'background': block(main_data, chars, cols, BACKGROUND), 'sega': block(main_data, chars, cols, SEGA),
              'am2': block(main_data, chars, cols, AM2), 'text': (tw_, th_, words)}
    if 0 in static['background'][2]:
        raise ValueError('background block has transparent pens')
    places = {'banner': (16, 8), 'photo': (0, 0), 'background': (0, 0), 'sega': (14*8, 32*8), 'am2': (34*8, 30*8),
              'text': place(TEXT_PLACE)}
    roles = {'banner': 0, 'photo': 1, 'background': 2, 'sega': 3, 'am2': 4, 'text': 5}
    cin_banks, cin_bank_of, _ = banks_for(cinematic, CINEMATIC_BANK, 2)
    st_banks, st_bank_of, _ = banks_for(static, STATIC_BANK, 8)
    args.output.mkdir()
    parts, checked, nbytes = [], 0, 0
    for group, banks, bank_of in ((cinematic, cin_banks, cin_bank_of), (static, st_banks, st_bank_of)):
        for name, (w, h, pixels) in group.items():
            opaque = name in ('photo', 'background')
            first_bank = min(bank_of.values())
            # The photo in one texture: one bilinear quad, no seam.
            for x, y, pw, ph, tw, th, source in slices(name, w, h, pixels, opaque, 256 if name == 'photo' else 128):
                blob = encode(source, tw, th, banks[bank_of[name]-first_bank])
                (args.output/('podium-%03d.dt' % len(parts))).write_bytes(blob)
                px, py = places[name]
                parts.append((roles[name], bank_of[name], px+x, py+y, pw, ph, tw, th))
                checked += tw*th
                nbytes += len(blob)
    for name, banks in (('cinematic.dp', cin_banks), ('static.dp', st_banks)):
        (args.output/name).write_bytes(b'DPAL'+struct.pack('<I', 16*len(banks))+b''.join(struct.pack('<16I', *b) for b in banks))
    lines = ['/* Local ROM-derived data: do not distribute. The podium screen (tools/prepare_podium_screen.py). */',
             '#ifndef DAYTONA_PODIUM_SCREEN_DATA_H', '#define DAYTONA_PODIUM_SCREEN_DATA_H',
             '#define NM_PODIUM_SCREEN_DATA 1',
             '#include "render/ui/podium_render.h"',
             'enum { NM_PODIUM_SCREEN_PARTS = %d, NM_PODIUM_CINEMATIC_BANK = %d, NM_PODIUM_CINEMATIC_BANKS = %d,'
             ' NM_PODIUM_STATIC_BANK = %d, NM_PODIUM_STATIC_BANKS = %d };' % (
                 len(parts), CINEMATIC_BANK, len(cin_banks), STATIC_BANK, len(st_banks)),
             '/* role (NM_PODIUM_PART_*), bank, x, y, width, height, texture width, height: podium-NNN.dt in order. */',
             'static const nm_podium_part_t nm_podium_screen_parts[NM_PODIUM_SCREEN_PARTS] = {']
    lines += ['    {%d,%d,%d,%d,%d,%d,%d,%d},' % p for p in parts]
    lines += ['};', '#endif', '']
    args.header.parent.mkdir(parents=True, exist_ok=True)
    with args.header.open('x') as f:
        f.write('\n'.join(lines))
    print(json.dumps(dict(parts=len(parts), text=string, cinematic_banks=len(cin_banks), static_banks=len(st_banks),
                          checked_texels=checked, bytes=nbytes,
                          colour_source_sha256=hashlib.sha256(args.colours.read_bytes()).hexdigest())))


if __name__ == '__main__':
    main()
