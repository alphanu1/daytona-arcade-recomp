#!/usr/bin/env python3
"""The original Saturn Daytona USA's (1995) front-end art for the PC build's Saturn-style menus.

    python3 prepare_saturn_menus.py SATURN_DIR ASSET_SET [--preview DIR]

SATURN_DIR  the files of your Saturn Daytona USA disc (APROG.BIN, SCROLL.BIN)
ASSET_SET   writes saturn-menu/*.dt there (DcTx textures, platform/pc-native/code/pc_saturn_menu.c)

The menus' screens (Saturn APROG's Mode Select 060193F4, Options 060198E0, Pause 060096DC) are 352 x 224 of 16-colour
8x8 cells: SCROLL.BIN (loaded at 002A0000) holds LZSS streams of chars and raw maps (width, height, 16-bit names);
the palettes are in APROG.BIN (loaded at 06003000), RGB555 with blue high. (The title before the menus is the
arcade's own.)

Written (the menus' pieces from Mode Select's and Options' own maps, fonts and palettes, Circuit Edition's main menu
words put together from the Mode Select words' letters; prepare_saturn_menus.py --preview DIR draws them):
  menu-bg, banner-mode, main-*-0/1 (item, cursor), value-*-0/1/2 (a value, the chosen one, on the cursor's row),
  font-label, font-cursor, font-value-0/1/2, font-pause (font C: 16 x 4 atlases of 8 x 16 glyphs, ASCII 0x20..0x5A)
Game-derived output: keep the asset set under build/.
"""
import argparse
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from prepare_cce_course import dctx, pow2  # noqa: E402

SCREEN_W, SCREEN_H = 352, 224


def lzss(data, start):
    """The Saturn game's LZSS (APROG 06028654): a flags byte, low bit first, 1 a literal, 0 two bytes b1 b2:
    offset ((b2 | 0xF0) << 8 | b1) + position in a 4096-byte ring of zeros, length (b2 >> 4) + 3; 0 0 ends."""
    out, ring, pos, i, flags = bytearray(), bytearray(4096), 0, start, 0
    while True:
        flags >>= 1
        if not flags & 0x100:
            flags = data[i] | 0xFF00
            i += 1
        if flags & 1:
            b = data[i]
            i += 1
            out.append(b)
            ring[pos] = b
            pos = (pos + 1) & 0xFFF
            continue
        b1, b2 = data[i], data[i + 1]
        i += 2
        if b1 == 0 and b2 == 0:
            return bytes(out)
        src = (((b2 | 0xF0) << 8) | b1) + pos
        for k in range((b2 >> 4) + 3):
            b = ring[(src + k) & 0xFFF]
            out.append(b)
            ring[pos] = b
            pos = (pos + 1) & 0xFFF


def texture(pixels, w, h, transparent):
    W, H = pow2(w), pow2(h)
    words = [pixels[y * w + x] if x < w and y < h else (None if transparent else 0) for y in range(H) for x in range(W)]
    if not transparent:
        words = [0 if v is None else v for v in words]
    return dctx(W, H, words, transparent)


# ---- the menu screens (Saturn APROG's Mode Select 060193F4, Options 060198E0, Pause 060096DC) ---------------
# What they draw from, read from the program's own setup (layers NBG0-3: 16 colours, 1-word names, 12-bit char
# numbers; NBG1 adds char 0x1000 and palette 32, NBG3 palette 48, NBG2 nothing). VRAM loads: (SCROLL.BIN offset,
# VRAM byte address), LZSS streams.
VRAM_LOADS = [(0x48514, 0x34000),       # the menus' blue background (Mode Select, Options)
              (0x4859C, 0x17700),       # the big words, the banners, the options' values
              (0x311A4, 0x3EE0)]        # font C, 8 x 16 (char 0x1F7 on)
# Colour RAM loads (palette, count, APROG.BIN offset): the front end's (Mode Select/Options) and the race's (Pause).
CRAM_MENU = [(64, 18, 0x451AC), (42, 1, 0x59E9C), (0, 1, 0x59D9C), (48, 1, 0x59F9C), (49, 7, 0x59EBC), (60, 4, 0x46DC4)]
CRAM_RACE = [(64, 18, 0x451AC), (0, 3, 0x4514C), (3, 2, 0x4548C), (5, 1, 0x454CC), (7, 1, 0x455AC), (8, 1, 0x4592C),
             (9, 1, 0x458EC), (10, 1, 0x4590C), (11, 1, 0x59DBC), (13, 1, 0x457EC), (14, 2, 0x453EC), (48, 2, 0x4548C),
             (51, 3, 0x4588C), (61, 1, 0x457EC), (62, 2, 0x453EC)]
LAYER_ADD = {1: (0x1000, 32), 2: (0, 0), 3: (0, 48)}
FONT_C_GLYPHS, FONT_C_ADD = 0x06059CC0 - 0x06003000, 0x1F7     # per ASCII from 0x20: top and bottom cell
# Raw maps in SCROLL.BIN (width, height, cells), drawn with put_map 06028400: cell + char base | palette << 12.
MENU_BG = (0x4D748, 1, 0xA00, 10)                       # 44 x 28, NBG1
BANNER_MODE = (0x4E0EC, 3, 0xBB8, 1)                    # "Mode Select", 24 x 4
WORDS = {'arcade-mode': 0x4E1B0, 'saturn-mode': 0x4E2B4, 'ranking': 0x4E3B8, 'options': 0x4E454}   # 4 rows, NBG3
WORD_PALETTES = (2, 4)                                  # the item, the cursor's (cream, yellow)
VALUES = {'normal': 0x4E4F0, 'grandprix': 0x4E510, 'endurance': 0x4E53C, 'very-easy': 0x4E56C, 'easy': 0x4E594,
          'normal-b': 0x4E5A8, 'hard': 0x4E5C4, 'very-hard': 0x4E5D8, 'stereo': 0x4E600, 'monaural': 0x4E620}
VALUE_PALETTES = (5, 6, 7)                              # a value, the chosen one, the chosen one on the cursor's row


class Vdp2:
    """The pieces of VDP2 the menus use: VRAM from the loads, colour RAM from the program's palettes."""

    def __init__(self, scroll, aprog, cram_loads):
        self.scroll, self.aprog = scroll, aprog
        self.vram = bytearray(0x80000)
        for src, at in VRAM_LOADS:
            data = lzss(scroll, src)
            self.vram[at:at + len(data)] = data
        self.cram = [0] * 1024
        for pal, count, at in cram_loads:
            self.cram[pal * 16:(pal + count) * 16] = struct.unpack_from(f'>{16 * count}H', aprog, at)

    def cells(self, cells, w, h, layer, opaque=False):
        """RGB555 words (None: transparent) of w x h cells (16-bit names, the layer's adds applied)."""
        char_add, pal_add = LAYER_ADD[layer]
        px = [None] * (w * 8 * h * 8)
        for r in range(h):
            for c in range(w):
                v = cells[r * w + c]
                ch, pal = ((v & 0xFFF) + char_add) & 0x3FFF, ((v >> 12) + pal_add) & 63
                b = self.vram[ch * 32:ch * 32 + 32]
                for y in range(8):
                    for x in range(8):
                        k = (b[y * 4 + x // 2] >> (4 if x % 2 == 0 else 0)) & 15
                        if k or opaque:
                            px[(r * 8 + y) * w * 8 + c * 8 + x] = self.cram[pal * 16 + k]
        return px

    def map(self, at, layer, char_base, palette, opaque=False):
        w, h = struct.unpack_from('>HH', self.scroll, at)
        raw = struct.unpack_from(f'>{w * h}H', self.scroll, at + 4)
        return self.cells([(v + char_base) & 0xFFF | palette << 12 for v in raw], w, h, layer, opaque), w * 8, h * 8

    def font_c(self, palette, layer):
        """Font C's glyphs, ASCII 0x20..0x5A, as a 16 x 4 atlas of 8 x 16 glyphs ('@' is the cursor)."""
        cells = [0] * (16 * 8)
        names = []
        for i in range(0x5B - 0x20):
            top, bottom = struct.unpack_from('>HH', self.aprog, FONT_C_GLYPHS + 4 * i)
            names.append((top + FONT_C_ADD, bottom + FONT_C_ADD))
        grid = [0x1F7] * (16 * 8)
        for i, (top, bottom) in enumerate(names):
            r, c = 2 * (i // 16), i % 16
            grid[r * 16 + c], grid[(r + 1) * 16 + c] = top | palette << 12, bottom | palette << 12
        return self.cells(grid, 16, 8, layer), 128, 64


# Circuit Edition's main menu items in the big words' font: its letters are 3 cells wide (I one), a space 2, so a
# word is the cell columns of the Mode Select words' letters put side by side. Letter: (word, first column, width).
LETTERS = {'A': ('arcade-mode', 0, 3), 'R': ('arcade-mode', 3, 3), 'C': ('arcade-mode', 6, 3),
           'D': ('arcade-mode', 12, 3), 'E': ('arcade-mode', 15, 3), 'M': ('arcade-mode', 20, 3),
           'O': ('options', 0, 3), 'P': ('options', 3, 3), 'T': ('options', 6, 3), 'I': ('options', 9, 1),
           'N': ('options', 13, 3), 'S': ('options', 16, 3), 'K': ('ranking', 9, 3), 'G': ('ranking', 16, 3),
           'U': ('saturn-mode', 9, 3)}
MAIN_WORDS = {'arcade': 'ARCADE', 'time-attack': 'TIME ATTACK', 'records': 'RECORDS', 'options': 'OPTIONS'}


def word_cells(scroll, text):
    """The cells (16-bit, before char base and palette) of TEXT in the big words' font, and its width."""
    cols = []
    for ch in text:
        if ch == ' ':
            cols += [None, None]
            continue
        word, first, width = LETTERS[ch]
        at = WORDS[word]
        w, h = struct.unpack_from('>HH', scroll, at)
        raw = struct.unpack_from(f'>{w * h}H', scroll, at + 4)
        cols += [[raw[r * w + first + c] for r in range(h)] for c in range(width)]
    h = 4
    blank = None
    cells = []
    for r in range(h):
        for col in cols:
            cells.append(None if col is None else col[r])
    return cells, len(cols), h


def menus(scroll, aprog):
    menu = Vdp2(scroll, aprog, CRAM_MENU)
    race = Vdp2(scroll, aprog, CRAM_RACE)
    out = {}
    px, w, h = menu.map(MENU_BG[0], MENU_BG[1], MENU_BG[2], MENU_BG[3], opaque=True)
    out['menu-bg'] = (px, w, h, False)
    px, w, h = menu.map(*BANNER_MODE)
    out['banner-mode'] = (px, w, h, True)
    for name, at in WORDS.items():
        for k, pal in enumerate(WORD_PALETTES):
            px, w, h = menu.map(at, 3, 0xBB8, pal)
            out[f'word-{name}-{k}'] = (px, w, h, True)
    for name, text in MAIN_WORDS.items():
        cells, w, h = word_cells(scroll, text)
        blank = struct.unpack_from('>H', scroll, WORDS['arcade-mode'] + 4 + 2 * 18)[0]   # the space's own cell
        for k, pal in enumerate(WORD_PALETTES):
            names = [((blank if v is None else v) + 0xBB8) & 0xFFF | pal << 12 for v in cells]
            out[f'main-{name}-{k}'] = (menu.cells(names, w, h, 3), w * 8, h * 8, True)
    for name, at in VALUES.items():
        for k, pal in enumerate(VALUE_PALETTES):
            px, w, h = menu.map(at, 3, 0xBB8, pal)
            out[f'value-{name}-{k}'] = (px, w, h, True)
    for name, vdp, pal, layer in (('font-label', menu, 0, 2), ('font-cursor', menu, 0, 3), ('font-pause', race, 0, 2)):
        px, w, h = vdp.font_c(pal, layer)
        out[name] = (px, w, h, True)
    # Values the maps lack (SLOW, QUICK, ON, OFF, ...): font C in the value words' two colours of each state, its
    # outline and fill (white label font's black and white) taken from the NORMAL word's.
    px, w, h = menu.font_c(0, 2)
    for k in range(len(VALUE_PALETTES)):
        word = out[f'value-normal-{k}'][0]
        colours = sorted({v for v in word if v is not None}, key=lambda v: sum((v >> b) & 31 for b in (0, 5, 10)))
        outline, fill = colours[0], colours[-1]
        out[f'font-value-{k}'] = ([None if v is None else (outline if v == 0 else fill) for v in px], w, h, True)
    return out


def preview_sheet(images, path):
    from PIL import Image
    def rgb(v):
        return ((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31)
    names = sorted(images)
    W = 360
    y, places = 0, []
    x, row_h = 0, 0
    for n in names:
        _, w, h, _ = images[n]
        if x + w > W * 2:
            x, y, row_h = 0, y + row_h + 4, 0
        places.append((n, x, y)); x += w + 4; row_h = max(row_h, h)
    sheet = Image.new('RGB', (W * 2, y + row_h + 4), (255, 0, 255))
    for n, x0, y0 in places:
        px, w, h, _ = images[n]
        for yy in range(h):
            for xx in range(w):
                v = px[yy * w + xx]
                if v is not None:
                    sheet.putpixel((x0 + xx, y0 + yy), rgb(v))
    sheet.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('saturn', type=Path)
    ap.add_argument('assets', type=Path)
    ap.add_argument('--preview', type=Path)
    a = ap.parse_args()
    scroll = (a.saturn / 'SCROLL.BIN').read_bytes()
    aprog = (a.saturn / 'APROG.BIN').read_bytes()
    images = menus(scroll, aprog)
    if a.preview:
        a.preview.mkdir(parents=True, exist_ok=True)
        preview_sheet(images, a.preview / 'menus.png')
    out = a.assets / 'saturn-menu'
    out.mkdir(exist_ok=True)
    for old in ('title-bg', 'title-logo', 'title-press'):     # the Saturn title of earlier versions: the arcade's now
        (out / f'{old}.dt').unlink(missing_ok=True)
    for name, (px, w, h, transparent) in images.items():
        path = out / f'{name}.dt'
        path.unlink(missing_ok=True)
        path.write_bytes(texture(px, w, h, transparent))
    print(f'Saturn menus: {len(images)} textures in {out}')


if __name__ == '__main__':
    main()
