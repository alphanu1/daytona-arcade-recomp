"""The podium screen (render/ui/podium_render.c) against the arcade's own frames.

Composites what the native draws at a podium timer from the generated parts (podium_screen_data.h, the PAL4
textures and palettes of ASSET_DIR/podium) with podium_render.c's rules (the banner's rise, the photo's zoom, the
background, the logos and the text, the row-scroll wipe), and compares it pixel by pixel with frames of the
original (m2segment ... --catalog DIR on the second-place script: frame F's file is frame_(F+1).tga, its timer
14385 - F; its 3D, the photo, shows the tick before). The 2D layers must be exact (but for up to 100 pixels
of the cinematic over the rising banner); the photo (bilinear on both sides) within a mean of 5 per channel.

usage: test_podium_screen.py HEADER ASSET_DIR PHOTO_TGA FRAME_TGA:FRAME [FRAME_TGA:FRAME ...]   (exit 77 without data)
PHOTO_TGA: the photo export's texture (m2segment --export-native-objects of list 288cb94: materials/texture_1.tga).
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from prepare_material_crops import block_index  # noqa: E402

SPAWN, PHOTO_FROM, PHOTO_STEPS, LOGOS_FROM, TEXT_FROM, LAYER_A_UNTIL = 1598, 1213, 40, 1408, 1409, 1570
BACKGROUND_TIMER, WIPE_FROM, BANNER_ROWS = 383, 40, 49
BANNER, PHOTO, BACKGROUND, SEGA, AM2, TEXT = range(6)


def tga(path):
    d = Path(path).read_bytes()
    w, h = struct.unpack_from('<HH', d, 12)
    top = bool(d[17] & 0x20)
    return w, h, lambda x, y: tuple(d[18+d[0]+4*(w*(y if top else h-1-y)+x):][:3][::-1])   # (r, g, b)


def load(header, assets):
    text = Path(header).read_text()
    parts = [tuple(map(int, m)) for m in re.findall(r'\{(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)\}', text)]
    first = {name: int(re.search(name+r' = (\d+)', text).group(1)) for name in ('NM_PODIUM_CINEMATIC_BANK', 'NM_PODIUM_STATIC_BANK')}
    palette = {}
    for name, bank in (('cinematic.dp', first['NM_PODIUM_CINEMATIC_BANK']), ('static.dp', first['NM_PODIUM_STATIC_BANK'])):
        d = (Path(assets)/name).read_bytes()
        n = struct.unpack_from('<I', d, 4)[0]
        for i, c in enumerate(struct.unpack_from('<%dI' % n, d, 8)):
            palette[bank*16+i] = ((c >> 16) & 255, (c >> 8) & 255, c & 255)
    textures = []
    for i, p in enumerate(parts):
        d = (Path(assets)/('podium-%03d.dt' % i)).read_bytes()
        tw, th = p[6], p[7]
        pens = [0]*(tw*th)
        for y in range(th):
            for x in range(tw):
                a = block_index(x, y, tw, th)
                pens[y*tw+x] = (d[32+a//2] >> ((a & 1)*4)) & 15
        textures.append(pens)
    return parts, palette, textures


def render(parts, palette, textures, timer):
    """The 2D picture podium_render.c makes at `timer` (None where nothing 2D lies), and the photo's rectangle."""
    k = SPAWN-timer
    img = [[None]*496 for _ in range(384)]
    top = [[False]*496 for _ in range(384)]   # a layer A pixel: over everything

    def draw(i, y_offset=0, scroll=0):
        role, bank, x0, y0, w, h, tw, th = parts[i]
        for dy in range(h):
            y = y0+y_offset+dy
            if not 0 <= y < 384:
                continue
            shift = 0 if not scroll else (-scroll if y & 1 else scroll)
            for dx in range(w):
                pen = textures[i][dy*tw+dx]
                if role not in (PHOTO, BACKGROUND) and not pen:
                    continue
                x = x0+dx+shift
                if 0 <= x < 496:
                    img[y][x] = palette[bank*16+pen]
                    top[y][x] = role not in (PHOTO, BACKGROUND)
    if timer <= BACKGROUND_TIMER:
        for i, p in enumerate(parts):
            if p[0] == BACKGROUND:
                draw(i)
    photo = None
    if k >= PHOTO_FROM:
        n = max(0, PHOTO_STEPS-(k-PHOTO_FROM))
        s = 0.29+0.05*min(n, 26)
        photo = (248-560*s, 196-560*s, 560*s)
    if k < LAYER_A_UNTIL:
        scroll = 25*(WIPE_FROM-timer) if timer <= WIPE_FROM else 0
        banner = 8*(max(1, BANNER_ROWS-k)-1)
        for i, p in enumerate(parts):
            if p[0] == BANNER or (p[0] in (SEGA, AM2) and k >= LOGOS_FROM) or (p[0] == TEXT and k >= TEXT_FROM):
                draw(i, banner if p[0] == BANNER else 0, scroll)
    return img, top, photo


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    header, assets, photo_tga = sys.argv[1:4]
    if not all(Path(p).exists() for p in (header, assets, photo_tga)):
        print('SKIP: no podium screen data')
        return 77
    parts, palette, textures = load(header, assets)
    pw, ph, texel = tga(photo_tga)

    def bilinear(u, v):
        u -= 0.5
        v -= 0.5
        x0, y0 = int(u//1), int(v//1)
        fx, fy = u-x0, v-y0
        out = [0.0]*3
        for dx, dy, w in ((0, 0, (1-fx)*(1-fy)), (1, 0, fx*(1-fy)), (0, 1, (1-fx)*fy), (1, 1, fx*fy)):
            c = texel(min(pw-1, max(0, x0+dx)), min(ph-1, max(0, y0+dy)))
            for a in range(3):
                out[a] += w*c[a]
        return out
    failures = 0
    for item in sys.argv[4:]:
        path, frame = item.rsplit(':', 1)
        if not Path(path).exists():
            print('SKIP: %s' % path)
            continue
        timer = 14385-int(frame)
        _, _, pixel = tga(path)
        img, top, photo = render(parts, palette, textures, timer)
        # The arcade shows its 3D one frame after its 2D layers: its photo in this frame is the tick before's.
        photo = render(parts, palette, textures, timer+1)[2]
        inside = (lambda x, y: abs(x+0.5-248) < photo[2] and abs(y+0.5-196) < photo[2]) if photo else (lambda x, y: False)
        exact = differ = 0
        for y in range(384):
            for x in range(496):
                c = img[y][x]
                if c is None or (not top[y][x] and inside(x, y)):
                    continue   # nothing 2D, or the background under the photo
                if pixel(x, y) == c:
                    exact += 1
                else:
                    differ += 1
        error = count = 0
        if photo:
            half = photo[2]
            for sy in range(0, 384, 5):
                for sx in range(0, 496, 5):
                    if top[sy][sx]:
                        continue
                    X, Y = (sx+0.5-248)/half, (196-(sy+0.5))/half
                    u, v = (1-Y)/2*pw, (X+1)/2*ph
                    if not (1 <= u < pw-1 and 1 <= v < ph-1):
                        continue
                    c, p = bilinear(u, v), pixel(sx, sy)
                    error += max(abs(c[a]-p[a]) for a in range(3))
                    count += 1
        mean = error/count if count else 0.0
        ok = differ <= 100 and mean <= 5.0   # 100: the arcade's 3D cinematic over the rising banner

        failures += not ok
        print('frame %s (timer %d): 2D exact %d, differing %d, photo mean error %.1f over %d samples: %s'
              % (frame, timer, exact, differ, mean, count, 'PASS' if ok else 'FAIL'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
