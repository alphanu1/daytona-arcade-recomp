"""Per-cell colour error of a captured tests/palette_probe frame.

Usage: check_palette_probe.py CAPTURE.png probe.json --page 0|1
Finds the four magenta corner markers, maps the 640x480 sheet onto the capture
(any scale or offset), and for every cell reports the mean and maximum 8-bit
error of the captured (a) exact-palette, (b) gain-bucket and (c) pack-anchor
quads against the host's exact colours, and of (b) and (c) against the captured
(a). Each 64x64 quad is averaged down to its 32x32 texels, ignoring a one-texel
border (bilinear rows blend there). Capture one frame per page and palette
format; the palette format only changes what the console shows, not the
reference.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

MARKERS = np.array([[12, 12], [628, 12], [12, 468], [628, 468]], float)  # Centres of the 8x8 squares.


def find_markers(image):
    """Centres of the four pure-magenta squares. Each is searched only in the
    outer 6% band of its own corner, where no cell is drawn, so coloured
    content elsewhere on the sheet cannot pull the centroid."""
    r, g, b = (image[..., i].astype(int) for i in range(3))
    mask = (r > 200) & (b > 200) & (g < 80)
    h, w = mask.shape
    bw, bh = int(w*0.06), int(h*0.085)
    found = []
    for right, bottom in ((0, 0), (1, 0), (0, 1), (1, 1)):
        x0, y0 = (w-bw if right else 0), (h-bh if bottom else 0)
        ys, xs = np.nonzero(mask[y0:y0+bh, x0:x0+bw])
        if len(xs) < 9:
            raise SystemExit('a magenta corner marker is missing')
        found.append((x0+(xs.min()+xs.max()+1)/2.0, y0+(ys.min()+ys.max()+1)/2.0))
    return np.array(found)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('capture', type=Path)
    parser.add_argument('probe', type=Path)
    parser.add_argument('--page', type=int, required=True, choices=(0, 1))
    args = parser.parse_args()
    probe = json.loads(args.probe.read_text())
    image = np.asarray(Image.open(args.capture).convert('RGB'))
    found = find_markers(image)
    # Affine map sheet (x, y, 1) -> capture pixels, least squares over four markers.
    A = np.column_stack([MARKERS, np.ones(4)])
    transform, *_ = np.linalg.lstsq(A, found, rcond=None)
    scale = (abs(transform[0, 0]), abs(transform[1, 1]))
    print(f'capture {image.shape[1]}x{image.shape[0]}; sheet pixel = {scale[0]:.3f} x {scale[1]:.3f} capture pixels; '
          f'marker fit residual {np.abs(A@transform-found).max():.2f} px')

    def texels(x, y, size):
        """Mean captured colour of each of the 32x32 texels of a quad."""
        out = np.zeros((32, 32, 3))
        step = size/32.0
        for v in range(32):
            for u in range(32):
                x0, y0 = np.array([x+u*step+0.25*step, y+v*step+0.25*step, 1]) @ transform
                x1, y1 = np.array([x+(u+1)*step-0.25*step, y+(v+1)*step-0.25*step, 1]) @ transform
                xa, xb = sorted((int(round(x0)), int(round(x1))))
                ya, yb = sorted((int(round(y0)), int(round(y1))))
                out[v, u] = image[ya:max(yb, ya+1), xa:max(xb, xa+1)].reshape(-1, 3).mean(0)
        return out

    def error(a, b):
        d = np.abs(a[1:-1, 1:-1]-b[1:-1, 1:-1])
        return d.mean(), d.max()
    cell, (ox, oy) = probe['cell'], probe['origin']
    cells = probe['pages'][args.page]['cells']
    print('texture luma filter   | a vs host exact | b vs host exact | c vs host exact | b vs captured a | c vs captured a   (mean / max, 8-bit)')
    totals = {}
    for c in cells:
        expected = np.array(c['expected'], float).reshape(32, 32, 3)
        for filtered in (0, 1):
            x = ox+c['light_index']*3*cell
            y = oy+(c['texture']*2+filtered)*cell
            a, b, pack = (texels(x+k*cell, y, cell) for k in range(3))
            rows = [error(a, expected), error(b, expected), error(pack, expected), error(b, a), error(pack, a)]
            print(f'   {c["texture"]}    {c["light"]:3d}  {"bilinear" if filtered else "point   "} | '
                  + ' | '.join(f'{m:6.2f} / {x_:5.1f}' for m, x_ in rows))
            for name, row in zip(('a', 'b', 'c'), rows[:3]):
                totals.setdefault((name, filtered), []).append(row[0])
    for (name, filtered), values in sorted(totals.items()):
        print(f'mean error of ({name}) against host exact, {"bilinear" if filtered else "point"} rows: {np.mean(values):.2f}')
    c = cells[1]
    y = oy+6*cell+8
    expected = np.array(c['expected'], float).reshape(32, 32, 3)
    names = ('a at 1x', 'b at 1x', 'VQ+PAL4 wide', 'VQ+PAL4 narrow')
    for k, name in enumerate(names):
        m, x_ = error(texels(ox+64*k, y, 32), expected)
        print(f'bottom row {name}: mean {m:.2f} max {x_:.1f} against host exact (a wrong VQ encoding shows as a large error)')


if __name__ == '__main__':
    main()
