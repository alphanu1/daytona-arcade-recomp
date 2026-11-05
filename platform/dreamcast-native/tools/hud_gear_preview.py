"""Pictures of the HUD's gear indicator in each build variant, drawn on the host from a HUD pack.

Usage: hud_gear_preview.py HUD_PACK_DIR OUT_DIR [--rpm RPM ...] [--needle-rpm RPM ...] [--cc CC]

HUD_PACK_DIR: tools/prepare_hud.py output (hud_data.h, fit.json, hud/sheet-N.dt, hud/hud.dp).
For each variant (digit, the default build: NM_HUD_GEAR_GATE=1 NM_HUD_GEAR_DIGIT=1, the "(n)" digit right of the
H-gate; gate: NM_HUD_GEAR_GATE=1 alone; original: the arcade's "(n) SHIFT", NM_HUD_GEAR_GATE=0)
tests/hud_gear_preview.c is compiled against the pack and writes the quad lists
of the settled gears 1..4 and of a shift 2 -> 3; they are drawn here in their paint order (the list's order,
nearest texel at arcade pixel centres, as tools/check_hud.py) over a plain sky and road. Writes into OUT_DIR:
  <variant>-gearN.png        the whole HUD, arcade 496x384, at 2x
  <variant>-gearN-crop.png   the indicator with the speed and the tachometer's right end, at 4x
  <variant>-extension-crop.png  gear 3 with "TIME EXTENSION!!" (ae98, row 12) under the indicator
  <variant>-shift.png        the crops of the shift 2 -> 3, every second tick, 4 a row
  <variant>-needle.png       gear 4 with the needle at each --needle-rpm (default 8000 to the stop), crops, labelled
  sheet.png                  the variants (rows) in gears 1..4 (columns), crops, labelled
The pictures hold ROM-derived art: keep them out of the repository.
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_hud import load_sheets  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
VARIANTS = (('digit', ['-DNM_HUD_GEAR_GATE=1', '-DNM_HUD_GEAR_DIGIT=1']), ('gate', ['-DNM_HUD_GEAR_GATE=1']),
            ('original', ['-DNM_HUD_GEAR_GATE=0']))
CROP = (196, 36, 348, 116)  # x0, y0, x1, y1 in arcade pixels: km/h, the indicator, the arc's "9" and "10"


def background():
    canvas = np.zeros((384, 496, 3), dtype=np.uint8)
    for y in range(384):
        if y < 176:
            t = y/176
            canvas[y] = (int(70+80*t), int(120+75*t), int(200+40*t))
        else:
            canvas[y] = (96, 98, 100) if (y//24) % 2 else (88, 90, 92)
    return canvas


def draw(path, sheets, canvas):
    """The quads in file order (paint order), nearest texel at pixel centres, texel alpha."""
    for line in path.read_text().splitlines():
        if not line or line[0] == '#':
            continue
        fields = line.split()
        texture = sheets[int(fields[0])]
        corners = [tuple(map(float, c.split(','))) for c in fields[1:]]
        for a, b, c in ((corners[0], corners[1], corners[2]), (corners[2], corners[1], corners[3])):
            det = (b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
            if abs(det) < 1e-9:
                continue
            x0, x1 = max(0, int(np.floor(min(a[0], b[0], c[0])))), min(496, int(np.ceil(max(a[0], b[0], c[0])))+1)
            y0, y1 = max(0, int(np.floor(min(a[1], b[1], c[1])))), min(384, int(np.ceil(max(a[1], b[1], c[1])))+1)
            if x0 >= x1 or y0 >= y1:
                continue
            px, py = np.meshgrid(np.arange(x0, x1)+0.5, np.arange(y0, y1)+0.5)
            w0 = ((b[1]-c[1])*(px-c[0])+(c[0]-b[0])*(py-c[1]))/det
            w1 = ((c[1]-a[1])*(px-c[0])+(a[0]-c[0])*(py-c[1]))/det
            w2 = 1-w0-w1
            inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
            u = w0*a[2]+w1*b[2]+w2*c[2]
            v = w0*a[3]+w1*b[3]+w2*c[3]
            tx = np.clip((u*texture.shape[1]).astype(int), 0, texture.shape[1]-1)
            ty = np.clip((v*texture.shape[0]).astype(int), 0, texture.shape[0]-1)
            colour = texture[ty, tx]
            ink = inside & ((colour >> 24) > 0)
            region = canvas[y0:y1, x0:x1]
            region[ink] = np.stack(((colour >> 16) & 255, (colour >> 8) & 255, colour & 255), axis=-1)[ink]


def picture(path, sheets):
    canvas = background()
    draw(path, sheets, canvas)
    return Image.fromarray(canvas)


def crop(image, scale):
    part = image.crop(CROP)
    return part.resize((part.width*scale, part.height*scale), Image.NEAREST)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('pack', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--rpm', type=lambda s: int(s, 0), nargs='+', default=[0x1d00],
                        help='filtered rpm +154 of the needle (default 0x1d00, near "9"; 0x2134 is the stop)')
    parser.add_argument('--needle-rpm', type=lambda s: int(s, 0), nargs='+', default=[0x1f40, 0x2008, 0x206c, 0x20d0, 0x2134],
                        help='filtered rpm of the needle in <variant>-needle.png (default 8000, 8200, 8300, 8400, 8500)')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('-D', dest='defines', action='append', default=[],
                        help='extra definition for every variant, e.g. NM_HUD_GEAR_DIGIT_AT_Y=89')
    args = parser.parse_args()
    import json
    fit = json.loads((args.pack/'fit.json').read_text())
    sheets = load_sheets(args.pack, fit)
    args.out.mkdir(parents=True, exist_ok=True)
    rows = []
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        for name, flags in VARIANTS:
            program = work/f'preview-{name}'
            subprocess.run([args.cc, '-std=c11', '-O1', *flags, *(f'-D{d}' for d in args.defines), f'-I{ROOT/"include"}', f'-I{args.pack}',
                            str(ROOT/'tests'/'hud_gear_preview.c'), '-o', str(program), '-lm'], check=True)
            for rpm in args.rpm:
                suffix = '' if len(args.rpm) == 1 else f'-rpm{rpm:x}'
                subprocess.run([str(program), str(work/f'{name}{suffix}'), str(rpm)], check=True)
                crops = []
                for gear in range(1, 5):
                    image = picture(work/f'{name}{suffix}-gear{gear}', sheets)
                    image.resize((992, 768), Image.NEAREST).save(args.out/f'{name}{suffix}-gear{gear}.png')
                    crop(image, 4).save(args.out/f'{name}{suffix}-gear{gear}-crop.png')
                    crops.append(crop(image, 2))
                rows.append((f'{name}{suffix}', crops))
                crop(picture(work/f'{name}{suffix}-extension', sheets), 4).save(args.out/f'{name}{suffix}-extension-crop.png')
                ticks = [picture(work/f'{name}{suffix}-shift-{t:02d}', sheets) for t in range(30, -1, -2)]
                w, h = CROP[2]-CROP[0], CROP[3]-CROP[1]
                strip = Image.new('RGB', (4*w, (len(ticks)+3)//4*h))
                for i, image in enumerate(ticks):
                    strip.paste(image.crop(CROP), (i % 4*w, i//4*h))
                strip.resize((strip.width*2, strip.height*2), Image.NEAREST).save(args.out/f'{name}{suffix}-shift.png')
            # The needle through the indicator: gear 4 at each --needle-rpm, labelled.
            w, h = 4*(CROP[2]-CROP[0]), 4*(CROP[3]-CROP[1])
            strip = Image.new('RGB', (len(args.needle_rpm)*(w+8), h+20), (32, 32, 32))
            pen = ImageDraw.Draw(strip)
            for i, rpm in enumerate(args.needle_rpm):
                subprocess.run([str(program), str(work/f'{name}-needle{rpm:x}'), str(rpm)], check=True)
                strip.paste(crop(picture(work/f'{name}-needle{rpm:x}-gear4', sheets), 4), (i*(w+8), 20))
                pen.text((i*(w+8)+6, 4), f'{rpm} rpm' + (' (the stop)' if rpm >= 0x2134 else ''), fill=(230, 230, 230))
            strip.save(args.out/f'{name}-needle.png')
    width, height = rows[0][1][0].size
    label = 90
    sheet = Image.new('RGB', (label+4*(width+8), 24+len(rows)*(height+8)), (32, 32, 32))
    pen = ImageDraw.Draw(sheet)
    for gear in range(4):
        pen.text((label+gear*(width+8)+width//2-20, 6), f'gear {gear+1}', fill=(230, 230, 230))
    for row, (name, crops) in enumerate(rows):
        y = 24+row*(height+8)
        pen.text((6, y+height//2-6), name, fill=(230, 230, 230))
        for gear, image in enumerate(crops):
            sheet.paste(image, (label+gear*(width+8), y))
    sheet.save(args.out/'sheet.png')
    print(f'wrote {args.out}')


if __name__ == '__main__':
    main()
