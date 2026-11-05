"""Compare the native HUD rules with what the original displays, tick by tick.

Usage: check_hud.py HUD_TRACE_DIR HUD_PACK_DIR DISPLAY_FILE [--preview-dir NEW_DIR]

HUD_TRACE_DIR: `m2segment --trace-hud` output (reference tile words, overlay polygons).
HUD_PACK_DIR:  tools/prepare_hud.py output (fit.json, sheets, palette).
DISPLAY_FILE:  rows written by tests/check_hud.c (rules on the reference's source
               variables) or by tests/trace_race_native.c --hud (rules on the native race).
Tile cells must agree exactly on every compared frame; overlay polygons within
--tolerance pixels. The original shows its polygons one frame after the tiles, so
the reference polygons of frame F are compared with the native row of frame F-1
(the course map's markers, the needle and the CONDITION panel: its icons, the
set of cars shown, the panel; its zoom +34 is compared bit for bit on the same
frame). The messages a818 and ae98 are compared where the reference's rows show
the block or blanks.
Frames before the first phase-22 frame are skipped (the reference trace samples
the scene load mid-pass). A frame whose reference cells equal the native row of
the previous frame is counted as "reference one tick stale", not as agreement. With --preview-dir, every DISPLAY_FILE.quads-FRAME is drawn from the
exported PAL4 sheets next to the reference frame of the same tick (needs a snapshot).
"""
import argparse
import json
from pathlib import Path
import struct
import sys

import numpy as np
from PIL import Image

from prepare_hud import MAP_SCALES, bits_float, points, trace_rows, unit
from prepare_material_crops import block_index

BLANKS = {0x8280, 0x8020, 0x0020}


def glyph(value):
    return '_' if value is None else str(value) if value < 10 else {10: 'm', 11: 's'}.get(value, '?')


def big(value):
    return '_' if value is None else str(value) if value < 10 else 'abcd'[value-11] if 11 <= value <= 14 else '/'


def decode(row, fit):
    """Reference tile words -> the cell text of tests/hud_dump.h."""
    words = lambda name: [int(w, 16) for w in row[name].split('.')]

    def family(word, base, step, count):
        if word in BLANKS or not base <= word < base+step*count or (word-base) % step:
            return None
        return (word-base)//step

    def small(word):
        if word in BLANKS or not 0x8000 <= word < 0xc000:
            return None, None
        return ((word-0x8000) & 0x7f)//2, (word-0x8000) >> 7
    timer = words('timer')
    out = {'timer': ''.join(glyph(family(timer[i], 0x8500, 12, 10)) for i in (0, 3))}
    circles = words('L2')
    out['lap'] = '%s/%s' % tuple(glyph(family(circles[i], 0x8780, 4, 12)) for i in (6, 10))
    lap_rows = []
    for index in range(8):
        cells = words(f'L{4+2*index}')
        number = [small(cells[i])[0] for i in (3, 4)]
        time = [small(cells[i]) for i in range(5, 13)]
        if all(v is None for v in number) and all(v[0] is None for v in time):
            break
        colours = {c for v, c in time if v is not None}
        lap_rows.append('%s|%s|%u' % (''.join(glyph(v) for v in number), ''.join(glyph(v) for v, _ in time), colours == {14}))
        if len(colours) > 1 or not colours <= {13, 14}:
            lap_rows[-1] += '?'
    out['rows'] = lap_rows
    position = words('P3')
    out['rank'] = ''.join(big(family(position[i], 0x8580, 8, 15)) for i in (2, 4, 6))
    out['slash'] = big(family(position[8], 0x8580, 8, 15))
    out['count'] = ''.join(big(family(position[i], 0x8580, 8, 15)) for i in (10, 12))
    speed = words('S9')
    out['speed'] = ''.join(glyph(family(speed[i], 0x8120, 4, 10)) for i in (3, 5, 7))
    out['unit'] = family(speed[9], 0x8120, 4, 11)
    # a818: the message block covers rows 21..26 from column 25 and is erased to 0x0020. Other art
    # shares the area (the start and time-up messages): those frames are not compared.
    if 'M25' in row and 'reverse_row25' in fit:
        cells = words('M25')
        if cells == fit['reverse_row25']:
            out['reverse'] = '1'
        elif all(w in BLANKS for w in cells):
            out['reverse'] = '0'
    # ae98: the first row of the block at column 19 of row 12 (bit0) or 16 (bit1); row 16 also holds the
    # lap time message (ad34, not native yet): frames where a row shows other art are not compared.
    if 'X12' in row and 'extension_row' in fit:
        for bit, name in ((1, 'X12'), (2, 'X16')):
            cells = words(name)
            if cells == fit['extension_row']:
                out[f'extension{bit}'] = '1'
            elif all(w in BLANKS for w in cells):
                out[f'extension{bit}'] = '0'
    # The rolling start's overlay (ab78, d438): the hint on row 19, the digit or GO! on row 18, the banner and its car on
    # row 24 with the scroll of their lines. Frames with other art on a row are not compared on it.
    if 'R19' in row and 'rolling' in fit:
        r = fit['rolling']
        hint, cells = words('R19'), words('R18')
        banner = words('R24')
        if hint == r['hint_accel']:
            out['hint'] = '1'
        elif hint[0] in BLANKS and hint[1:24] == r['hint_auto'] and hint[24] in BLANKS:
            out['hint'] = '2'
        elif all(w in BLANKS for w in hint):
            out['hint'] = '0'
        if cells == r['go']:
            out['go'] = '1'; out['digit'] = '0'
        elif all(w in BLANKS for w in cells):
            out['go'] = '0'; out['digit'] = '0'
        elif all(w in BLANKS for w in cells[:6]+cells[15:]) and cells[6:15] in r['digits']:
            out['go'] = '0'; out['digit'] = str(r['digits'].index(cells[6:15])+1)
        if banner == r['row24']:
            out['banner'] = '1'
            out['scroll'] = str((-int(row['lscroll'], 16)) & 1023)
        elif all(w in BLANKS for w in banner):
            out['banner'] = '0'
    # ad34, the lap message: the label on row 15, the time's first tile row in X16 from column 22; the erased cells are
    # the font's space (six tiles). Frames with other art there are not compared.
    if 'M15' in row and 'lastlap' in row and 'lap_message' in fit:
        m = fit['lap_message']
        label, time = words('M15'), words('X16')[3:19]
        spaces = {m['font']+i for i in range(6)} | BLANKS
        glyphs = {word: index for index, word in enumerate(m['glyphs'])}
        if all(w in spaces for w in label):
            out['message'] = '0:'
        elif label in (m['lap'], m['best']):
            cells = [glyphs.get(time[i], None if time[i] in spaces else -1) for i in range(0, 16, 2)]
            if -1 not in cells:
                out['message'] = f"{1+(label == m['best'])}:"+''.join(glyph(c) for c in cells)
    # b0c0, the gear indicator (packs with 'gear'): rows 9..11 of columns 33..36 (S9..S11 start at column 22).
    # The lever frame f (3x3 tiles at column 34) or the digit d between its brackets with the label below,
    # '_' when not drawn; frames with other art there are not compared.
    if 'gear' in fit and 'S11' in row:
        g = fit['gear']
        rows = [words(f'S{r}')[11:15] for r in (9, 10, 11)]
        lever = [w for r in rows for w in r[1:]]
        if lever in g['lever']:
            out['gear'] = '%d_' % g['lever'].index(lever)
        elif all(w in BLANKS for w in lever+rows[2][:1]):  # column 33 of rows 9 and 10 is not the indicator's
            out['gear'] = '__'
        elif ([rows[0][1], rows[1][1]] == g['open'] and [rows[0][3], rows[1][3]] == g['close'] and rows[2] == g['label'][:4]
              and rows[0][2] in g['digits']):
            digit = g['digits'].index(rows[0][2])+1
            if rows[1][2] == rows[0][2]+1:
                out['gear'] = '_%d' % digit
    return out


def display_rows(path):
    result = {}
    for line in path.read_text().splitlines():
        if not line.startswith('D '):
            continue
        fields = dict(f.split('=', 1) for f in line.split()[1:])
        count = int(fields['rows'])
        fields['rows'] = [fields[f'r{i}'] for i in range(count)]
        if 'extension' in fields:
            fields['extension1'] = str(int(fields['extension']) & 1)
            fields['extension2'] = str(int(fields['extension']) >> 1 & 1)
        if 'roll' in fields:
            fields.update(hint=fields['roll'][0], digit=fields['roll'][1], go=fields['roll'][2], banner=fields['roll'][3])
        if fields.get('message', '').startswith('0:'):
            fields['message'] = '0:'
        result[int(fields['frame'])] = fields
    return result


def load_sheets(pack, fit):
    palette = (pack/'hud'/'hud.dp').read_bytes()
    colours = struct.unpack_from(f'<{(len(palette)-8)//4}I', palette, 8)
    sheets = []
    for index in range(1+max(s['sheet'] for s in fit['sprites'].values())):
        blob = (pack/'hud'/f'sheet-{index}.dt').read_bytes()
        bank = fit.get('sheet_banks', list(range(index+1)))[index]  # packs before the gear indicator: sheet = bank
        width, height = struct.unpack_from('<HH', blob, 12)
        pixels = np.zeros((height, width), dtype=np.uint32)
        for y in range(height):
            for x in range(width):
                at = block_index(x, y, width, height)
                pixels[y, x] = colours[16*bank+((blob[32+at//2] >> ((at & 1)*4)) & 15)]
        sheets.append(pixels)
    return sheets


def draw_quads(path, sheets, canvas):
    """Nearest-texel rasterizer at arcade pixel centres; returns the ink mask."""
    ink = np.zeros(canvas.shape[:2], dtype=bool)
    quads = [line.split() for line in path.read_text().splitlines() if line and line[0] != '#']
    for sheet in range(len(sheets)):
        for quad in quads:
            if int(quad[0]) != sheet:
                continue
            corners = [tuple(map(float, c.split(','))) for c in quad[1:]]
            texture = sheets[sheet]
            for a, b, c in ((corners[0], corners[1], corners[2]), (corners[2], corners[1], corners[3])):
                xs, ys = (a[0], b[0], c[0]), (a[1], b[1], c[1])
                det = (b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
                if abs(det) < 1e-9:
                    continue
                for y in range(max(0, int(np.floor(min(ys)))), min(canvas.shape[0], int(np.ceil(max(ys)))+1)):
                    for x in range(max(0, int(np.floor(min(xs)))), min(canvas.shape[1], int(np.ceil(max(xs)))+1)):
                        px, py = x+0.5, y+0.5
                        w0 = ((b[1]-c[1])*(px-c[0])+(c[0]-b[0])*(py-c[1]))/det
                        w1 = ((c[1]-a[1])*(px-c[0])+(a[0]-c[0])*(py-c[1]))/det
                        w2 = 1-w0-w1
                        if min(w0, w1, w2) < -1e-6:
                            continue
                        u = w0*a[2]+w1*b[2]+w2*c[2]; v = w0*a[3]+w1*b[3]+w2*c[3]
                        tx = min(texture.shape[1]-1, max(0, int(u*texture.shape[1])))
                        ty = min(texture.shape[0]-1, max(0, int(v*texture.shape[0])))
                        colour = int(texture[ty, tx])
                        if colour >> 24:
                            canvas[y, x] = ((colour >> 16) & 255, (colour >> 8) & 255, colour & 255)
                            ink[y, x] = True
    return ink


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('pack', type=Path)
    parser.add_argument('display', type=Path)
    parser.add_argument('--tolerance', type=float, default=0.05)
    parser.add_argument('--preview-dir', type=Path)
    parser.add_argument('--course', type=int, default=0, help='course slot of the race (a --course-map of the pack)')
    args = parser.parse_args()
    fit = json.loads((args.pack/'fit.json').read_text())
    reference = trace_rows(args.trace/'hud.trace')
    native = display_rows(args.display)
    turn = np.array(fit['turn'])
    matrices = fit['course_maps'][str(args.course)] if args.course else fit
    map_turn, marker_size = (matrices['turn'], matrices['size']) if args.course else (0, 1.0)
    seconds = 0  # frames the "2" marker was compared on
    failures, compared, polygons, worst = {}, 0, 0, 0.0
    first_failure = {}

    def fail(kind, frame, expected, actual):
        failures[kind] = failures.get(kind, 0)+1
        first_failure.setdefault(kind, (frame, expected, actual))
    started, stale = False, {}
    messages = [0, 0]  # wrong-way message frames compared, of them drawn
    extensions = [0, 0]  # time extension message cells compared, of them drawn
    gears = [0, 0, 0]  # gear indicator frames compared, of them with the lever, with the digit
    radar = dict(frames=0, icons=0, zoom=0, worst=0.0, panel_frames=0, mapsize=0)
    overlay, drawn = {}, {}  # the rolling start's overlay: frames compared and frames it is drawn on, by element
    lap_messages = [0, 0, 0]  # lap message (ad34) frames compared, with LAP TIME, with BEST LAP
    for row in reference:
        frame = int(row['frame'])
        started = started or row['active'] == '22'
        if not started or frame not in native or frame-1 not in native:
            continue
        shown, d, before = decode(row, fit), native[frame], native[frame-1]
        compared += 1
        if 'reverse' in shown and 'reverse' in d:
            messages[0] += 1; messages[1] += shown['reverse'] == '1'
        for bit in (1, 2):
            if f'extension{bit}' in shown and f'extension{bit}' in d:
                extensions[0] += 1; extensions[1] += shown[f'extension{bit}'] == '1'
        if 'zoom' in row and 'zoom' in d:  # the map task's +34 after this tick
            radar['zoom'] += 1
            if int(row['zoom'], 16) != int(d['zoom'].split(':')[1], 16) or row['zix'] != d['zoom'].split(':')[0]:
                fail('panel zoom', frame, (row['zix'], row['zoom']), d['zoom'])
        if 'mix' in row and 'mapsize' in d:  # and its +52, +38
            radar['mapsize'] += 1
            if row['mix'] != d['mapsize'].split(':')[0] or abs(bits_float(row['mscale'])-bits_float(d['mapsize'].split(':')[1])) > 1e-13:
                fail('map size', frame, (row['mix'], row['mscale']), d['mapsize'])
        if 'gear' in shown and 'gear' in d:
            gears[0] += 1; gears[1] += shown['gear'][0] != '_'; gears[2] += shown['gear'][1] != '_'
        for kind in ('hint', 'digit', 'go', 'banner', 'scroll'):
            if kind in shown and kind in d:
                overlay[kind] = overlay.get(kind, 0)+1
                drawn[kind] = drawn.get(kind, 0)+(shown[kind] != '0')
        if 'message' in shown and 'message' in d:
            lap_messages[0] += 1; lap_messages[1] += shown['message'][0] == '1'; lap_messages[2] += shown['message'][0] == '2'
        for kind in ('timer', 'lap', 'rank', 'count', 'speed', 'rows', 'reverse', 'extension1', 'extension2', 'gear',
                     'hint', 'digit', 'go', 'banner', 'scroll', 'message'):
            if kind not in shown or kind not in d:
                continue
            if shown[kind] != d[kind]:
                if shown[kind] == before[kind]:
                    stale[kind] = stale.get(kind, 0)+1
                else:
                    fail(kind, frame, shown[kind], d[kind])
        # Overlay polygons: shown one frame after their tick.
        d = before
        needle_on, needle_angle = d['needle'].split(':')
        if ('needle' in row) != (needle_on == '1'):
            fail('needle visibility', frame, 'needle' in row, needle_on)
        elif 'needle' in row and len(points(row['needle'])) == 4:
            c, s = unit(int(needle_angle))
            for k, (x, y) in enumerate(points(row['needle'])):
                ex = fit['needle_pivot'][0]+fit['needle_cos'][k][0]*c+fit['needle_sin'][k][0]*s
                ey = fit['needle_pivot'][1]+fit['needle_cos'][k][1]*c+fit['needle_sin'][k][1]*s
                error = max(abs(ex-x), abs(ey-y)); worst = max(worst, error)
                if error > args.tolerance:
                    fail('needle', frame, (x, y), (ex, ey))
            polygons += 1
        second = d.get('second') == '1' and fit.get('second_first')
        for key, matrix, local, source in (('player', 'affine', 'player_first', d['player'].split(',')),
                                           ('leader', 'affine_leader', 'second_first' if second else 'leader_first',
                                            d['leader'].split(':')[1].split(','))):
            if key not in row or len(points(row[key])) != len(fit[local]):
                continue
            seconds += key == 'leader' and bool(second)
            if d['map'] != '1' or (key == 'leader' and d['leader'][0] != '1'):
                fail(key+' marker visibility', frame, True, False)
                continue
            origin = np.array(matrices[matrix])@np.array([bits_float(source[0]), bits_float(source[1]), 1.0])
            if d.get('mapsize'):  # The size of the chord: the places scale about the model's origin (hud_draw.h).
                standard = bits_float('%08x' % MAP_SCALES[args.course][1])
                k, anchor = bits_float(d['mapsize'].split(':')[1])/standard, np.array(matrices['affine_leader'])[:, 2]
                origin = anchor+k*(origin-anchor)
            c, s = unit(int(source[2])+map_turn)
            for (x, y), offset in zip(points(row[key]), fit[local]):
                expected = origin+marker_size*(c*np.eye(2)+s*turn)@np.array(offset)
                error = float(np.abs(expected-np.array([x, y])).max()); worst = max(worst, error)
                if error > args.tolerance:
                    fail(key+' marker', frame, (x, y), expected.tolist())
            polygons += 1
        # CONDITION panel (12c98..13208): drawn with the map, from the previous tick's cars.
        if 'rpanel' in row and 'radar' in fit:
            r = fit['radar']
            if d['map'] != '1' or 'zoom' not in d:
                fail('panel visibility', frame, True, False)
                continue
            radar['frames'] += 1
            error = max(abs(a-b) for v, (x, y) in zip(points(row['rpanel']), ((r['panel'][0], r['panel'][3]), (r['panel'][0], r['panel'][1]),
                        (r['panel'][2], r['panel'][1]), (r['panel'][2], r['panel'][3]))) for a, b in zip(v, (x, y)))
            radar['panel_frames'] += error <= args.tolerance
            if error > args.tolerance:
                fail('panel', frame, row['rpanel'], r['panel'])
            zoom = bits_float(d['zoom'].split(':')[1]); k = zoom/r['zoom']
            centre, matrix = np.array(r['centre']), np.array(r['matrix'])
            groups = row['ricons'].split('|') if 'ricons' in row else []
            icons = []
            if d['radar'] != '0':
                for item in d['radar'].split(':', 1)[1].split(';'):
                    car_id, dx, dz, angle = item.split(':')
                    icons.append((int(car_id), bits_float(dx), bits_float(dz), int(angle)))
            if len(groups) != len(icons)+1:
                fail('panel cars', frame, len(groups)-1, [i[0] for i in icons])
                continue
            c0, s0 = unit(int(d['ra']))
            expected = [centre+k*np.array(v) for v in r['player_first']]
            shapes = [(groups[0], expected)]
            for (car_id, dx, dz, angle), group in zip(icons, groups[1:]):
                origin = centre+matrix@(zoom*np.array([dx*c0, dx*s0, dz*c0, dz*s0]))
                c, s = unit((angle-int(d['ra'])) & 0xffff)
                turn_matrix = c*np.eye(2)+r['turn']*s*turn
                shapes.append((group, [origin+k*turn_matrix@np.array(v) for v in r['opponent_first']]))
                radar['icons'] += 1
            for group, expected in shapes:
                error = max(float(np.abs(np.array(v)-e).max()) for v, e in zip(points(group), expected))
                radar['worst'] = max(radar['worst'], error)
                if error > args.tolerance:
                    fail('panel icon', frame, points(group), [e.tolist() for e in expected])
            polygons += len(shapes)
    print(f'HUD check: frames compared={compared} of {len(reference)} reference rows; overlay polygons checked={polygons}, '
          f'worst overlay error={worst:.4f} px (tolerance {args.tolerance})')
    if radar['frames']:
        print(f"  CONDITION panel (12c98..13208): {radar['frames']} frames, panel at its place on {radar['panel_frames']}, "
              f"{radar['icons']} opponent icons (and the player's on every frame), worst icon error={radar['worst']:.4f} px; "
              f"zoom compared bit for bit on {radar['zoom']} frames, the map's size on {radar['mapsize']}")
    if extensions[0]:
        print(f'  time extension message (ae98): compared on {extensions[0]} cells (rows 12 and 16 of a frame), drawn on {extensions[1]}')
    if gears[0]:
        print(f'  gear indicator (b0c0): compared on {gears[0]} frames, lever drawn on {gears[1]}, digit on {gears[2]}')
    if seconds:
        print(f'  the second car\'s marker (12b34, the player leading): compared on {seconds} frames')
    if lap_messages[0]:
        print(f'  lap message (ad34): compared on {lap_messages[0]} frames, LAP TIME drawn on {lap_messages[1]}, BEST LAP on {lap_messages[2]}')
    if overlay:
        print('  rolling start overlay (ab78, d438): '+', '.join(f'{kind} compared on {count} frames, drawn on {drawn[kind]}'
                                                                 for kind, count in overlay.items()))
    if messages[0]:
        print(f'  wrong-way message (a818): compared on {messages[0]} frames, drawn on {messages[1]}')
    if stale:
        print('  reference one tick stale (equals the native row of the previous frame): ' +
              ', '.join(f'{kind} {count} frames' for kind, count in stale.items()))
    for kind, count in failures.items():
        frame, expected, actual = first_failure[kind]
        print(f'  MISMATCH {kind}: {count} frames; first at frame {frame}: reference={expected} native={actual}')
    if args.preview_dir:
        args.preview_dir.mkdir()
        sheets = load_sheets(args.pack, fit)
        for path in sorted(args.display.parent.glob(args.display.name+'.quads-*')):
            frame = int(path.name.rsplit('-', 1)[1])
            source = args.trace/f'frame_{frame}.tga'
            if not source.exists():
                print(f'  preview {frame}: no reference snapshot'); continue
            original = np.array(Image.open(source).convert('RGB'))
            alone = np.zeros_like(original); alone[:] = (40, 40, 56)
            ink = draw_quads(path, sheets, alone)
            over = (original//3).astype(np.uint8)
            draw_quads(path, sheets, over)
            differs = ink & (np.abs(alone.astype(int)-original.astype(int)).max(axis=2) > 0)
            sheet = np.concatenate([original, alone, over], axis=1)
            Image.fromarray(sheet).resize((sheet.shape[1]*2, sheet.shape[0]*2), Image.NEAREST).save(args.preview_dir/f'hud-{frame}.png')
            marked = original.copy(); marked[differs] = (255, 0, 255)
            Image.fromarray(marked).save(args.preview_dir/f'hud-{frame}-differences.png')
            print(f'  preview {frame}: native HUD ink={int(ink.sum())} px, differing from the reference frame={int(differs.sum())} px '
                  f'-> {args.preview_dir}/hud-{frame}.png (reference | native HUD | native HUD over dimmed reference)')
    if failures or not compared:
        sys.exit(1)
    print('HUD check PASS')


if __name__ == '__main__':
    main()
