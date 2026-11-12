#!/usr/bin/env python3
"""The course select's art for the Circuit Edition courses of the PC build (slots 3, 4), added to an asset set's
select pack (platform/dreamcast-native/tools/prepare_select.py: select/select.dsl and its PAL4 textures).

    python3 prepare_select_cce.py SELECT_EXPORTS CCE_DAYTONA_DIR MAP_DIR ASSET_SET [--preview DIR]

SELECT_EXPORTS   the arcade's select captures the asset recipe made (OUTDIR/work/export: select-rte, select-adv,
                 select-exp; m2segment --export-native-select)
CCE_DAYTONA_DIR  the DAYTONA folder of your Circuit Edition disc (COURSEn.MDL/.TEX)
MAP_DIR          courseSLOT-map.json of each course (prepare_cce_course.py --map: the centre line)
ASSET_SET        its select/ gets the new textures and select.dsl the new quads (those of an earlier run replaced)

What it makes, in the arcade's own style:
  - each course's panel, lit and dim, from the arcade panel of the same name length (BEGINNER's for NATIONAL,
    EXPERT's for DESERT): its frame and bands as they are, the name in the panels' big font (14 x 28, 4-pixel
    strokes, corners cut 2 and 1 pixels: the arcade's letters where it has them, the others drawn in it), the
    second word and the laps in the bottom band's fonts ("PARK x 6 LAPS"), shadows as the arcade's (the letter
    shifted 1 and 2 pixels right and down), each pixel's role coloured as in the arcade panel's lit and dim
    versions, the course's colours a turn of the template's;
  - each course's map, lit and dim: the course seen from above (CCE's polygons, each in its texture's mean
    colour, lowest first), the road the arcade maps' way (black, white edges) along the centre line, in the
    arcade maps' bevelled border, quantized to 16 colours; dim is the arcade's dim map's per-channel scale of
    its lit one; the ground (beyond CCE's trackside polygons, of which the grey ones are drawn) clouds of the
    arcade maps' greens (National Park, as Beginner's) or sands (Desert City, as Advanced's);
  - the transmission screen's small map: the lit map, scaled as the arcade's.
Roles (code/render/ui/select_render.c): 64 + 2 (slot - 3) + variant panels, 68 + 2 (slot - 3) + lit maps,
72 + slot - 3 small maps. Output is game-derived: keep the asset set under build/.
"""
import argparse
from collections import Counter
import json
import math
import shutil
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
DN_TOOLS = HERE.parent.parent / 'dreamcast-native' / 'tools'
sys.path.insert(0, str(DN_TOOLS))
sys.path.insert(0, str(HERE))
from prepare_select import Pack  # noqa: E402
from prepare_cce_course import load_course, rgb555  # noqa: E402

# slot: CCE course, panel template (the arcade panel sprite of the same name length), name, second word, laps,
# colour turn (a function of the template's colours).
COURSES = {
    3: dict(cce=3, template='beginner', name='NATIONAL', word='PARK', laps=6, turn='purple', terrain='green'),
    4: dict(cce=4, template='expert', name='DESERT', word='CITY', laps=4, turn='gold', terrain='sand'),
}
TEMPLATES = {'beginner': (0, 1), 'advanced': (2, 3), 'expert': (4, 5)}   # sprite files: dim, lit

# --- fonts --------------------------------------------------------------------------------------------------
BIG_H, BIG_W = 28, 14


def big_glyph(letter):
    """A 28 x 14 letter of the panels' big font: blocks of 4-pixel strokes, round corners cut 2 then 1 pixel."""
    g = np.zeros((BIG_H, BIG_W), bool)
    W, H = BIG_W, BIG_H

    def box(x0, y0, x1, y1):
        g[y0:y1, x0:x1] = True

    def cut(corner):
        """Round one corner of the glyph's box: 'tl', 'tr', 'bl', 'br'."""
        rows = (0, 1) if corner[0] == 't' else (H - 1, H - 2)
        for row, n in zip(rows, (2, 1)):
            if corner[1] == 'l':
                g[row, :n] = False
            else:
                g[row, W - n:] = False
    if letter == 'A':
        box(0, 0, W, 4); box(0, 0, 4, H); box(W - 4, 0, W, H); box(0, 12, W, 16)
        cut('tl'); cut('tr')
    elif letter == 'D':
        box(0, 0, W, 4); box(0, H - 4, W, H); box(0, 0, 4, H); box(W - 4, 0, W, H)
        cut('tr'); cut('br')
    elif letter == 'O':
        box(0, 0, W, 4); box(0, H - 4, W, H); box(0, 0, 4, H); box(W - 4, 0, W, H)
        for c in ('tl', 'tr', 'bl', 'br'):
            cut(c)
    elif letter == 'L':
        box(0, 0, 4, H); box(0, H - 4, W, H)
    elif letter == 'S':
        box(0, 0, W, 4); box(0, 0, 4, 16); box(0, 12, W, 16); box(W - 4, 12, W, H); box(0, H - 4, W, H)
        cut('tl'); cut('br')
        g[12, :2] = False; g[13, :1] = False      # the middle bar's ends turn as the arcade's B and R do
        g[15, W - 2:] = False; g[14, W - 1:] = False
    elif letter == 'T':
        box(0, 0, W, 4); box(5, 0, 9, H)
    elif letter == 'I':
        g = np.zeros((H, 4), bool); g[:, :] = True
    else:
        raise ValueError(f'no big {letter!r}')
    return g


SMALL = {   # the bottom band's descriptor font (SHORT, MEDIUM, LONG): 14 rows, 2-pixel strokes
    'P': ['#####.', '######', '##..##', '##..##', '##..##', '##..##', '######', '#####.', '##....', '##....',
          '##....', '##....', '##....', '##....'],
    'A': ['.####.', '######', '##..##', '##..##', '##..##', '##..##', '######', '######', '##..##', '##..##',
          '##..##', '##..##', '##..##', '##..##'],
    'R': ['#####.', '######', '##..##', '##..##', '##..##', '##..##', '#####.', '#####.', '##..##', '##..##',
          '##..##', '##..##', '##..##', '##..##'],
    'K': ['##..##', '##..##', '##.###', '##.##.', '####..', '###...', '###...', '####..', '##.##.', '##.###',
          '##..##', '##..##', '##..##', '##..##'],
    'C': ['.####.', '######', '##..##', '##..##', '##....', '##....', '##....', '##....', '##....', '##....',
          '##..##', '##..##', '######', '.####.'],
    'I': ['##'] * 14,
    'T': ['######', '######'] + ['..##..'] * 12,
    'Y': ['##..##', '##..##', '##..##', '##..##', '##..##', '##..##', '######', '.####.', '..##..', '..##..',
          '..##..', '..##..', '..##..', '..##..'],
}


def small_glyph(letter):
    return np.array([[c == '#' for c in row] for row in SMALL[letter]], bool)


def text_mask(glyphs, gap):
    height = max(g.shape[0] for g in glyphs)
    width = sum(g.shape[1] for g in glyphs) + gap * (len(glyphs) - 1)
    m = np.zeros((height, width), bool)
    x = 0
    for g in glyphs:
        m[:g.shape[0], x:x + g.shape[1]] = g
        x += g.shape[1] + gap
    return m


# --- panels -------------------------------------------------------------------------------------------------
def load_rgba(path):
    return np.array(Image.open(path).convert('RGBA'))


def colour_key(a):
    return tuple(int(v) for v in a)


# The course's colours: each colour of the template's panels (dim and lit) to the course's. Beginner's blues to
# purples for National Park, Expert's reds to golds for Desert City; greys, black and white stay.
TURNS = {
    'purple': {(0, 0, 86): (60, 0, 90), (0, 0, 166): (118, 0, 170), (0, 0, 255): (168, 32, 255),
               (0, 62, 255): (186, 82, 255), (0, 190, 255): (216, 160, 255), (14, 255, 255): (242, 218, 255),
               (0, 126, 255): (176, 92, 255)},
    'gold': {(134, 0, 0): (112, 72, 0), (190, 0, 0): (164, 108, 0), (246, 62, 0): (226, 160, 0),
             (255, 62, 0): (222, 152, 0), (255, 158, 0): (255, 214, 64), (255, 214, 0): (255, 250, 176),
             (255, 110, 0): (246, 196, 16), (255, 174, 0): (255, 174, 0)},
}


def turn(rgb, how):
    if rgb[0] == rgb[1] == rgb[2]:
        return rgb
    table = TURNS[how]
    if rgb not in table:
        raise ValueError(f'{how}: no colour for the template\'s {rgb}')
    return table[rgb]


def panel(exports, course, preview=None):
    dim_i, lit_i = TEMPLATES[course['template']]
    course_dir = exports / 'select-rte' / 'course'
    dim, lit = load_rgba(course_dir / f'sprite_{dim_i}.tga'), load_rgba(course_dir / f'sprite_{lit_i}.tga')
    H, W = dim.shape[:2]
    white, orange = (255, 255, 255), (255, 174, 0)
    # Roles from the dim template: 1 big band text, 2 its shadow, 3 big band ground, 4 bottom band text,
    # 5 its digit, 6 bottom band ground; 0 everything else (kept as it is).
    role = np.zeros((H, W), np.uint8)
    rgb = dim[:, :, :3]
    is_white = np.all(rgb == white, axis=2)
    is_orange = np.all(rgb == orange, axis=2)
    big = np.zeros((H, W), bool); big[16:46, 12:132] = True
    low = np.zeros((H, W), bool); low[53:72, 9:135] = True
    in_big = Counter(colour_key(v) for v in rgb[big & ~is_white])
    ground_big = in_big.most_common(1)[0][0]
    shadow = [c for c, _ in in_big.most_common(2) if c != ground_big][0]
    in_low = Counter(colour_key(v) for v in rgb[low & ~is_white & ~is_orange & (dim[:, :, 3] > 0)])
    ground_low = in_low.most_common(1)[0][0]
    role[big & is_white] = 1
    role[big & np.all(rgb == shadow, axis=2)] = 2
    role[big & np.all(rgb == ground_big, axis=2)] = 3
    role[low & is_white] = 4
    role[low & is_orange] = 5
    role[low & np.all(rgb == ground_low, axis=2)] = 6
    # Each role's colour, dim and lit (the lit template's colour where the dim one has the role).
    colours = {}
    for r in range(1, 7):
        ys, xs = np.where(role == r)
        dc = Counter(colour_key(dim[y, x]) for y, x in zip(ys, xs)).most_common(1)[0][0]
        lc = Counter(colour_key(lit[y, x]) for y, x in zip(ys, xs)).most_common(1)[0][0]
        colours[r] = (dc, lc)
    # Clear the template's words, then the course's.
    new = role.copy()
    new[role == 1] = 3; new[role == 2] = 3
    new[role == 4] = 6; new[role == 5] = 6
    name = text_mask([big_glyph_from_arcade(exports, c) for c in course['name']], 2)
    ys, xs = np.where(role == 1)
    x0, y0 = xs.min(), ys.min()
    if name.shape[1] != xs.max() + 1 - x0:   # the same letter widths as the template's word: its place
        x0 = 12 + (120 - name.shape[1]) // 2
    shadow_mask = np.zeros((H, W), bool)
    name_mask = np.zeros((H, W), bool)
    name_mask[y0:y0 + name.shape[0], x0:x0 + name.shape[1]] = name
    for d in (1, 2):
        shadow_mask[d:, d:] |= name_mask[:-d, :-d]
    new[shadow_mask & ~name_mask & big] = 2
    new[name_mask] = 1
    # Bottom band: WORD x N LAPS; the multiplication sign and LAPS stay the template's, the word and digit new.
    times_laps = (role == 4) & (np.arange(W)[None, :] >= 60)
    new[times_laps] = 4
    word = text_mask([small_glyph(c) for c in course['word']], 1)
    wx = 9 + (52 - word.shape[1]) // 2 + 2
    new[55:55 + word.shape[0], wx:wx + word.shape[1]][word] = 4
    digit = digit_mask(exports, course['laps'])
    dy, dx = np.where(role == 5)
    new[dy.min():dy.min() + digit.shape[0], dx.min():dx.min() + digit.shape[1]][digit] = 5
    out = []
    for variant in (0, 1):
        img = (lit if variant else dim).copy()
        for r in range(1, 7):
            c = colours[r][variant]
            img[new == r] = c
        # The course's colours: every coloured pixel turned, but the dim digit's orange (the arcade's on all).
        keep = (new == 5) if not variant else np.zeros((H, W), bool)
        for y in range(H):
            for x in range(W):
                if img[y, x, 3] and not keep[y, x]:
                    img[y, x, :3] = turn(tuple(int(v) for v in img[y, x, :3]), course['turn'])
        out.append(img)
    if preview:
        Image.fromarray(np.concatenate(out, axis=1)).resize((W * 2 * 4, H * 4), Image.NEAREST).save(preview)
    return out


ARCADE_BIG = {   # letters of the arcade's panels: sprite, word, index
    'B': (0, 'BEGINNER', 0), 'E': (4, 'EXPERT', 0), 'G': (0, 'BEGINNER', 2), 'N': (0, 'BEGINNER', 4),
    'R': (4, 'EXPERT', 4), 'X': (4, 'EXPERT', 1), 'P': (4, 'EXPERT', 2), 'T': (4, 'EXPERT', 5), 'I': (0, 'BEGINNER', 3),
}


def big_glyph_from_arcade(exports, letter):
    if letter not in ARCADE_BIG:
        return big_glyph(letter)
    sprite, word, index = ARCADE_BIG[letter]
    a = load_rgba(exports / 'select-rte' / 'course' / f'sprite_{sprite}.tga')
    m = np.all(a[16:16 + BIG_H, :, :3] == (255, 255, 255), axis=2)
    m[:, :9] = False; m[:, 135:] = False
    cols = np.where(m.any(axis=0))[0]
    runs, start, prev = [], cols[0], cols[0]
    for c in cols[1:]:
        if c != prev + 1:
            runs.append((start, prev)); start = c
        prev = c
    runs.append((start, prev))
    if len(runs) != len(word):
        raise ValueError(f'{word}: {len(runs)} letters found')
    x0, x1 = runs[index]
    return m[:, x0:x1 + 1]


def digit_mask(exports, n):
    """The laps digit (16 x 15, the bottom band's): the arcade's 8, 4 and 2, and 6 (its 8 without the upper right
    stroke)."""
    source = {8: ('select-rte', 0), 4: ('select-rte', 2), 2: ('select-rte', 4), 6: ('select-rte', 0)}[n]
    a = load_rgba(exports / source[0] / 'course' / f'sprite_{source[1]}.tga')
    m = np.all(a[:, :, :3] == (255, 174, 0), axis=2)
    ys, xs = np.where(m)
    d = m[ys.min():ys.max() + 1, xs.min():xs.max() + 1].copy()
    if n == 6:
        h, w = d.shape
        d[3:h // 2, w - 3:] = False        # the 8's upper right stroke, between its top and middle bars
    return d


# --- maps ---------------------------------------------------------------------------------------------------
MAP = 112


def course_map(cce_dir, course, centre, exports):
    mdl = (cce_dir / f'COURSE{course["cce"]}.MDL').read_bytes()
    tex = (cce_dir / f'COURSE{course["cce"]}.TEX').read_bytes()
    blocks, texture = load_course(mdl, tex)
    centre = np.array(centre, float)
    low, high = centre.min(axis=0), centre.max(axis=0)
    span = float(max(high - low)) * 1.12
    middle = (low + high) / 2
    scale = (MAP - 1) / span

    def screen(x, z):
        return ((x - middle[0]) * scale + MAP / 2, -(z - middle[1]) * scale + MAP / 2)
    means = {}
    polys = []
    for polys_b in blocks:
        for attr, word, corners in polys_b:
            if (attr >> 5) & 15:
                if word not in means:
                    w, h, words, _ = texture(word)
                    px = [rgb555(v) for v in words if v is not None]
                    means[word] = tuple(int(sum(c[i] for c in px) / len(px)) for i in range(3)) if px else None
                colour = means[word]
            else:
                colour = rgb555(word)
            if colour is None:
                continue
            ys = [p[1] for p in corners]
            nx = (corners[1][1] - corners[0][1]) * (corners[2][2] - corners[0][2]) - \
                 (corners[1][2] - corners[0][2]) * (corners[2][1] - corners[0][1])
            ny = (corners[1][2] - corners[0][2]) * (corners[2][0] - corners[0][0]) - \
                 (corners[1][0] - corners[0][0]) * (corners[2][2] - corners[0][2])
            nz = (corners[1][0] - corners[0][0]) * (corners[2][1] - corners[0][1]) - \
                 (corners[1][1] - corners[0][1]) * (corners[2][0] - corners[0][0])
            ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
            if abs(ny) / ln < 0.35:
                continue                    # walls, fences, signs: seen edge-on from above
            polys.append((max(ys), [screen(p[0], p[2]) for p in corners], colour))
    polys.sort(key=lambda p: p[0])
    big = 4
    img = Image.fromarray(terrain(exports, course['terrain'], MAP * big))
    draw = ImageDraw.Draw(img)
    for _, pts, colour in polys:
        if paved(colour):
            draw.polygon([(x * big, y * big) for x, y in pts], fill=colour)
    img = img.resize((MAP, MAP), Image.LANCZOS)
    draw = ImageDraw.Draw(img)
    line = [screen(x, z) for x, z in centre]
    line.append(line[0])
    draw.line(line, fill=(255, 255, 255), width=7, joint='curve')
    draw.line(line, fill=(24, 24, 24), width=5, joint='curve')
    lit = np.array(img.convert('RGBA'))
    # The arcade maps' border (Advanced's map: its outer 2 pixels, grey on all four sides) and its dim scale.
    arcade_lit = load_rgba(exports / 'select-adv' / 'course_cursor1' / 'map_1.tga')
    arcade_dim = load_rgba(exports / 'select-rte' / 'course' / 'map_1.tga')
    border = np.zeros((MAP, MAP), bool)
    border[:2, :] = border[-2:, :] = border[:, :2] = border[:, -2:] = True
    lit[border] = arcade_lit[border]
    a, b = arcade_lit[~border][:, :3].astype(float), arcade_dim[~border][:, :3].astype(float)
    ratio = (a * b).sum(axis=0) / np.maximum((a * a).sum(axis=0), 1.0)
    dim = lit.copy()
    dim[:, :, :3] = np.clip(lit[:, :, :3] * ratio, 0, 255).astype(np.uint8)
    dim[border] = arcade_dim[border]
    return lit, dim, ratio


def paved(colour):
    """A polygon drawn on the map: grey (asphalt, concrete, roofs); the ground's greens and sands are the
    terrain's."""
    r, g, b = colour
    return max(r, g, b) - min(r, g, b) < 24


def terrain(exports, kind, size, seed=7):
    """The ground in the arcade maps' manner: clouds of a colour ramp, the ramp taken from an arcade map
    (Beginner's greens, Advanced's sands)."""
    source = {'green': ('select-rte', 'course', 0), 'sand': ('select-adv', 'course_cursor1', 1)}[kind]
    a = load_rgba(exports / source[0] / source[1] / f'map_{source[2]}.tga')[4:-4, 4:-4, :3].reshape(-1, 3).astype(int)
    r, g, b = a[:, 0], a[:, 1], a[:, 2]
    pick = (g > r + 30) & (g > b + 30) if kind == 'green' else (r > g + 20) & (g > b + 10)
    ramp = a[pick]
    ramp = ramp[np.argsort(ramp.sum(axis=1))]
    rng = np.random.default_rng(seed)
    field = np.zeros((size, size))
    for octave, cells in enumerate((4, 8, 16, 32, 64)):
        grid = rng.random((cells + 1, cells + 1))
        field += np.asarray(Image.fromarray((grid * 255).astype(np.uint8)).resize((size, size), Image.BICUBIC),
                            float) * 0.55 ** octave
    rank = field.argsort(axis=None).argsort().reshape(size, size) / (size * size)
    return ramp[np.minimum((rank * len(ramp)).astype(int), len(ramp) - 1)].astype(np.uint8)


def quantize(rgba):
    img = Image.fromarray(rgba[:, :, :3]).quantize(colors=16, method=Image.Quantize.MEDIANCUT).convert('RGB')
    return [bytes((b, g, r, 255)) for (r, g, b) in img.get_flattened_data()]


def tga_pixels(rgba):
    return [bytes((int(p[2]), int(p[1]), int(p[0]), int(p[3]))) for p in rgba.reshape(-1, 4)]


# --- the select pack ----------------------------------------------------------------------------------------
def read_dsl(path):
    blob = path.read_bytes()
    magic, version, textures, palettes, quads = struct.unpack_from('<4s4I', blob, 0)
    if magic != b'DSL1' or version != 1:
        raise ValueError(f'{path}: not a DSL1 select layout')
    anchors = struct.unpack_from('<4i', blob, 20)
    at = 36
    tex = [struct.unpack_from('<4HI', blob, at + 12 * i) for i in range(textures)]; at += 12 * textures
    pals = [blob[at + 64 * i:at + 64 * i + 64] for i in range(palettes)]; at += 64 * palettes
    qs = [struct.unpack_from('<HhhHHHHH', blob, at + 16 * i) for i in range(quads)]
    return anchors, tex, pals, qs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('exports', type=Path)
    ap.add_argument('cce', type=Path)
    ap.add_argument('maps', type=Path)
    ap.add_argument('assets', type=Path, nargs='?')
    ap.add_argument('--preview', type=Path, help='write PNG previews of the new art there')
    a = ap.parse_args()
    art = {}
    for slot, course in COURSES.items():
        spec = json.loads((a.maps / f'course{slot}-map.json').read_text())
        dim_p, lit_p = panel(a.exports, course, a.preview / f'panel{slot}.png' if a.preview else None)
        lit_m, dim_m, ratio = course_map(a.cce, course, spec['centre'], a.exports)
        if a.preview:
            Image.fromarray(np.concatenate([lit_m, dim_m], axis=1)).resize((MAP * 2 * 3, MAP * 3), Image.NEAREST) \
                .save(a.preview / f'map{slot}.png')
        art[slot] = (dim_p, lit_p, lit_m, dim_m)
        print(f'course {slot} ({course["name"]} {course["word"]}): panels {dim_p.shape[1]}x{dim_p.shape[0]}, '
              f'map {MAP}x{MAP} (dim scale {np.round(ratio, 3).tolist()})')
    if not a.assets:
        return
    select = a.assets / 'select'
    anchors, textures, palettes, quads = read_dsl(select / 'select.dsl')
    if any(q[7] >= 64 for q in quads):
        # Made before: its textures, banks and quads come last; drop them for the new ones.
        first = min(q[0] for q in quads if q[7] >= 64)
        quads = [q for q in quads if q[7] < 64]
        if any(q[0] >= first for q in quads):
            raise ValueError(f'{select}: the Circuit Edition textures are not the last ones')
        textures = textures[:first]
        palettes = palettes[:max(t[2] for t in textures) + 1]
        for index in range(first, 1 << 16):
            old = select / f'tex-{index}.dt'
            if not old.exists():
                break
            old.unlink()
        print(f'{select}: the Circuit Edition art made before replaced')
    encoder = shutil.which('pvrtex')
    if not encoder:
        raise ValueError('pvrtex missing (source enDjinn/environ.sh, as for the asset recipe)')
    pack = Pack(select, encoder)
    pack.textures = list(textures)
    pack.palettes = {('arcade', i): i for i in range(len(palettes))}
    small = [q for q in quads if 52 <= q[7] < 56][0]
    for slot, (dim_p, lit_p, lit_m, dim_m) in sorted(art.items()):
        k = slot - 3
        x, y = 0, 0                      # placed by the renderer (five courses: select_render.c)
        for variant, img in ((0, dim_p), (1, lit_p)):
            h, w = img.shape[:2]
            pack.quad(pack.texture(tga_pixels(img), w, h, False), x, y, w, h, 0, 0, 64 + 2 * k + variant)
        lit_index = None
        for lit, img in ((1, lit_m), (0, dim_m)):
            index = pack.texture(quantize(img), MAP, MAP, True)
            pack.quad(index, x, y, MAP, MAP, 0, 0, 68 + 2 * k + lit)
            if lit:
                lit_index = index
        pack.quad(lit_index, small[1], small[2], small[3], small[4], MAP, MAP, 72 + k)
    if len(pack.palettes) > 64:
        raise ValueError('select screens exceed 64 PAL4 banks')
    new_pals = []
    for pal, bank in sorted(pack.palettes.items(), key=lambda p: p[1]):
        if isinstance(pal, tuple):
            new_pals.append(palettes[bank])
            continue
        count = struct.unpack_from('<I', pal, 4)[0]
        colours = [struct.unpack_from('<I', pal, 8 + 4 * i)[0] for i in range(count)] + [0] * (16 - count)
        new_pals.append(struct.pack('<16I', *colours))
    blob = bytearray(b'DSL1' + struct.pack('<4I', 1, len(pack.textures), len(new_pals), len(quads) + len(pack.quads)))
    blob += struct.pack('<4i', *anchors)
    for t in pack.textures:
        blob += struct.pack('<4HI', *t)
    for p in new_pals:
        blob += p
    for q in list(quads) + pack.quads:
        blob += struct.pack('<HhhHHHHH', *q)
    path = select / 'select.dsl'
    path.unlink()                         # an asset set of links: never write through one
    path.write_bytes(bytes(blob))
    for p in select.glob('pal-*.pal'):
        p.unlink()
    print(f'{path}: {len(pack.textures) - len(textures)} textures, {len(new_pals) - len(palettes)} palette banks and '
          f'{len(pack.quads)} quads added')


if __name__ == '__main__':
    main()
