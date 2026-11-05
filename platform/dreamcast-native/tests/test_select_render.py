"""Select screens' native picture against the original's (code/render/ui/select_render.c).

usage: test_select_render.py ASSET_DIR EXPORT_DIR [NEW_PNG_DIR]

ASSET_DIR holds select/ (tools/prepare_select.py), EXPORT_DIR the race_to_end export it came from (m2segment
--export-native-select: course/, transmission/, gentlemen/ with screen.tga and pose.txt). The renderer is built
for the host (tests/select_preview.c: the PVR calls stubbed) and draws the pose the original had on each captured
frame; the quads are rasterized (point sampled, PAL4 decoded from the .dt files, palette alpha as punch-through,
depth GEQUAL) at 496x384 and compared with the original's screen. Every pixel must be equal but these, which the
check leaves out and counts: the maps and the small map (their textures are quantized to 16 colours) and, on the
transmission
screen, what the original draws in 3D and the native screen does not yet (the two cars and the small map).
Exits 77 when the inputs are missing."""
import struct, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools'))
from prepare_scene import read_tga


def tga(path):
    data = path.read_bytes()
    w, h = struct.unpack_from('<HH', data, 12)
    return w, h, read_tga(path, w, h)


def decode_pal4(path):
    data = path.read_bytes()
    w, h = struct.unpack_from('<HH', data, 12)
    start = (data[9] + 1) * 32
    side = min(w, h)
    def spread(v):
        return sum(((v >> b) & 1) << (2 * b) for b in range(side.bit_length() - 1))
    pens = []
    for y in range(h):
        for x in range(w):
            index = (x // side + y // side) * side * side + (spread(y % side) | (spread(x % side) << 1))
            pens.append((data[start + index // 2] >> ((index & 1) * 4)) & 15)
    return w, h, pens


def render(preview, assets, pose):
    out = subprocess.run([str(preview), str(assets) + '/'] + pose, capture_output=True, text=True, check=True).stdout
    palette, textures, quads = {}, {}, []
    for line in out.splitlines():
        f = line.split()
        if f[0] == 'P':
            palette[int(f[1])] = int(f[2], 16)
        elif f[0] == 'T':
            textures[int(f[1])] = (f[2], int(f[5]), int(f[6]))
        elif f[0] == 'Q':
            quads.append((int(f[1]), int(f[2]), [float(v) for v in f[3:19]], float(f[19])))
    decoded = {}
    pixels = [None] * (496 * 384)
    depth = [-1.0] * (496 * 384)
    for lst, tex, v, z in sorted(quads, key=lambda q: q[0] != 0):  # opaque list first, then punch-through
        name, bank, opaque = textures[tex]
        if tex not in decoded:
            decoded[tex] = decode_pal4(assets / 'select' / name)
        tw, th, pens = decoded[tex]
        xs, ys = [v[0], v[4], v[8], v[12]], [v[1], v[5], v[9], v[13]]
        us, vs = [v[2], v[6], v[10], v[14]], [v[3], v[7], v[11], v[15]]
        x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
        u0, u1 = us[xs.index(x0)], us[xs.index(x1)]
        v0, v1 = vs[ys.index(y0)], vs[ys.index(y1)]
        sx, sy = 640 / 496, 480 / 384
        for py in range(max(0, round(y0 / sy)), min(384, round(y1 / sy))):
            fy = ((py + 0.5) * sy - y0) / (y1 - y0)
            ty = min(th - 1, int((v0 + (v1 - v0) * fy) * th))
            for px in range(max(0, round(x0 / sx)), min(496, round(x1 / sx))):
                fx = ((px + 0.5) * sx - x0) / (x1 - x0)
                tx = min(tw - 1, int((u0 + (u1 - u0) * fx) * tw))
                colour = palette[16 * bank + pens[ty * tw + tx]]
                if lst != 0 and colour >> 24 < 128:
                    continue
                i = py * 496 + px
                if z >= depth[i]:
                    depth[i], pixels[i] = z, colour & 0xffffff
    return pixels


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__); return 2
    assets, export = Path(sys.argv[1]), Path(sys.argv[2])
    png = Path(sys.argv[3]) if len(sys.argv) == 4 else None
    if not (assets / 'select' / 'select.dsl').is_file() or not (export / 'course' / 'pose.txt').is_file():
        print('no select assets or export, skipped'); return 77
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        stub = Path(tmp) / 'include'
        for name in ('enDjinn/enj_defs.h', 'enDjinn/enj_enDjinn.h', 'sh4zam/shz_sh4zam.h'):
            (stub / name).parent.mkdir(parents=True, exist_ok=True)
            (stub / name).write_text('')
        (stub / 'malloc.h').write_text('#include <stdlib.h>\nstatic inline void *memalign(size_t a,size_t n) '
                                       '{ void *p=NULL; return posix_memalign(&p,a,n) ? NULL : p; }\n')
        preview = Path(tmp) / 'preview'
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wno-unused-function', '-fsanitize=address,undefined',
                        '-I', str(stub), '-I', str(HERE.parent / 'include'), str(HERE / 'select_preview.c'),
                        '-o', str(preview)], check=True)
        for screen in ('course', 'transmission', 'gentlemen'):
            d = export / screen
            pose = [l for l in (d / 'pose.txt').read_text().splitlines() if not l.startswith('#')][0].split()[:7]
            native = render(preview, assets, pose)
            w, h, original = tga(d / 'screen.tga')
            _, _, composite = tga(d / 'composite.tga')
            maps = set()
            if (d / 'maps.txt').is_file():
                for r in [l.split() for l in (d / 'maps.txt').read_text().splitlines() if not l.startswith('#')]:
                    x, y, mw, mh = map(int, r[2:6])
                    maps |= {(yy * 496 + xx) for yy in range(y, y + mh) for xx in range(x, x + mw)}
            if (d / 'small_map.txt').is_file():
                r = [l.split() for l in (d / 'small_map.txt').read_text().splitlines() if not l.startswith('#')][0]
                x, y, mw, mh = map(int, r[:4])
                maps |= {(yy * 496 + xx) for yy in range(y, y + mh) for xx in range(x, x + mw)}
            differ = skipped = 0
            first = None
            for i in range(496 * 384):
                o = int.from_bytes(original[i][:3], 'little')
                if i in maps or (screen == 'transmission' and original[i] != composite[i] and native[i] != o):
                    skipped += 1
                    continue
                if native[i] != o:
                    differ += 1
                    first = first or (i % 496, i // 496, hex(native[i] or 0), hex(o))
            print(f'{screen}: {496 * 384 - skipped - differ} pixels equal, {differ} differ, {skipped} left out (maps, 3D not drawn)')
            if differ:
                problems.append(f'{screen}: {differ} pixels differ, first at {first}')
            if png:
                from PIL import Image
                png.mkdir(exist_ok=True)
                image = Image.new('RGB', (992, 384))
                image.putdata([((c or 0) >> 16 & 255, (c or 0) >> 8 & 255, (c or 0) & 255) for c in native] + [], )
                left = Image.new('RGB', (496, 384)); left.putdata([((c or 0) >> 16 & 255, (c or 0) >> 8 & 255, (c or 0) & 255) for c in native])
                right = Image.new('RGB', (496, 384)); right.putdata([(p[2], p[1], p[0]) for p in original])
                image.paste(left, (0, 0)); image.paste(right, (496, 0))
                image.save(png / f'{screen}.png')
    for p in problems:
        print('FAIL:', p)
    if problems:
        return 1
    print('PASS: select screens drawn as the original shows them (maps and the 3D cars left out)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
