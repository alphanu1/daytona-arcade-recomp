"""Prepare the select screens' art (m2segment --export-native-select) as PAL4 textures and a layout.

python3 platform/dreamcast-native/tools/prepare_select.py RACE_TO_END_EXPORT ADVANCED_EXPORT EXPERT_EXPORT NEW_OUTPUT

RACE_TO_END_EXPORT holds course/, transmission/ and gentlemen/ of a run through the selects with the
course cursor on slot 0 (scripts/inputs/race_to_end.txt); ADVANCED_EXPORT and EXPERT_EXPORT hold
course_cursor1/ and course_cursor2/ (course_advanced.txt, course_expert.txt): the maps lit.
NEW_OUTPUT/select/ gets tex-N.dt (PAL4: the layers and sprites exact, the maps quantized to 16 colours)
and select.dsl, read by code/render/ui/select_render.c:
  'DSL1', u32 version 1, u32 textures, u32 palettes, u32 quads, i32 ring anchors (course x, y,
  transmission x, y: the select's frame centre +34/+38 when the ring was captured);
  per texture: u16 width, u16 height, u16 palette, u16 opaque, u32 file bytes;
  per palette: 16 x u32 ARGB8888 (unused entries 0);
  per quad: u16 texture, i16 x, i16 y, u16 width, u16 height, u16 u, u16 v, u16 role.
Roles: 1/2 course layers 2/0, 3/4 transmission layers, 5/6 GENTLEMEN layers 2/0, 8/9 course and
transmission frame rings, 16+2*slot+variant course panels (variant 1: the blink), 24+2*slot+variant
transmission panels, 32+digit timer digits (42 blank), 48+cursor CAR SELECT arrows, 52+slot the transmission
screen's small map (the slot's lit map texture, scaled; u/v span the map's own width/height), 56+2*slot+lit maps.
Outputs contain ROM-derived data and must stay in ignored local directories.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_dreamcast_textures import dt_info, exact_palette
from prepare_scene import read_tga, tga, check_pal4

ROLE_LAYERS = {('course', 2): 1, ('course', 0): 2, ('transmission', 2): 3, ('transmission', 0): 4,
               ('gentlemen', 2): 5, ('gentlemen', 0): 6}


def tga_size(path):
    data = path.read_bytes()
    return struct.unpack_from('<HH', data, 12)


def read_any(path):
    w, h = tga_size(path)
    return w, h, read_tga(path, w, h)


def pow2(n):
    p = 8
    while p < n:
        p *= 2
    return p


def split(pixels, width, opaque, x, y, w, h):
    """Rectangles of at most 256 (opaque) / 128 (alpha) a side and 16 colours, transparent ones dropped."""
    part = [pixels[(y + j) * width + x + i] for j in range(h) for i in range(w)]
    if not opaque and not any(p[3] for p in part):
        return []
    colours = set(part if opaque else (p if p[3] else bytes(4) for p in part))
    limit = 256 if opaque else 128
    if w > limit or h > limit or len(colours) > 16:
        if w <= 8 and h <= 8:
            raise ValueError('an 8x8 tile exceeds exact PAL4')
        if w >= h:
            return split(pixels, width, opaque, x, y, w // 2, h) + split(pixels, width, opaque, x + w // 2, y, w - w // 2, h)
        return split(pixels, width, opaque, x, y, w, h // 2) + split(pixels, width, opaque, x, y + h // 2, w, h - h // 2)
    return [(x, y, w, h)]


class Pack:
    def __init__(self, output, encoder):
        self.output, self.encoder = output, encoder
        self.textures, self.palettes, self.quads = [], {}, []

    def texture(self, pixels, w, h, opaque):
        """One PAL4 texture of the pow2 box around w x h source pixels (padding transparent/edge)."""
        tw, th = pow2(w), pow2(h)
        box = []
        for j in range(th):
            for i in range(tw):
                p = pixels[min(j, h - 1) * w + min(i, w - 1)] if opaque else (pixels[j * w + i] if i < w and j < h else bytes(4))
                if not opaque and not p[3]:
                    p = bytes(4)
                box.append(p if not opaque else p[:3] + b'\xff')
        source = tga(tw, th, box)
        index = len(self.textures)
        texture = self.output / f'tex-{index}.dt'
        src = texture.with_suffix('.tga')
        src.write_bytes(source)
        pal = exact_palette(source, tw, th)
        bank = self.palettes.setdefault(pal, len(self.palettes))
        (self.output / f'pal-{bank}.pal').write_bytes(pal)
        subprocess.run([self.encoder, '-i', str(src), '-o', str(texture), '-d', '0', '-f', 'PAL4BPP',
                        '--max-color', '16', '--palette', str(self.output / f'pal-{bank}.pal')],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        dt_info(texture, tw, th, 5, False)
        check_pal4(texture.read_bytes(), pal, box, tw, th)
        src.unlink()
        self.textures.append((tw, th, bank, int(opaque), texture.stat().st_size))
        return index

    def quad(self, texture, x, y, w, h, u, v, role):
        self.quads.append((texture, x, y, w, h, u, v, role))

    def layer(self, path, opaque, role):
        width, height, pixels = read_any(path)
        for (x, y, w, h) in split(pixels, width, opaque, 0, 0, 496, 384):
            part = [pixels[(y + j) * width + x + i] for j in range(h) for i in range(w)]
            if not opaque:  # trim to the drawn pixels
                rows = [j for j in range(h) if any(part[j * w + i][3] for i in range(w))]
                cols = [i for i in range(w) if any(part[j * w + i][3] for j in range(h))]
                if not rows:
                    continue
                y0, x0 = rows[0], cols[0]
                h2, w2 = rows[-1] + 1 - y0, cols[-1] + 1 - x0
                part = [part[(y0 + j) * w + x0 + i] for j in range(h2) for i in range(w2)]
                x, y, w, h = x + x0, y + y0, w2, h2
            self.quad(self.texture(part, w, h, opaque), x, y, w, h, 0, 0, role)

    def sprite(self, path, x, y, role, opaque=False):
        w, h, pixels = read_any(path)
        self.quad(self.texture(pixels, w, h, opaque), x, y, w, h, 0, 0, role)

    def map(self, path, x, y, role):
        from PIL import Image
        w, h = tga_size(path)
        image = Image.open(path).convert('RGB').quantize(colors=16, method=Image.Quantize.MEDIANCUT).convert('RGB')
        pixels = [bytes((b, g, r, 255)) for (r, g, b) in image.get_flattened_data()]
        self.quad(self.texture(pixels, w, h, True), x, y, w, h, 0, 0, role)


def table(path):
    return [line.split() for line in path.read_text().splitlines() if line and not line.startswith('#')]


def prepare(rte, advanced, expert, output):
    encoder = shutil.which('pvrtex')
    if not encoder:
        raise ValueError('source enDjinn/environ.sh first (pvrtex missing)')
    output.mkdir()
    out = output / 'select'
    out.mkdir()
    pack = Pack(out, encoder)
    for screen in ('course', 'transmission', 'gentlemen'):
        d = rte / screen
        pack.layer(d / 'layer_2.tga', True, ROLE_LAYERS[screen, 2])
        pack.layer(d / 'layer_0.tga', False, ROLE_LAYERS[screen, 0])
        if (d / 'sprites.txt').is_file():
            for role, x, y, w, h, name in table(d / 'sprites.txt'):
                pack.sprite(d / name, int(x), int(y), int(role))
    anchors = []
    for screen, role in (('course', 8), ('transmission', 9)):
        x, y, w, h, cx, cy, name = table(rte / screen / 'frame.txt')[0]
        pack.sprite(rte / screen / name, int(x), int(y), role)
        anchors += [int(cx), int(cy)]
    # Maps: lit from the run whose cursor was on the slot, dim from another run.
    lit = {0: rte / 'course', 1: advanced / 'course_cursor1', 2: expert / 'course_cursor2'}
    dim = {0: advanced / 'course_cursor1', 1: rte / 'course', 2: rte / 'course'}
    for slot in range(3):
        for state, source in ((1, lit[slot]), (0, dim[slot])):
            rows = {int(r[0]): r for r in table(source / 'maps.txt')}
            s, l, x, y, w, h, name = rows[slot]
            if int(l) != state:
                raise ValueError(f'map {slot}: {source} does not show it {"lit" if state else "dim"}')
            pack.map(source / name, int(x), int(y), 56 + 2 * slot + state)
    # The transmission screen's small map: the chosen course's lit map, scaled into the box the original draws it in.
    x, y, w, h, course = map(int, table(rte / 'transmission' / 'small_map.txt')[0])
    lit = {q[7]: q for q in pack.quads if q[7] >= 56}
    for slot in range(3):
        source = lit[57 + 2 * slot]
        pack.quad(source[0], x, y, w, h, source[3], source[4], 52 + slot)  # u, v: the source extent
    if len(pack.palettes) > 64:
        raise ValueError('select screens exceed 64 PAL4 banks')
    blob = bytearray(b'DSL1' + struct.pack('<4I', 1, len(pack.textures), len(pack.palettes), len(pack.quads)))
    blob += struct.pack('<4i', *anchors)
    for t in pack.textures:
        blob += struct.pack('<4HI', *t)
    for pal, bank in sorted(pack.palettes.items(), key=lambda p: p[1]):
        count = struct.unpack_from('<I', pal, 4)[0]
        colours = [struct.unpack_from('<I', pal, 8 + 4 * i)[0] for i in range(count)] + [0] * (16 - count)
        blob += struct.pack('<16I', *colours)
    for q in pack.quads:
        blob += struct.pack('<HhhHHHHH', *q)
    (out / 'select.dsl').write_bytes(bytes(blob))
    for p in out.glob('pal-*.pal'):
        p.unlink()
    vram = sum(t[0] * t[1] // 2 for t in pack.textures)
    report = dict(textures=len(pack.textures), palettes=len(pack.palettes), quads=len(pack.quads),
                  vram_texel_bytes=vram, maps='quantized to 16 colours', layers_and_sprites='exact PAL4')
    (output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(pack.textures)} PAL4 textures ({vram} bytes of texels), {len(pack.palettes)} banks, '
          f'{len(pack.quads)} quads; layers and sprites exact, maps quantized')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('race_to_end', type=Path)
    parser.add_argument('advanced', type=Path)
    parser.add_argument('expert', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    prepare(args.race_to_end, args.advanced, args.expert, args.output)
