"""Export the in-race HUD art offline from verified daytona93 (course 0, automatic).

Usage: prepare_hud.py ROM_CACHE HUD_TRACE_DIR NEW_DIRECTORY [--first-bank N] [--course-map SLOT TRACE ...]
                      [--second-marker SLOT TRACE] [cost model: --line-ns --lines --build-ns --tile-ns --px-ns]

--course-map SLOT TRACE: the course map of another course slot (501460: 1 Expert, 2 Advanced) from a HUD trace
of a race on it: its flat map polygons become the sprite MAP_C<SLOT> and its markers' fits the matrices
nm_hud_map_matrix_c<SLOT> and nm_hud_map_leader_c<SLOT> (the markers' turn J is the HUD's and must agree with
course 0's); nm_hud_course_maps lists the maps by slot.

--second-marker SLOT TRACE: the map's "2" marker (288b5f0, on the second car while the player leads) from a HUD trace
of a race on a course of --course-map in which the player leads: the sprite MARKER_SECOND.

HUD_TRACE_DIR comes from `m2segment ... --trace-hud NEW_DIR EVERY` (host only): its
snapshots supply the final palette colours (colour translation and gamma applied
by the board, never guessed RGB555) and the overlay polygons (course map, map
markers, tachometer needle, the CONDITION panel and its car icons; its rows the
panel's projection, fitted against the cars' positions). Tile art itself is read from the ROM through the
shared 5454 loader lists and authored block headers, then checked against the
snapshot's character RAM.

Output (ROM-derived, keep under ignored build/ directories):
  hud_data.h           sprite table, layout and fitted overlay constants, and the tight
                       geometry of every sprite (tight_pieces below; include/render/ui/hud_draw.h)
  hud/sheet-N.dt       twiddled PAL4 atlas per palette bank
  hud/hud.dp           ARGB8888 palette, 16 entries per bank
  preview-sheet-N.png, fit.json (constants used by check_hud.py)
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

import numpy as np
from PIL import Image, ImageDraw

from inspect_course import IMAGES, MAIN_BASE
from prepare_material_crops import block_index
from prepare_scene import check_pal4

# Tile destinations in the original (layer 0, 64 words per row); see include/hud.h.
BLOCKS = {  # name: (authored block, destination, source)
    'TACH': (0x2878f1e, 0x10002a6),       # a6b4
    'LAP_LABEL': (0x28729ca, 0x100008e),  # a6c8
    'CONDITION_FRAME': (0x28790ec, 0x1000460),  # a72c
    'CONDITION_LABEL': (0x2872c26, 0x10009e2),  # a750 (53e59a bit0 set on this board)
    'POSITION_LABEL': (0x28729b0, 0x1000162),   # a77c
}
CELLS = {  # name: destination of the first cell
    'TIMER': 0x10000b8, 'CIRCLE_CURRENT': 0x100010c, 'CIRCLE_SLASH': 0x1000110, 'CIRCLE_TOTAL': 0x1000114,
    'RANK': 0x10001dc, 'SUFFIX': 0x10001e4, 'SLASH': 0x10001e8, 'COUNT': 0x10001ec,
    'SPEED': 0x10004b2, 'UNIT': 0x10004be,
}


# ae98 "TIME EXTENSION!!": block, and its two places (row 12 while the lap message 5010dc runs, else row 16).
EXTENSION = (0x2872c42, 0x1000626, 0x1000826)
# ab2c "PIT WORK!": destination and block.
PIT = (0x1000a28, 0x2876860)
# b0c0 gear indicator (manual only): the lever animation rows 23253a[old][new] (16 frames each) of the
# blocks 23263c[0..7] at 10004c4; after it the digit gear+1 (18698, colour 0x84) at 10004c6 between the brackets
# 2872a0a (10004c4) and 2872a1a (10004c8), the label 28729de at 10005c2. Program words checked in gear_sprites.
GEAR = dict(frames=0x23253a, blocks=0x23263c, lever=0x10004c4, digit=0x10004c6, close=0x10004c8, label=0x10005c2,
            open_block=0x2872a0a, close_block=0x2872a1a, label_block=0x28729de, colour=0x84)


def gear_frame(old, new, index):
    """include/hud.h nm_hud_gear_frame: the lever block of frame `index` (15 first) of row [old][new]."""
    if not index:
        return 7
    step = 15-index
    path = [old, 5+(old >> 1)] + ([4, 5+(new >> 1)] if old >> 1 != new >> 1 else [])
    return path[step] if old != new and step < len(path) else new


def gear_blocks(program):
    """Check b0c0 and its tables against the rule of include/hud.h; returns the seven lever blocks."""
    word = lambda a: struct.unpack_from('<I', program, a)[0]
    expected = {0xb0f8: GEAR['frames'], 0xb10c: GEAR['label'], 0xb114: GEAR['label_block'], 0xb134: GEAR['blocks'],
                0xb13c: GEAR['lever'], 0xb148: 0x8c180000 | GEAR['colour'], 0xb15c: GEAR['digit'], 0xb168: GEAR['label'],
                0xb170: GEAR['label_block'], 0xb17c: GEAR['lever'], 0xb184: GEAR['open_block'], 0xb190: GEAR['close'],
                0xb198: GEAR['close_block'], 0xb0e4: 0x5c181e1f}  # mov 31,r3: the animation's ticks
    for address, value in expected.items():
        if word(address) != value:
            raise ValueError(f'unexpected gear indicator b0c0 at {address:x}')
    for old in range(4):
        for new in range(4):
            row = program[GEAR['frames']-0x200000+old*64+new*16:][:16]
            if list(row) != [gear_frame(old, new, i) for i in range(16)]:
                raise ValueError(f'gear indicator row 23253a[{old}][{new}] does not follow the rule')
    blocks = struct.unpack_from('<8I', program, GEAR['blocks']-0x200000)
    # Block 7 (in the program image) is the blank 3x3 block that erases the lever: drawn as nothing.
    base, wide, height, width = struct.unpack_from('<hhII', program, blocks[7]-0x200000)
    cells = struct.unpack_from('<9h', program, blocks[7]-0x200000+12)
    if (wide, height, width) != (1, 3, 3) or any(((base+v) & 0xffff) != 0x8020 for v in cells):
        raise ValueError('unexpected blank lever block')
    return blocks[:7]


# ab78..ad30 and d438..d5e0, the rolling start's overlay (course 0, include/hud.h nm_hud_overlay): the hint blocks and
# their places, GO!, the digits 1, 2, 3 at one place, the banner and its car; (word of the program, value) of each.
ROLLING = dict(hint_accel=(0x28766c8, 0x10009a6), hint_auto=(0x287679c, 0x10009a8), go=(0x2882f14, 0x100072a),
               digits=(0x2882dac, 0x2882e24, 0x2882e9c), digit_at=0x1000836, banner=(0x28829f0, 0x1000b8e),
               car=(0x2882d1a, 0x1000c00))
ROLLING_WORDS = {0xabb0: 0x287679c, 0xaba8: 0x10009a8, 0xabc4: 0x28766c8, 0xabbc: 0x10009a6, 0xac50: 0x100072a,
                 0xac58: 0x2882f14, 0xac80: 0x1000836, 0xac88: 0x2882dac, 0xacbc: 0x1000836, 0xacc4: 0x2882e24,
                 0xacfc: 0x1000836, 0xad04: 0x2882e9c, 0xd44c: 0x1000b8e, 0xd454: 0x28829f0, 0xd460: 0x1000c00,
                 0xd468: 0x2882d1a, 0xd4b8: 0x10009a6, 0xd4c0: 0x28766c8, 0xd4f0: 0x10009a8, 0xd4f8: 0x287679c}
ROLLING_SPLIT = 28  # tile columns of the banner's first sprite: a sheet is 256 texels wide, the banner 456
# ad34, the lap message: BEST LAP or LAP TIME (blocks) over the time in the 2x3-tile font of 18414 in colour 5.
LAP_MESSAGE = dict(best=0x28829a0, lap=0x28829c8, label_at=0x10007b0, time_at=0x100082c, colour=5, glyphs='0123456789\'"')
LAP_MESSAGE_WORDS = {0xad7c: 0x100082c, 0xada0: 0x10007b0, 0xada8: 0x28829a0, 0xadc8: 0x10007b0, 0xadd0: 0x28829c8,
                     0xad8c: 0x5c181e05, 0xadb4: 0x5c181e05}  # the last two: mov 5,r3 before 501338


# CONDITION panel (12c98..13208): zoom table 234dd8 (include/hud.h nm_hud_radar_zooms), the icon of the
# automatic car 13218[0], the opponents' icon 288b550 (each car's +84 in the race).
RADAR_ZOOMS = (0x3c03126f, 0x3ba3d70a, 0x3b23d70a)
# 234de8[course*4 + index], the map's scale +38 (include/hud.h nm_hud_map_scales, 12a4c).
MAP_SCALES = ((0x339652e8, 0x3356bf95, 0x331652e8), (0x32e7eda1, 0x32bcfa83, 0x32920765), (0x332bcc77, 0x3300d959, 0x32abcc77))
RADAR_SCALE = 4  # sprite texels per arcade pixel of the radar icons (2 x 5 and 6 x 7 arcade pixels)


def radar_cars(row):
    """The opponents of a trace row (list 501224): id, position and drawn heading +58 + +d6."""
    cars = []
    for item in row.get('cars', '').split(';'):
        if item:
            car_id, x, z, yaw, d6 = item.split(',')
            cars.append(dict(id=int(car_id), x=bits_float(x), z=bits_float(z), angle=(int(yaw)+int(d6)) & 0xffff))
    return cars


def radar_inside(before, zoom):
    """Opponents inside the circle of radius 0.21/zoom around the player (12e84, 13130), in list order, from
    the car state of the row `before`; None when a car lies within 1e-3 of the circle (where the TGP's own
    divide and square root decide, include/hud.h)."""
    radius = 0.21/zoom
    x0, z0 = bits_float(before['x']), bits_float(before['z'])
    inside = []
    for car in radar_cars(before):
        distance = math.hypot(car['x']-x0, car['z']-z0)
        if abs(distance-radius) < 1e-3:
            return None
        if distance < radius:
            inside.append(car)
    return inside


def radar_polygons(path):
    """The CONDITION panel's polygons of a snapshot (window 1 at depth 1.5): the panel, then six an icon
    (full luma, within 12 pixels of the panel: the rule of tools/trace_hud.hpp)."""
    panel, icons = None, []
    for line in path.read_text().splitlines():
        if line.startswith('#'):
            continue
        p = line.split()
        vertices = [tuple(map(float, v.split(','))) for v in p[9:]]
        if int(p[7]) != 1 or not 1.45 < vertices[0][2] < 1.55:
            continue
        if int(p[2], 16) != 0x8000 and (int(p[6]) != 255 or not all(372 <= v[0] <= 475 and 55 <= v[1] <= 162 for v in vertices)):
            continue
        if int(p[2], 16) == 0x8000:
            panel = panel or [(v[0], v[1]) for v in vertices]
        elif panel and not int(p[2], 16):
            icons.append(dict(colour=int(p[5], 16), xy=[(v[0], v[1]) for v in vertices]))
    return panel, icons


def map_turn(program, slot):
    """12a10..12af0: the map model turns by 234d98[slot].angle; a marker's heading on another course's map by its
    difference from course 0's, as a 16-bit angle."""
    angle = lambda s: struct.unpack_from('<I', program, 0x34d98+16*s+12)[0]
    return (angle(slot)-angle(0)) & 0xffff


def c_float(value):
    """A C float literal (always with a point or an exponent)."""
    text = '%.9g' % value
    return text+('f' if any(c in text for c in '.en') else '.0f')


def cell(address):
    offset = address-0x1000000
    return ((offset >> 1) & 63)*8, (offset >> 7)*8


def load_rom(source):
    program, main = (source/'program.bin').read_bytes(), (source/'main_data.bin').read_bytes()
    for name, blob in (('program.bin', program), ('main_data.bin', main)):
        size, digest = IMAGES[name]
        if len(blob) != size or hashlib.sha256(blob).hexdigest() != digest:
            raise ValueError(f'unrecognized {name}')
    return program, main


def shared_characters(main):
    """Character lists of the shared 5454 loader (root 2800000), as prepare_result_labels."""
    word = lambda a: struct.unpack_from('<I', main, a-MAIN_BASE)[0]
    chars, present = bytearray(0x80000), bytearray(0x80000)
    address = word(word(0x2800000))
    for _ in range(128):
        source = word(address); address += 4
        if not source:
            return bytes(chars), bytes(present)
        destination = word(address); address += 4
        size, offset = word(source)*32, destination-0x1080000
        if offset < 0 or offset+size > len(chars):
            raise ValueError('character destination out of bounds')
        chars[offset:offset+size] = main[source+4-MAIN_BASE:source+4-MAIN_BASE+size]
        present[offset:offset+size] = bytes([1])*size
    raise ValueError('unterminated character list')


def block_tiles(main, address):
    base, wide, height, width = struct.unpack_from('<hhII', main, address-MAIN_BASE)
    if wide not in (0, 1) or not 1 <= width <= 64 or not 1 <= height <= 64:
        raise ValueError('invalid authored block header')
    values = struct.unpack_from('<'+('h' if wide else 'b')*(width*height), main, address-MAIN_BASE+12)
    return width, height, [(base+v) & 65535 for v in values]


def trace_rows(path):
    rows = []
    for line in path.read_text().splitlines():
        if not line.startswith('H '):
            continue
        rows.append(dict(field.split('=', 1) for field in line.split()[1:]))
    return rows


def bits_float(text):
    return struct.unpack('<f', struct.pack('<I', int(text, 16)))[0]


def points(text):
    return [tuple(map(float, p.split(','))) for p in text.split(';')]


def needle_angle(rpm):
    """afc0: 6a00 + int(min(rpm154, 8500) * 1.18), single precision, round to nearest even."""
    value = np.float32(min(rpm, 0x2134))*np.float32(1.18)
    return (0x6a00+int(np.rint(value))) & 0xffff


def unit(angle):
    radians = angle*(2*np.pi/65536)
    return np.cos(radians), np.sin(radians)


def inside(polygon, x, y):
    sign = 0
    for i in range(len(polygon)):
        (ax, ay), (bx, by) = polygon[i], polygon[(i+1) % len(polygon)]
        cross = (bx-ax)*(y-ay)-(by-ay)*(x-ax)
        if abs(cross) < 1e-9:
            continue
        if sign and (cross > 0) != (sign > 0):
            return False
        sign = 1 if cross > 0 else -1
    return True


def sample_colour(frame, polygon, later):
    """Colour of a flat polygon in a reference frame: interior pixels not covered by later polygons."""
    xs, ys = [p[0] for p in polygon], [p[1] for p in polygon]
    votes = {}
    for margin in (0.45, 0.25, 0.0):
        for y in range(int(min(ys)), int(max(ys))+1):
            for x in range(int(min(xs)), int(max(xs))+1):
                if not (0 <= x < frame.width and 0 <= y < frame.height):
                    continue
                probes = [(x+0.5+dx, y+0.5+dy) for dx in (-margin, margin) for dy in (-margin, margin)]
                if all(inside(polygon, *p) for p in probes) and not any(inside(q, x+0.5, y+0.5) for q in later):
                    colour = frame.getpixel((x, y))[:3]
                    votes[colour] = votes.get(colour, 0)+1
        if votes:
            break
    return max(votes, key=votes.get) if votes else None


# ---- tight geometry ---------------------------------------------------------
# On the PVR every pixel of a punch-through polygon is textured for the alpha test
# (28.4 ns each, measured), a transparent one for nothing, and each triangle is set up
# again in every tile it touches; a pixel of an opaque polygon in front costs next to
# nothing, and it keeps whatever lies behind it from being textured. But every rectangle
# is also SH4 work: TA records pushed through the store queues, which in the worst frames
# of the race is the binding limit. So each sprite is drawn either as its one quad or as
#   cores:  rectangles without a transparent texel, drawn on the opaque list in front, and
#   pieces: punch-through rectangles that together hold every opaque texel outside the
#           cores. A piece may run under a core: the hidden-surface pass removes it there,
# whichever costs less in the model below (CostModel); ties keep the one quad.

SX, SY = 640/496, 480/384   # output pixels per arcade pixel (one texel of a sprite of scale 1)


class CostModel:
    """Nanoseconds per frame that one rectangle of w x h texels costs, `textured` of them
    textured for the alpha test:
      SH4: lines * line_ns + build_ns  (TA records of 32 bytes through the store queues, and
           the work of building them)
      PVR: 2 triangles * tiles * tile_ns + textured * SX*SY * px_ns
    tiles: the tiles of 32x32 pixels a rectangle touches on average over its placements."""
    def __init__(self, line_ns, lines, build_ns, tile_ns, px_ns):
        self.line_ns, self.lines, self.build_ns, self.tile_ns, self.px_ns = line_ns, lines, build_ns, tile_ns, px_ns
        self.rectangle = lines*line_ns+build_ns

    def __call__(self, w, h, textured):
        tiles = (1+np.asarray(w)*SX/32)*(1+np.asarray(h)*SY/32)
        return self.rectangle+2*tiles*self.tile_ns+np.asarray(textured)*SX*SY*self.px_ns

    def describe(self):
        return (f'{self.lines} TA lines of {self.line_ns:g} ns + {self.build_ns:g} ns SH4 a rectangle, '
                f'{self.tile_ns:g} ns a triangle and tile, {self.px_ns:g} ns a textured pixel')


def largest_rectangle(free):
    """(area, x, y, width, height) of the largest rectangle of true cells."""
    height, width = free.shape
    best, columns = (0, 0, 0, 0, 0), [0]*width
    for y in range(height):
        stack = []
        for x in range(width+1):
            if x < width:
                columns[x] = columns[x]+1 if free[y, x] else 0
            level, start = (columns[x] if x < width else 0), x
            while stack and stack[-1][1] >= level:
                start, top = stack.pop()
                if top*(x-start) > best[0]:
                    best = (top*(x-start), start, y-top+1, x-start, top)
            stack.append((start, level))
    return best


def run_rectangles(mask):
    """The true cells as rectangles: runs of a row, joined downwards while they repeat."""
    height, width = mask.shape
    rectangles, live = [], {}
    for y in range(height+1):
        runs = set()
        x = 0
        while y < height and x < width:
            if mask[y, x]:
                first = x
                while x < width and mask[y, x]:
                    x += 1
                runs.add((first, x))
            else:
                x += 1
        for run in list(live):
            if run not in runs:
                rectangles.append((run[0], live.pop(run), run[1], y))
        for run in runs:
            live.setdefault(run, y)
    return sorted(rectangles)


def table(cells):
    height, width = cells.shape
    sums = np.zeros((height+1, width+1), dtype=np.int64)
    sums[1:, 1:] = np.cumsum(np.cumsum(cells, axis=0), axis=1)
    return sums


def box(sums, x0, y0, x1, y1):
    return sums[y1, x1]-sums[y0, x1]-sums[y1, x0]+sums[y0, x0]


def merge_pieces(rest, price, cost):
    """Runs of the remaining texels, joined greedily two at a time while a join costs less than
    the two pieces (and their bounding box holds no third piece: pieces stay disjoint)."""
    height, width = rest.shape
    p = table(price)
    r = np.array(run_rectangles(rest), dtype=np.int64).reshape(-1, 4)
    while len(r) > 1:
        taken = np.zeros((height, width), dtype=np.int64)
        for x0, y0, x1, y1 in r:
            taken[y0:y1, x0:x1] = 1
        taken = table(taken)
        area = (r[:, 2]-r[:, 0])*(r[:, 3]-r[:, 1])
        own = cost(r[:, 2]-r[:, 0], r[:, 3]-r[:, 1], box(p, r[:, 0], r[:, 1], r[:, 2], r[:, 3]))
        x0, y0 = np.minimum.outer(r[:, 0], r[:, 0]), np.minimum.outer(r[:, 1], r[:, 1])
        x1, y1 = np.maximum.outer(r[:, 2], r[:, 2]), np.maximum.outer(r[:, 3], r[:, 3])
        alone = box(taken, x0, y0, x1, y1) == np.add.outer(area, area)  # no third piece inside
        saved = np.where(alone, np.add.outer(own, own)-cost(x1-x0, y1-y0, box(p, x0, y0, x1, y1)), -1)
        np.fill_diagonal(saved, -1)
        i, j = divmod(int(np.argmax(saved)), len(r))
        if saved[i, j] <= 0:
            break
        r = np.vstack([np.delete(r, [i, j], axis=0), np.array([[x0[i, j], y0[i, j], x1[i, j], y1[i, j]]])])
    return [(int(a), int(b), int(c-a), int(d-b)) for a, b, c, d in r]


def split_pieces(rest, price, cost):
    """Guillotine cuts: the bounding box of the remaining texels as one piece, or cut in two
    at the line that leaves the cheapest two bounding boxes, recursively, whichever costs less."""
    p = table(price)

    def bounds(m, x0, y0):
        cols, rows = np.flatnonzero(m.any(axis=0)), np.flatnonzero(m.any(axis=1))
        return (x0+int(cols[0]), y0+int(rows[0]), x0+int(cols[-1])+1, y0+int(rows[-1])+1) if len(cols) else None

    def single(b):
        return float(cost(b[2]-b[0], b[3]-b[1], box(p, *b)))

    def best_cut(b):
        """The cut of box b whose two bounding boxes cost least: (cost, box, box) or None."""
        x0, y0, x1, y1 = b
        m = rest[y0:y1, x0:x1]
        best = None
        for axis in (0, 1):
            mm = m if axis == 0 else m.T            # cut between columns of mm
            n, length = mm.shape[1], mm.shape[0]
            if n < 2:
                continue
            any_ = mm.any(axis=0)
            top = np.where(any_, mm.argmax(axis=0), length)
            bottom = np.where(any_, length-mm[::-1].argmax(axis=0), 0)
            idx = np.arange(n)
            first_l = np.minimum.accumulate(np.where(any_, idx, n))      # leftmost column so far
            last_l = np.maximum.accumulate(np.where(any_, idx, -1))
            top_l, bottom_l = np.minimum.accumulate(top), np.maximum.accumulate(bottom)
            r = slice(None, None, -1)
            first_r = np.minimum.accumulate(np.where(any_, idx, n)[r])[r]
            last_r = np.maximum.accumulate(np.where(any_, idx, -1)[r])[r]
            top_r, bottom_r = np.minimum.accumulate(top[r])[r], np.maximum.accumulate(bottom[r])[r]
            for k in range(1, n):   # left: columns < k, right: columns >= k
                if last_l[k-1] < 0 or first_r[k] >= n:
                    continue
                left = (first_l[k-1], top_l[k-1], last_l[k-1]+1, bottom_l[k-1])
                right = (first_r[k], top_r[k], last_r[k]+1, bottom_r[k])
                if axis == 0:
                    lb = (x0+left[0], y0+left[1], x0+left[2], y0+left[3])
                    rb = (x0+right[0], y0+right[1], x0+right[2], y0+right[3])
                else:
                    lb = (x0+left[1], y0+left[0], x0+left[3], y0+left[2])
                    rb = (x0+right[1], y0+right[0], x0+right[3], y0+right[2])
                lb, rb = tuple(int(v) for v in lb), tuple(int(v) for v in rb)
                c = single(lb)+single(rb)
                if best is None or c < best[0]:
                    best = (c, lb, rb)
        return best

    def solve(b):
        whole = single(b)
        cut = best_cut(b)
        if cut is None or cut[0] >= whole+slack:   # greedy, with a little look-ahead
            return whole, [b]
        cl, pl = solve(cut[1])
        cr, pr = solve(cut[2])
        return (cl+cr, pl+pr) if cl+cr < whole else (whole, [b])

    slack = 0.5*cost.rectangle

    b = bounds(rest, 0, 0)
    if b is None:
        return []
    return [(x0, y0, x1-x0, y1-y0) for x0, y0, x1, y1 in solve(b)[1]]


def cover(mask, cost, core_minimum, method):
    """Cores of at least core_minimum texels, largest first; then the rest in pieces."""
    rest, cores = mask.copy(), []
    while True:
        area, x, y, w, h = largest_rectangle(rest)
        if not area or area < core_minimum:
            break
        cores.append((x, y, w, h)); rest[y:y+h, x:x+w] = False
    price = np.ones(mask.shape, dtype=np.int64)  # what a piece textures: everything no core hides
    for x, y, w, h in cores:
        price[y:y+h, x:x+w] = 0
    pieces = method(rest, price, cost) if rest.any() else []
    p = table(price)
    textured = int(sum(box(p, x, y, x+w, y+h) for x, y, w, h in pieces))
    total = sum(float(cost(w, h, 0)) for x, y, w, h in cores)+sum(float(cost(w, h, box(p, x, y, x+w, y+h))) for x, y, w, h in pieces)
    return total, cores, pieces, textured


def tight_pieces(mask, cost, quick=False):
    """The cheapest form in the cost model: the one quad (a core when the sprite has no
    transparent texel), or cores and pieces for one of a range of core sizes and either way
    of cutting the pieces. quick: two core sizes, cut only: merge_pieces is cubic in the runs of
    a sprite and largest_rectangle goes texel by texel in Python, which large art cannot afford.
    Returns cost, cores, pieces, textured texels."""
    height, width = mask.shape
    whole = (float(cost(width, height, 0)), [(0, 0, width, height)], [], 0) if mask.all() else \
        (float(cost(width, height, width*height)), [], [(0, 0, width, height)], width*height)
    best = whole
    area = int(mask.sum())
    for core_minimum in (64, 256, 1 << 30) if quick else sorted({8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512} | {1 << 30}):
        if core_minimum > area and core_minimum != 1 << 30:
            continue
        for method in (split_pieces,) if quick else (merge_pieces, split_pieces):
            candidate = cover(mask, cost, core_minimum, method)
            if candidate[0] < best[0]-1e-6:
                best = candidate
    return best


def check_tight(mask, cores, pieces):
    height, width = mask.shape
    held = np.zeros_like(mask)
    for index, (x, y, w, h) in enumerate(cores+pieces):
        if w < 1 or h < 1 or x < 0 or y < 0 or x+w > width or y+h > height or max(x, y, w, h) > 255:
            raise ValueError('tight rectangle outside its sprite')
        if index < len(cores) and not mask[y:y+h, x:x+w].all():
            raise ValueError('opaque core holds a transparent texel')
        held[y:y+h, x:x+w] = True
    if (mask & ~held).any():
        raise ValueError('tight geometry misses an opaque texel')



def course_map(trace, slot, j_reference, p_reference):
    """The course map of a HUD trace of another course (--course-map): its flat polygons, the colour they are
    drawn in, and the player's and the leader's marker matrices (screen = M (x, z, 1)), fitted as course 0's in
    main() from the previous tick's cars. The marker shapes and their turn are the HUD's; the turn must agree."""
    from collections import Counter
    rows = trace_rows(trace/'hud.trace')
    by_frame = {int(r['frame']): r for r in rows}
    previous = {int(r['frame']): rows[i-1] for i, r in enumerate(rows) if i}
    snapshots = sorted(int(p.stem.split('_')[1]) for p in trace.glob('tiles_*.bin'))
    counts = Counter(by_frame[f].get('overlay') for f in snapshots if by_frame[f]['active'] == '22')
    if not counts:
        raise ValueError(f'course {slot}: the HUD trace has no race snapshot')
    overlay_count = int(counts.most_common(1)[0][0])
    map_count = overlay_count-19   # then the leader's 6 and the player's 13 marker polygons, as on course 0
    steady = [f for f in snapshots if by_frame[f]['active'] == '22' and by_frame[f].get('overlay') == str(overlay_count)
              and f >= int(rows[0]['frame'])+700]
    if len(steady) < 8 or map_count < 1:
        raise ValueError(f'course {slot}: need at least eight steady race snapshots with the map')

    def polygons(frame):
        result = []
        for line in (trace/f'polys_{frame}.txt').read_text().splitlines():
            if line.startswith('#'):
                continue
            p = line.split()
            vertices = [tuple(map(float, v.split(','))) for v in p[9:]]
            result.append(dict(z=int(p[1]), tex=[int(t, 16) for t in p[2:6]], xy=[(v[0], v[1]) for v in vertices], pz=vertices[0][2]))
        return result
    overlay = {f: [p for p in polygons(f) if not p['tex'][0] and p['z'] == 0 and p['pz'] < 1e-3] for f in steady}
    if any(len(v) != overlay_count for v in overlay.values()):
        raise ValueError(f'course {slot}: the overlay polygon count changes')
    track = [p['xy'] for p in overlay[steady[0]][:map_count]]
    for f in steady:
        for a, b in zip(track, overlay[f][:map_count]):
            if max(abs(u-v) for pa, pb in zip(a, b['xy']) for u, v in zip(pa, pb)) > 1e-3:
                raise ValueError(f'course {slot}: the course map moves between steady snapshots')
    fit_rows = [r for r in rows[1:] if r['active'] == '22' and r.get('overlay') == str(overlay_count)][::7]

    def fit(keys, key, selected):
        a_rows, values = [], []
        for r in selected:
            before = previous[int(r['frame'])]
            x, z = bits_float(before[keys[0]]), bits_float(before[keys[1]])
            c, s = unit(int(before[keys[2]]))
            for k, (px, py) in enumerate(points(r[key])):
                for axis, value in enumerate((px, py)):
                    row = np.zeros(6+16)
                    row[axis*3:axis*3+3] = (x, z, 1)
                    row[6+k*4+axis*2] = c
                    row[6+k*4+axis*2+1] = s
                    a_rows.append(row); values.append(value)
        system, values = np.array(a_rows), np.array(values)
        solution, *_ = np.linalg.lstsq(system, values, rcond=None)
        residual = float(np.abs(system@solution-values).max())
        p_first = solution[6:].reshape(4, 2, 2)[:, :, 0]
        q_first = solution[6:].reshape(4, 2, 2)[:, :, 1]
        turn = float(np.abs(p_first@j_reference.T-q_first).max())
        return solution[:6].reshape(2, 3), residual, turn, p_first
    player = [r for r in fit_rows if 'player' in r and len(points(r['player'])) == 4]
    affine, residual, turn, p_first = fit(('x', 'z', 'yaw'), 'player', player)
    # The marker on this map against course 0's: the same shape turned by the map and scaled by its depth.
    centred = p_reference-p_reference.mean(axis=0)
    a = np.vstack([np.column_stack([centred[:, 0], (centred@j_reference.T)[:, 0]]),
                   np.column_stack([centred[:, 1], (centred@j_reference.T)[:, 1]])])
    (cosine, sine), *_ = np.linalg.lstsq(a, (p_first-p_first.mean(axis=0)).T.flatten(), rcond=None)
    size = float(np.hypot(cosine, sine))
    print(f'course {slot} marker: size {size:.5f} of course 0\'s, turned by {np.degrees(np.arctan2(sine, cosine)) % 360:.2f} degrees')
    print(f'course {slot} map marker fit: residual={residual:.4f} px over {len(player)} rows, turn residual={turn:.4f}, '
          f'affine={affine.round(6).tolist()}')
    if residual > 0.05 or turn > 0.05:
        raise ValueError(f'course {slot}: the player marker does not follow the affine map rule with the HUD turn')
    leader = [r for r in fit_rows if previous[int(r['frame'])].get('leader_id', '255') != '255'
              and previous[int(r['frame'])]['rank'] != '0' and 'leader' in r and len(points(r['leader'])) == 4]
    affine_l, residual_l, turn_l, _ = fit(('lx', 'lz', 'lyaw'), 'leader', leader)
    print(f'course {slot} leader marker fit: residual={residual_l:.4f} px over {len(leader)} rows, turn residual={turn_l:.4f}')
    if residual_l > 0.05 or turn_l > 0.05:
        raise ValueError(f'course {slot}: the leader marker does not follow the affine map rule with the HUD turn')
    frame = Image.open(trace/f'frame_{steady[0]}.tga').convert('RGB')
    votes = {}
    for i, polygon in enumerate(track):
        colour = sample_colour(frame, polygon, [p['xy'] for p in overlay[steady[0]][i+1:]])
        if colour:
            votes[colour] = votes.get(colour, 0)+1
    if not votes:
        raise ValueError(f'course {slot}: no map polygon visible')
    colour = max(votes, key=votes.get)
    print(f'course {slot} map: {map_count} polygons, colour #{colour[0]:02x}{colour[1]:02x}{colour[2]:02x} '
          f'({votes[colour]} of {sum(votes.values())} sampled polygons)')
    return dict(track=track, colour=colour, affine=affine, affine_leader=affine_l, residual=residual, residual_leader=residual_l,
                polygons=map_count, steady=steady, size=size, turn_degrees=float(np.degrees(np.arctan2(sine, cosine)) % 360))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('trace', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--first-bank', type=int, default=62)  # banks 62..63 are free in a race
    parser.add_argument('--max-banks', type=int, default=2)
    parser.add_argument('--course-map', nargs=2, action='append', metavar=('SLOT', 'TRACE'), default=[],
                        help='the course map of another course slot from a HUD trace of a race on it (module text)')
    # The cost model of the tight geometry (CostModel). line-ns: one 32-byte TA record in the
    # worst frames of the console run of 14 October (SH4 registration 15.8 -> 16.4 ms for
    # 236 -> 820 HUD vertices); a rectangle is one sprite of two such records
    # (code/render/ui/hud_render.c). build-ns: building it, about 90 SH4 instructions a
    # rectangle with its share of the per-sprite work (records_build, counted in the SH
    # simulator over both scripted races), at one instruction a cycle of 200 MHz.
    # tile-ns, px-ns: the PVR work model (tests/stream_sim, SIM_PVR).
    parser.add_argument('--second-marker', nargs=2, metavar=('SLOT', 'TRACE'),
                        help='the map marker of the second car from a HUD trace of a race the player leads')
    parser.add_argument('--line-ns', type=float, default=1030.0)
    parser.add_argument('--lines', type=int, default=2, help='TA records of 32 bytes a rectangle (2: sprite, 4: strip)')
    parser.add_argument('--build-ns', type=float, default=450.0)
    parser.add_argument('--tile-ns', type=float, default=216.0)
    parser.add_argument('--px-ns', type=float, default=28.4)
    args = parser.parse_args()
    cost = CostModel(args.line_ns, args.lines, args.build_ns, args.tile_ns, args.px_ns)
    program, main_data = load_rom(args.source)
    chars, present = shared_characters(main_data)
    rows = trace_rows(args.trace/'hud.trace')
    by_frame = {int(r['frame']): r for r in rows}
    snapshots = sorted(int(p.stem.split('_')[1]) for p in args.trace.glob('tiles_*.bin'))
    # Steady race snapshots only: active phase 22, the rolling start and its banner over
    # (the timer has started), every overlay polygon present.
    steady = [f for f in snapshots if by_frame[f]['active'] == '22' and by_frame[f].get('overlay') == '43'
              and int(by_frame[f]['remaining']) < 3200 and int(by_frame[f]['frame']) >= int(rows[0]['frame'])+700]
    if len(steady) < 8:
        raise ValueError('need at least eight steady race snapshots')
    pens = [struct.unpack('<8192I', (args.trace/f'pens_{f}.bin').read_bytes()) for f in steady]
    snapshot_chars = (args.trace/f'chars_{steady[0]}.bin').read_bytes()

    # ---- tile sprites -------------------------------------------------------
    sprites = {}   # name -> dict(width,height,pixels RGBA ints (0 transparent), scale)
    used_tiles = set()

    def tile_sprite(name, width, height, tiles):
        pixels = []
        for y in range(height*8):
            for x in range(width*8):
                tile = tiles[(y//8)*width+x//8]
                index = tile & 0x3fff
                if not all(present[index*32:index*32+32]):
                    raise ValueError(f'{name}: tile {tile:04x} not supplied by the shared loader')
                if chars[index*32:index*32+32] != snapshot_chars[index*32:index*32+32]:
                    raise ValueError(f'{name}: tile {tile:04x} differs from the running game')
                used_tiles.add(tile)
                packed = chars[(index*32+(y % 8)*4+(x % 8)//2) ^ 1]
                pen = (packed & 15) if x & 1 else packed >> 4
                colour = ((tile >> 7) & 255)*16+pen
                values = {p[colour] for p in pens}
                if pen and len(values) != 1:
                    raise ValueError(f'{name}: palette entry {colour} changes during the race')
                pixels.append((pens[0][colour] | 0xff000000) if pen else 0)
        sprites[name] = dict(width=width*8, height=height*8, pixels=pixels, scale=1)

    layout = {}
    blocks = dict(BLOCKS)
    # a818 wrong way: 236830[53e59a & 1] = (cell x, row, block); unit 1 on this board (as a750's label):
    # "逆走" 2876914 at (25, 21); unit 0 would be "REVERSE" 28769e0 at (21, 21).
    x, y, address = struct.unpack_from('<hhI', program, 0x36830+8*1)
    if (x, y, address) != (25, 21, 0x2876914):
        raise ValueError('unexpected wrong-way message table 236830')
    blocks['REVERSE'] = (address, 0x1000000+2*x+128*y)
    # ae98: lda 2872c42,g0 (ae98), lda 1000826,g1 (aeac) and lda 1000626,g1 (aeb8): the block and its places.
    program_word = lambda a: struct.unpack_from('<I', program, a)[0]
    if (program_word(0xae9c), program_word(0xaebc), program_word(0xaeb0)) != EXTENSION:
        raise ValueError('unexpected time extension message ae98')
    blocks['EXTENSION'] = EXTENSION[:2]
    # ab2c "PIT WORK!" (a7c0 while the pit's stop count 501a84 > 0): lda 1000a28,g1 / lda 2876860,g0 at ab48 and
    # ab60, cell (20, 20).
    if (program_word(0xab4c), program_word(0xab54), program_word(0xab64), program_word(0xab6c)) != PIT*2:
        raise ValueError('unexpected pit message ab2c')
    blocks['PIT'] = (PIT[1], PIT[0])
    layout['EXTENSION_LOW'] = cell(EXTENSION[2])
    for name, (address, destination) in blocks.items():
        width, height, tiles = block_tiles(main_data, address)
        tile_sprite(name, width, height, tiles)
        layout[name] = cell(destination)
        if name == 'REVERSE':  # check_hud.py tells the message from other art in its row 25 by these words
            reverse_row = tiles[(25-y)*width:(26-y)*width]
        if name == 'EXTENSION':  # and the time extension message in rows 12 and 16 by its first row
            extension_row = tiles[:width]
    for name, destination in CELLS.items():
        layout[name] = cell(destination)
    # Families are contiguous: sprite = family base + digit.
    for digit in range(10):
        tile_sprite(f'TIMER_{digit}', 3, 4, [0x8500+12*digit+i for i in range(12)])     # 18b44
    for digit in range(10):
        tile_sprite(f'BIG_{digit}', 2, 4, [0x8580+8*digit+i for i in range(8)])         # 18a08
    for glyph, name in ((10, 'SLASH'), (11, 'ST'), (12, 'ND'), (13, 'RD'), (14, 'TH')):
        tile_sprite(f'BIG_{name}', 2, 4, [0x8580+8*glyph+i for i in range(8)])           # 189b4
    for digit in range(10):
        tile_sprite(f'SPEED_{digit}', 2, 2, [0x8120+4*digit+i for i in range(4)])       # 1891c
    tile_sprite('SPEED_UNIT', 2, 2, [0x8120+4*10+i for i in range(4)])                   # 188e8(10)
    for digit in range(10):
        tile_sprite(f'LAPNUM_{digit}', 1, 2, [0x8000+(2 << 7)+2*digit+i for i in range(2)])  # 187c0, colour 2
    for glyph in range(12):  # 18698: digits, 10 = minute mark, 11 = second mark; colour 13
        tile_sprite(f'TIME_{glyph}', 1, 2, [0x8000+(13 << 7)+2*glyph+i for i in range(2)])
    for glyph in range(12):  # colour 14: the best lap
        tile_sprite(f'BEST_{glyph}', 1, 2, [0x8000+(14 << 7)+2*glyph+i for i in range(2)])
    for glyph in (*range(1, 9), 11):  # 18660: lap digits in circles, 11 = slash
        tile_sprite(f'CIRCLE_{glyph}', 2, 2, [0x8780+4*glyph+i for i in range(4)])
    # b0c0: the gear indicator (sprites GEAR_*; not on screen in this automatic trace, checked against the ROM).
    gear_words = dict(lever=[])  # tile words for check_hud.py (fit.json)
    for frame, address in enumerate(gear_blocks(program)):
        width, height, tiles = block_tiles(main_data, address)
        tile_sprite(f'GEAR_LEVER_{frame}', width, height, tiles)
        gear_words['lever'].append(tiles)
    for name, key in (('GEAR_OPEN', 'open_block'), ('GEAR_CLOSE', 'close_block'), ('GEAR_LABEL', 'label_block')):
        width, height, tiles = block_tiles(main_data, GEAR[key])
        tile_sprite(name, width, height, tiles)
        gear_words[key.split('_')[0]] = tiles
    for digit in range(1, 5):  # 18698: 50133c (0x8000) + (501334 << 7) + 2*digit
        tile_sprite(f'GEAR_DIGIT_{digit}', 1, 2, [0x8000+(GEAR['colour'] << 7)+2*digit+i for i in range(2)])
    gear_words['digits'] = [0x8000+(GEAR['colour'] << 7)+2*digit for digit in range(1, 5)]
    layout.update(GEAR_LEVER=cell(GEAR['lever']), GEAR_DIGIT=cell(GEAR['digit']), GEAR_CLOSE=cell(GEAR['close']),
                  GEAR_LABEL=cell(GEAR['label']))
    suffix = list(program[0x31efc:0x31efc+10])  # 231efc: st/nd/rd/th glyph by last digit
    if suffix != [14, 11, 12, 13, 14, 14, 14, 14, 14, 14]:
        raise ValueError('unexpected rank suffix table')

    # The tile rules above must reproduce the running game's layer 0 for a steady frame.
    tiles0 = (args.trace/f'tiles_{steady[0]}.bin').read_bytes()
    word = lambda col, row: struct.unpack_from('<H', tiles0, 2*(row*64+col))[0]
    seen = {word(c, r) for r in range(48) for c in range(62)}-{0x8020, 0x8280, 0x0020}
    seen = {t for t in seen if any(snapshot_chars[(t & 0x3fff)*32:(t & 0x3fff)*32+32])}
    if not seen <= used_tiles:
        raise ValueError('layer 0 shows tiles outside the exported sets: '+' '.join(f'{t:04x}' for t in sorted(seen-used_tiles)))

    # ---- overlay polygons ---------------------------------------------------
    def polygons(frame):
        result = []
        for line in (args.trace/f'polys_{frame}.txt').read_text().splitlines():
            if line.startswith('#'):
                continue
            p = line.split()
            vertices = [tuple(map(float, v.split(','))) for v in p[9:]]
            result.append(dict(z=int(p[1]), tex=[int(t, 16) for t in p[2:6]],
                               xy=[(v[0], v[1]) for v in vertices], pz=vertices[0][2]))
        return result
    frames = {f: Image.open(args.trace/f'frame_{f}.tga').convert('RGB') for f in steady}
    overlay = {f: [p for p in polygons(f) if not p['tex'][0] and p['z'] == 0 and p['pz'] < 1e-3] for f in steady}
    if any(len(v) != 43 for v in overlay.values()):
        raise ValueError('expected 24 map, 6 leader and 13 player polygons')
    track = [p['xy'] for p in overlay[steady[0]][:24]]
    for f in steady:
        for a, b in zip(track, overlay[f][:24]):
            if max(abs(u-v) for pa, pb in zip(a, b['xy']) for u, v in zip(pa, pb)) > 1e-3:
                raise ValueError('course map moves between steady snapshots')

    # Player marker: v = A*(x,z) + b + P cos(yaw) + Q sin(yaw) for its first polygon (12bf0..12c70).
    # The map object runs before the cars in the task table: it draws the positions, headings
    # and ranks left by the PREVIOUS tick (fit residual 0.7 px without the lag, 0.003 px with it).
    previous = {int(r['frame']): rows[i-1] for i, r in enumerate(rows) if i}
    fit_rows = [r for r in rows[1:] if r['active'] == '22' and 'player' in r and r.get('overlay') == '43'][::7]

    def marker_system(prefix_xyz, key, selected):
        a_rows, values = [], []
        for r in selected:
            before = previous[int(r['frame'])]
            x, z = bits_float(before[prefix_xyz[0]]), bits_float(before[prefix_xyz[1]])
            c, s = unit(int(before[prefix_xyz[2]]))
            for k, (px, py) in enumerate(points(r[key])):
                for axis, value in enumerate((px, py)):
                    row = np.zeros(6+16)
                    row[axis*3:axis*3+3] = (x, z, 1)
                    row[6+k*4+axis*2] = c
                    row[6+k*4+axis*2+1] = s
                    a_rows.append(row); values.append(value)
        return np.array(a_rows), np.array(values)
    system, values = marker_system(('x', 'z', 'yaw'), 'player', [r for r in fit_rows if len(points(r['player'])) == 4])
    solution, *_ = np.linalg.lstsq(system, values, rcond=None)
    residual = float(np.abs(system@solution-values).max())
    affine = solution[:6].reshape(2, 3)   # screen = affine @ (x, z, 1)
    # Rotation: Q = J P for every vertex of the marker (rigid model through a fixed projection).
    p_first = solution[6:].reshape(4, 2, 2)[:, :, 0]
    q_first = solution[6:].reshape(4, 2, 2)[:, :, 1]
    centre = p_first.mean(axis=0)
    j_matrix, *_ = np.linalg.lstsq(p_first-centre, q_first-q_first.mean(axis=0), rcond=None)
    j_matrix = j_matrix.T
    # The marker origin is where P and Q agree with pure rotation about the car position: fold
    # the constant part into the affine so that offsets rotate about the projected car position.
    print(f'map marker fit: residual={residual:.4f} px affine={affine.round(6).tolist()} J={j_matrix.round(5).tolist()}')
    if residual > 0.05:
        raise ValueError('player marker does not follow the affine map rule')
    j_residual = float(np.abs((p_first)@j_matrix.T-q_first).max())
    print(f'marker rotation: Q=J*P residual={j_residual:.4f} px')
    if j_residual > 0.05:
        raise ValueError('marker rotation is not a fixed linear map')
    # Leader marker (12b34..12bd0): drawn at height 0 where the player's is at height 1, so the
    # perspective gives it its own, slightly different, affine. Same rotation rule.
    leader_rows = [r for r in fit_rows if previous[int(r['frame'])].get('leader_id', '255') != '255'
                   and previous[int(r['frame'])]['rank'] != '0' and 'leader' in r and len(points(r['leader'])) == 4]
    system_l, values_l = marker_system(('lx', 'lz', 'lyaw'), 'leader', leader_rows)
    solution_l, *_ = np.linalg.lstsq(system_l, values_l, rcond=None)
    residual_l = float(np.abs(system_l@solution_l-values_l).max())
    affine_l = solution_l[:6].reshape(2, 3)
    turn_l = float(np.abs(solution_l[6:].reshape(4, 2, 2)[:, :, 0]@j_matrix.T-solution_l[6:].reshape(4, 2, 2)[:, :, 1]).max())
    print(f'leader marker fit: residual={residual_l:.4f} px rows={len(leader_rows)} affine={affine_l.round(6).tolist()} turn residual={turn_l:.4f}')
    if residual_l > 0.05 or turn_l > 0.05:
        raise ValueError('leader marker does not follow the affine map rule')

    def rotation(angle):
        c, s = unit(angle)
        return c*np.eye(2)+s*j_matrix

    def marker_local(first, count, keys):
        """Upright (yaw 0) vertex offsets of every polygon of a marker, averaged over snapshots."""
        local, worst = None, 0.0
        samples = []
        for f in steady:
            r = previous[f]
            if keys[0] == 'lx' and r.get('leader_id', '255') == '255':
                continue
            origin = (affine_l if keys[0] == 'lx' else affine)@np.array([bits_float(r[keys[0]]), bits_float(r[keys[1]]), 1.0])
            inverse = np.linalg.inv(rotation(int(r[keys[2]])))
            polys = overlay[f][first:first+count]
            samples.append([[inverse@(np.array(v)-origin) for v in p['xy']] for p in polys])
        shapes = {tuple(len(p) for p in s) for s in samples}
        if len(shapes) != 1:
            raise ValueError('marker polygon shapes change')
        local = [[np.mean([s[i][k] for s in samples], axis=0) for k in range(len(samples[0][i]))] for i in range(count)]
        for s in samples:
            for i in range(count):
                for k in range(len(local[i])):
                    worst = max(worst, float(np.abs(s[i][k]-local[i][k]).max()))
        return local, worst

    def marker_sprite(name, first, count, keys, scale=2):
        local, worst = marker_local(first, count, keys)
        print(f'{name}: {count} polygons, upright geometry spread={worst:.4f} px')
        if worst > 0.08:
            raise ValueError(f'{name} is not rigid under the fitted rotation')
        colours = []
        for i in range(count):
            votes = {}
            for f in steady:
                r = previous[f]
                if keys[0] == 'lx' and r.get('leader_id', '255') == '255':
                    continue
                group = overlay[f]
                later = [p['xy'] for p in group[first+i+1:]]
                colour = sample_colour(frames[f], group[first+i]['xy'], later)
                if colour:
                    votes[colour] = votes.get(colour, 0)+1
            if not votes:
                raise ValueError(f'{name}: polygon {i} never visible')
            colours.append(max(votes, key=votes.get))
        marker_raster(name, local, colours, scale)
        return local, colours

    def marker_raster(name, local, colours, scale=2):
        xs = [v[0] for p in local for v in p]; ys = [v[1] for p in local for v in p]
        left, top = int(np.floor(min(xs)))-1, int(np.floor(min(ys)))-1
        width, height = int(np.ceil(max(xs)))+1-left, int(np.ceil(max(ys)))+1-top
        image = Image.new('RGBA', (width*scale, height*scale), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        for polygon, colour in zip(local, colours):
            draw.polygon([((v[0]-left)*scale-0.5, (v[1]-top)*scale-0.5) for v in polygon], fill=colour+(255,))
        pixels = [(0xff000000 | r << 16 | g << 8 | b) if a else 0 for r, g, b, a in image.get_flattened_data()]
        sprites[name] = dict(width=width*scale, height=height*scale, pixels=pixels, scale=scale, origin=(left, top))
    leader_local, _ = marker_sprite('MARKER_LEADER', 24, 6, ('lx', 'lz', 'lyaw'))
    player_local, _ = marker_sprite('MARKER_PLAYER', 30, 13, ('x', 'z', 'yaw'))

    # ---- CONDITION panel (12c98..13208) -------------------------------------
    # The panel and the player's icon do not move; an opponent's icon lies at T(x-x0, -0.01, z-z0) in the
    # zoomed panel turned by Ry(-(a0)), a0 = +58 + +d6 of the player, and is turned itself by Ry(+58 + +d6).
    # So its first polygon is v = C + B*zoom*(dx cos a0, dx sin a0, dz cos a0, dz sin a0) + P cos(a-a0) +
    # Q sin(a-a0), every icon scaled by zoom/0.005. Like the markers it shows the previous tick's cars.
    if [program_word(0x34dd8+4*i) for i in range(3)] != list(RADAR_ZOOMS) or program_word(0x13218) != 0x288b564:
        raise ValueError('unexpected CONDITION panel tables 234dd8, 13218')
    radar_rows = [r for r in rows[1:] if 'ricons' in r and 'zoom' in r and 'cars' in previous[int(r['frame'])]]
    if len(radar_rows) < 1000:
        raise ValueError('the trace has no CONDITION panel fields: make it with this checkout\'s m2segment --trace-hud')
    panels = {r['rpanel'] for r in radar_rows}
    if len(panels) != 1 or len(points(next(iter(panels)))) != 4:
        raise ValueError('the CONDITION panel polygon moves')
    panel_xy = points(next(iter(panels)))
    if {round(bits_float(r['zoom']), 9) for r in radar_rows} != {0.005}:
        raise ValueError('the reference race changes the panel zoom (fit needs 0.005 throughout)')
    radar_zoom = bits_float('%08x' % RADAR_ZOOMS[1])

    def radar_frame(r):
        """(player x0, z0, a0, opponents drawn) for the icons of row r, or None."""
        before = previous[int(r['frame'])]
        inside = radar_inside(before, bits_float(r['zoom']))
        if inside is None or len(inside) != len(r['ricons'].split('|'))-1:
            return None
        return bits_float(before['x']), bits_float(before['z']), (int(before['yaw'])+int(before['pd6'])) & 0xffff, inside

    def radar_feature(frame, car, zoom):
        x0, z0, a0, _ = frame
        c0, s0 = unit(a0)
        dx, dz = car['x']-x0, car['z']-z0
        return zoom*np.array([dx*c0, dx*s0, dz*c0, dz*s0])
    a_rows, values, skipped, icons_fitted = [], [], 0, 0
    for r in radar_rows[::2]:
        frame = radar_frame(r)
        if frame is None:
            skipped += 1
            continue
        k = bits_float(r['zoom'])/radar_zoom
        for car, group in zip(frame[3], r['ricons'].split('|')[1:]):
            feature = radar_feature(frame, car, bits_float(r['zoom']))
            c, s = unit((car['angle']-frame[2]) & 0xffff)
            icons_fitted += 1
            for vertex, (px, py) in enumerate(points(group)):
                for axis, value in enumerate((px, py)):
                    row = np.zeros(10+16)
                    row[axis] = 1
                    row[2+4*axis:6+4*axis] = feature
                    row[10+vertex*4+axis*2] = c*k
                    row[10+vertex*4+axis*2+1] = s*k
                    a_rows.append(row); values.append(value)
    system, values = np.array(a_rows), np.array(values)
    radar_solution, *_ = np.linalg.lstsq(system, values, rcond=None)
    radar_residual = float(np.abs(system@radar_solution-values).max())
    radar_centre, radar_matrix = radar_solution[:2], radar_solution[2:10].reshape(2, 4)
    radar_p = radar_solution[10:].reshape(4, 2, 2)[:, :, 0]
    radar_q = radar_solution[10:].reshape(4, 2, 2)[:, :, 1]
    print(f'CONDITION panel fit: residual={radar_residual:.4f} px over {icons_fitted} icons ({skipped} of '
          f'{len(radar_rows[::2])} frames skipped: a car on the circle or a count mismatch), centre={radar_centre.round(4).tolist()} '
          f'B={radar_matrix.round(4).tolist()}')
    if radar_residual > 0.05 or skipped*20 > len(radar_rows[::2]):
        raise ValueError('the CONDITION panel icons do not follow the panel rule')
    radar_j, *_ = np.linalg.lstsq(radar_p-radar_p.mean(axis=0), radar_q-radar_q.mean(axis=0), rcond=None)
    radar_j = radar_j.T
    radar_j_residual = float(np.abs(radar_p@radar_j.T-radar_q).max())
    turn_sign = 1.0 if np.abs(radar_j-j_matrix).max() < np.abs(radar_j+j_matrix).max() else -1.0
    print(f'CONDITION icon rotation: Q=J*P residual={radar_j_residual:.4f} px, J={radar_j.round(5).tolist()} = {turn_sign:+.0f} x the map\'s J')
    if radar_j_residual > 0.05 or np.abs(radar_j-turn_sign*j_matrix).max() > 0.01:
        raise ValueError('the CONDITION icons do not turn as the map markers do')

    def radar_rotation(angle):
        c, s = unit(angle)
        return c*np.eye(2)+s*radar_j
    radar_snapshots = [f for f in snapshots if 'ricons' in by_frame[f] and 'zoom' in by_frame[f]]
    shapes = {'RADAR_PLAYER': [], 'RADAR_OPPONENT': []}
    colour_votes, frame_cache = {}, {}
    for f in radar_snapshots:
        r = by_frame[f]
        panel, polys = radar_polygons(args.trace/f'polys_{f}.txt')
        frame = radar_frame(r)
        if frame is None or panel is None or len(polys) != 6*len(r['ricons'].split('|')):
            continue
        k = bits_float(r['zoom'])/radar_zoom
        shapes['RADAR_PLAYER'].append([[(np.array(v)-radar_centre)/k for v in p['xy']] for p in polys[:6]])
        for i, car in enumerate(frame[3]):
            origin = radar_centre+radar_matrix@radar_feature(frame, car, bits_float(r['zoom']))
            inverse = np.linalg.inv(radar_rotation((car['angle']-frame[2]) & 0xffff))
            shapes['RADAR_OPPONENT'].append([[inverse@(np.array(v)-origin)/k for v in p['xy']] for p in polys[6+6*i:12+6*i]])
        image = frame_cache.setdefault(f, frames.get(f) or Image.open(args.trace/f'frame_{f}.tga').convert('RGB'))
        for i, p in enumerate(polys):
            colour = sample_colour(image, p['xy'], [q['xy'] for q in polys[i+1:]])
            if colour:
                votes = colour_votes.setdefault(p['colour'], {})
                votes[colour] = votes.get(colour, 0)+1
    radar_colours = {index: max(votes, key=votes.get) for index, votes in colour_votes.items()}
    print('CONDITION icon colours by polygon colour word: ' +
          ', '.join(f'{index:04x}=#{c[0]:02x}{c[1]:02x}{c[2]:02x} ({max(colour_votes[index].values())} of '
                    f'{sum(colour_votes[index].values())} votes)' for index, c in sorted(radar_colours.items())))
    radar_local = {}
    for name, samples in shapes.items():
        if len(samples) < 8:
            raise ValueError(f'{name}: fewer than eight snapshots show it')
        local = [[np.mean([s[i][k] for s in samples], axis=0) for k in range(len(samples[0][i]))] for i in range(6)]
        spread = max(float(np.abs(s[i][k]-local[i][k]).max()) for s in samples for i in range(6) for k in range(len(local[i])))
        print(f'{name}: 6 polygons, {len(samples)} samples, upright geometry spread={spread:.4f} px')
        if spread > 0.08:
            raise ValueError(f'{name} is not rigid under the fitted rotation')
        radar_local[name] = local
    _, first_polys = radar_polygons(args.trace/f'polys_{radar_snapshots[0]}.txt')
    icon_colours = [p['colour'] for p in first_polys[:12]]
    for name, words in (('RADAR_PLAYER', icon_colours[:6]), ('RADAR_OPPONENT', icon_colours[6:12])):
        if any(w not in radar_colours for w in words):
            raise ValueError(f'{name}: a polygon colour is never visible')
        local = radar_local[name]
        xs = [v[0] for q in local for v in q]; ys = [v[1] for q in local for v in q]
        left, top = int(np.floor(min(xs)))-1, int(np.floor(min(ys)))-1
        width, height = int(np.ceil(max(xs)))+1-left, int(np.ceil(max(ys)))+1-top
        image = Image.new('RGBA', (width*RADAR_SCALE, height*RADAR_SCALE), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        for polygon, colour_word in zip(local, words):
            draw.polygon([((v[0]-left)*RADAR_SCALE-0.5, (v[1]-top)*RADAR_SCALE-0.5) for v in polygon],
                         fill=radar_colours[colour_word]+(255,))
        pixels = [(0xff000000 | r << 16 | g << 8 | b) if a else 0 for r, g, b, a in image.get_flattened_data()]
        sprites[name] = dict(width=width*RADAR_SCALE, height=height*RADAR_SCALE, pixels=pixels, scale=RADAR_SCALE,
                             origin=(left, top))
    # The panel: a stipple (texheader 8000) of one colour on every other pixel; on the Dreamcast one
    # translucent rectangle of that colour at half opacity (the same mean, no checker to alias at 640x480).
    panel_rect = [min(v[0] for v in panel_xy), min(v[1] for v in panel_xy), max(v[0] for v in panel_xy), max(v[1] for v in panel_xy)]
    parity_votes = [{}, {}]
    for f in steady[:8]:
        image = frames[f]
        for y in range(int(panel_rect[1])+2, int(panel_rect[3])-1):
            for x in range(int(panel_rect[0])+2, int(panel_rect[2])-1):
                colour = image.getpixel((x, y))
                parity_votes[(x+y) & 1][colour] = parity_votes[(x+y) & 1].get(colour, 0)+1
    stipple = max(((max(v.values())/sum(v.values()), max(v, key=v.get), parity) for parity, v in enumerate(parity_votes)))
    print(f'CONDITION panel: {panel_rect[0]:.3f}..{panel_rect[2]:.3f} x {panel_rect[1]:.3f}..{panel_rect[3]:.3f}, stipple #{stipple[1][0]:02x}{stipple[1][1]:02x}'
          f'{stipple[1][2]:02x} on (x+y)&1=={stipple[2]} ({100*stipple[0]:.1f}% of those pixels)')
    if stipple[0] < 0.9:
        raise ValueError('the CONDITION panel is not a one-colour stipple')
    panel_argb = 0x80000000 | stipple[1][0] << 16 | stipple[1][1] << 8 | stipple[1][2]

    # Course map: static flat polygons, drawn at the arcade's pixel grid.
    xs = [v[0] for p in track for v in p]; ys = [v[1] for p in track for v in p]
    left, top = int(np.floor(min(xs))), int(np.floor(min(ys)))
    width, height = int(np.ceil(max(xs)))+1-left, int(np.ceil(max(ys)))+1-top
    reference = frames[steady[0]]
    map_colour = sample_colour(reference, track[15], track[16:])
    image = Image.new('RGBA', (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    for polygon in track:
        draw.polygon([(v[0]-left-0.5, v[1]-top-0.5) for v in polygon], fill=map_colour+(255,))
    sprites['MAP'] = dict(width=width, height=height, scale=1,
                          pixels=[(0xff000000 | r << 16 | g << 8 | b) if a else 0 for r, g, b, a in image.get_flattened_data()])
    layout['MAP'] = (left, top)
    # Other courses' maps (--course-map): the same flat sprite, with their own marker matrices.
    course_maps = {}
    for slot_text, trace_text in args.course_map:
        slot = int(slot_text)
        if slot not in (1, 2) or slot in course_maps:
            raise ValueError('--course-map takes the slots 1 and 2, once each')
        other = course_map(Path(trace_text), slot, j_matrix, p_first)
        if abs((map_turn(program, slot)*360/65536-other['turn_degrees']+180) % 360-180) > 0.1:
            raise ValueError(f'course {slot}: the markers do not turn with the map (234d98)')
        xs = [v[0] for p in other['track'] for v in p]; ys = [v[1] for p in other['track'] for v in p]
        left2, top2 = int(np.floor(min(xs))), int(np.floor(min(ys)))
        width2, height2 = int(np.ceil(max(xs)))+1-left2, int(np.ceil(max(ys)))+1-top2
        image = Image.new('RGBA', (width2, height2), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        for polygon in other['track']:
            draw.polygon([(v[0]-left2-0.5, v[1]-top2-0.5) for v in polygon], fill=other['colour']+(255,))
        sprites[f'MAP_C{slot}'] = dict(width=width2, height=height2, scale=1,
                                       pixels=[(0xff000000 | r << 16 | g << 8 | b) if a else 0 for r, g, b, a in image.get_flattened_data()])
        layout[f'MAP_C{slot}'] = (left2, top2)
        course_maps[slot] = other

    # Tachometer needle: v_k = pivot + P_k cos(angle) + Q_k sin(angle) (afc0..b0b8, model 288b6a4).
    # The HUD object also runs before the player's update: it shows the previous tick's rpm.
    needle_rows = [r for r in rows[1:] if r['active'] in ('20', '22') and 'needle' in r and len(points(r['needle'])) == 4][::5]
    a_rows, values = [], []
    for r in needle_rows:
        c, s = unit(needle_angle(int(previous[int(r['frame'])]['rpm2'])))
        for k, (px, py) in enumerate(points(r['needle'])):
            for axis, value in enumerate((px, py)):
                row = np.zeros(2+16)
                row[axis] = 1
                row[2+k*4+axis*2] = c
                row[2+k*4+axis*2+1] = s
                a_rows.append(row); values.append(value)
    system, values = np.array(a_rows), np.array(values)
    needle, *_ = np.linalg.lstsq(system, values, rcond=None)
    needle_residual = float(np.abs(system@needle-values).max())
    print(f'needle fit: residual={needle_residual:.4f} px over {len(needle_rows)} frames, pivot={needle[:2].round(4).tolist()}')
    if needle_residual > 0.05:
        raise ValueError('needle does not follow 6a00 + rpm*1.18')
    needle_pq = needle[2:].reshape(4, 2, 2)  # [vertex][axis][cos, sin]
    r = by_frame[steady[0]]
    needle_colour = sample_colour(frames[steady[0]], points(r['needle']), [])
    sprites['NEEDLE'] = dict(width=4, height=4, scale=1,
                             pixels=[0xff000000 | needle_colour[0] << 16 | needle_colour[1] << 8 | needle_colour[2]]*16)

    # ---- the rolling start's overlay ----------------------------------------
    # Last in the table (index >= NM_HUD_TIGHT_EARLY): the race's hot table nm_hud_tight keeps its place and size.
    early_sprites = len(sprites)
    if any(struct.unpack_from('<3I', program, 0x34de8+16*course) != MAP_SCALES[course] for course in range(3)):
        raise ValueError('unexpected map scale table 234de8')
    if any(program_word(a) != v for a, v in ROLLING_WORDS.items()):
        raise ValueError('unexpected rolling start overlay ab78, d438')

    def block_sprite(name, address):
        width, height, tiles = block_tiles(main_data, address)
        tile_sprite(name, width, height, tiles)
        return width, height, tiles
    rolling_words = {}
    for name, key in (('ROLL_ACCEL', 'hint_accel'), ('ROLL_AUTO', 'hint_auto')):
        width, _, tiles = block_sprite(name, ROLLING[key][0])
        layout[name] = cell(ROLLING[key][1])
        rolling_words[key] = tiles[:width]
    width, _, tiles = block_sprite('ROLL_GO', ROLLING['go'][0])
    layout['ROLL_GO'] = cell(ROLLING['go'][1])
    rolling_words['go'] = tiles[4*width:5*width]      # tile row 18 of the screen
    rolling_words['digits'] = []
    for digit, address in enumerate(ROLLING['digits'], 1):
        width, _, tiles = block_sprite(f'ROLL_DIGIT_{digit}', address)
        rolling_words['digits'].append(tiles[2*width:3*width])  # tile row 18 too
    layout['ROLL_DIGIT'] = cell(ROLLING['digit_at'])
    width, _, car_tiles = block_sprite('ROLL_CAR', ROLLING['car'][0])
    layout['ROLL_CAR'] = cell(ROLLING['car'][1])
    wide, tall, tiles = block_tiles(main_data, ROLLING['banner'][0])
    rolling_words['row24'] = car_tiles[:width]+tiles[wide:2*wide]
    # The banner's backdrop is a stipple of one colour on every other pixel: at 640x480 it would alias, and all of it
    # would be textured for the alpha test. The art keeps the letters; the backdrop is a translucent panel of that
    # colour at half opacity (as the CONDITION panel), its rectangles the stipple's outline row by row.
    tile_sprite('ROLL_BANNER', wide, tall, tiles)
    record = sprites.pop('ROLL_BANNER')
    px = np.array(record['pixels'], dtype=np.uint32).reshape(record['height'], record['width'])
    colours, counts = np.unique(px[px != 0], return_counts=True)
    back = int(colours[counts.argmax()])
    ys, xs = np.nonzero(px == back)
    if len(set((xs+ys) & 1)) != 1:
        raise ValueError('the banner backdrop is not a stipple')
    # A row's outline to whole pairs of columns: the stipple starts on every other column from row to row.
    panel_rows = [(int(np.flatnonzero(row)[0]) & ~1, (int(np.flatnonzero(row)[-1])+2) & ~1) for row in px != 0]
    banner_panel = []
    for y, span in enumerate(panel_rows):
        if banner_panel and banner_panel[-1][0] == span[0] and banner_panel[-1][2] == span[1]:
            banner_panel[-1][3] = y+1
        else:
            banner_panel.append([span[0], y, span[1], y+1])
    px[px == back] = 0
    x0, y0 = cell(ROLLING['banner'][1])
    for index, (first, last) in enumerate(((0, ROLLING_SPLIT*8), (ROLLING_SPLIT*8, wide*8))):
        part = px[:, first:last]
        sprites[f'ROLL_BANNER_{index}'] = dict(width=int(part.shape[1]), height=int(part.shape[0]), scale=1,
                                               pixels=[int(v) for v in part.flatten()])
        layout[f'ROLL_BANNER_{index}'] = (x0+first, y0)
    banner_panel_argb = 0x80000000 | back & 0xffffff
    print(f'rolling banner: backdrop #{back & 0xffffff:06x}, {len(banner_panel)} translucent rectangles, '
          f'{sum((b[2]-b[0])*(b[3]-b[1]) for b in banner_panel)} arcade pixels')

    # ---- the lap message (ad34) -----------------------------------------------
    if any(program_word(a) != v for a, v in LAP_MESSAGE_WORDS.items()):
        raise ValueError('unexpected lap message ad34')
    width, _, best_tiles = block_sprite('MSG_BEST', LAP_MESSAGE['best'])
    width, _, lap_tiles = block_sprite('MSG_LAP', LAP_MESSAGE['lap'])
    font = 0x8000+(LAP_MESSAGE['colour'] << 7)
    for index, glyph in enumerate(LAP_MESSAGE['glyphs']):  # glyph numbers of include/hud.h nm_hud_time
        tile_sprite(f'MSG_{index}', 2, 3, [font+(ord(glyph)-32)*6+i for i in range(6)])
    layout['MSG_LABEL'] = cell(LAP_MESSAGE['label_at'])
    layout['MSG_TIME'] = cell(LAP_MESSAGE['time_at'])
    lap_words = dict(best=best_tiles[:width], lap=lap_tiles[:width], font=font,
                     glyphs=[font+(ord(glyph)-32)*6 for glyph in LAP_MESSAGE['glyphs']])

    # ---- the map's "2" marker (12b34: 288b5f0 on the second car while the player leads) -----------------------
    second_first = []
    if args.second_marker:
        slot, trace = int(args.second_marker[0]), Path(args.second_marker[1])
        rows2 = trace_rows(trace/'hud.trace')
        by2, before2 = {int(r['frame']): r for r in rows2}, {int(r['frame']): rows2[i-1] for i, r in enumerate(rows2) if i}
        count = 14
        total = course_maps[slot]['polygons']+count+13
        frames2 = [f for f in sorted(int(p.stem.split('_')[1]) for p in trace.glob('tiles_*.bin'))
                   if by2[f]['active'] == '22' and by2[f].get('overlay') == str(total) and before2[f]['rank'] == '0' and
                   before2[f]['leader_id'] != '255' and by2[f]['rank'] == '0']
        if len(frames2) < 4:
            raise ValueError('--second-marker: too few snapshots while the player leads')
        affine2, samples, votes = course_maps[slot]['affine_leader'], [], {}  # votes by colour word: the 2's strokes are thin
        for f in frames2:
            r = before2[f]
            polys = []
            for line in (trace/f'polys_{f}.txt').read_text().splitlines():
                p = line.split()
                if line.startswith('#') or int(p[1]) or int(p[2], 16):
                    continue
                vertices = [tuple(map(float, v.split(','))) for v in p[9:]]
                if vertices[0][2] < 1e-3:
                    polys.append(([(v[0], v[1]) for v in vertices], p[5]))
            origin = affine2@np.array([bits_float(r['lx']), bits_float(r['lz']), 1.0])
            inverse = np.linalg.inv(course_maps[slot]['size']*rotation(int(r['lyaw'])+map_turn(program, slot)))
            marker = polys[total-13-count:total-13]
            samples.append([[inverse@(np.array(v)-origin) for v in p] for p, _ in marker])
            frame = Image.open(trace/f'frame_{f}.tga').convert('RGB')
            for i, (polygon, word) in enumerate(marker):
                colour = sample_colour(frame, polygon, [p for p, _ in polys[total-13-count+i+1:]])
                if colour:
                    votes.setdefault(word, {})[colour] = votes.setdefault(word, {}).get(colour, 0)+1
        if len({tuple(len(p) for p in s) for s in samples}) != 1 or any(word not in votes for _, word in marker):
            raise ValueError('--second-marker: the marker changes shape or a colour is never visible')
        local2 = [[np.mean([s[i][k] for s in samples], axis=0) for k in range(len(samples[0][i]))] for i in range(count)]
        spread = max(float(np.abs(s[i][k]-local2[i][k]).max()) for s in samples for i in range(count) for k in range(len(local2[i])))
        print(f'MARKER_SECOND: {count} polygons of {len(frames2)} snapshots, upright geometry spread={spread:.4f} px')
        if spread > 0.08:
            raise ValueError('MARKER_SECOND is not rigid under the fitted rotation')
        marker_raster('MARKER_SECOND', local2, [max(votes[word], key=votes[word].get) for _, word in marker])
        second_first = [v.tolist() for v in local2[0]]
        # The player's marker lies over it in the original: a copy on its sheet, as the paint order goes sheet by sheet.
        sprites['MARKER_TOP'] = dict(sprites['MARKER_PLAYER'])

    # ---- palette banks and atlases -----------------------------------------
    for record in sprites.values():
        w, h, px = record['width'], record['height'], record['pixels']
        ink = [(i % w, i//w) for i, p in enumerate(px) if p]
        x0, x1 = min(x for x, _ in ink), max(x for x, _ in ink)+1
        y0, y1 = min(y for _, y in ink), max(y for _, y in ink)+1
        record['crop'] = (x0, y0, x1-x0, y1-y0)
        record['colours'] = {p for p in px if p}
    # The gear indicator's sprites (b0c0) and the rolling start's are placed after all others, in banks and on sheets:
    # the others keep the banks and atlas places they had before them, and these go on the space left or further sheets.
    late = lambda n: n.startswith(('GEAR_', 'ROLL_', 'MSG_')) or n in ('MARKER_SECOND', 'MARKER_TOP')
    banks = []
    for name in sorted((n for n in sprites if n != 'MARKER_TOP'), key=lambda n: (late(n), -len(sprites[n]['colours']))):
        used = sprites[name]['colours']
        if len(used) > 15:
            raise ValueError(f'{name} needs more than 15 colours')
        fits = [(len(used-bank), i) for i, bank in enumerate(banks) if len(used | bank) <= 15]
        if fits:
            index = min(fits)[1]; banks[index] |= used
        else:
            index = len(banks); banks.append(set(used))
        sprites[name]['bank'] = index
    if 'MARKER_TOP' in sprites:
        index = sprites['MARKER_TOP']['bank'] = sprites['MARKER_SECOND']['bank']
        banks[index] |= sprites['MARKER_TOP']['colours']
        if len(banks[index]) > 15:
            raise ValueError('the player\'s marker does not fit the bank of the second car\'s')
    if len(banks) > args.max_banks:
        raise ValueError(f'HUD needs {len(banks)} palette banks, limit {args.max_banks}')
    palettes = [[0]+sorted(bank)+[0]*(15-len(bank)) for bank in banks]

    # Sheets: the sprites of a bank on shelves of one 256x256 sheet (sheet = bank), and what does not fit on further
    # sheets of the same bank after those (no new palette bank). A sprite's `sheet` is its atlas, sheet_banks[sheet]
    # its bank.
    sheet_banks, placed = list(range(len(banks))), {bank: [] for bank in range(len(banks))}

    def shelve(names, bank, sheet):
        """Shelves from the top of `sheet`; returns the sprites that did not fit."""
        x = y = shelf = 0
        for i, name in enumerate(names):
            _, _, w, h = sprites[name]['crop']
            if x+w > 256:
                x, y, shelf = 0, y+shelf+1, 0
            if y+h > 256:
                return names[i:]
            sprites[name]['at'] = (x, y); sprites[name]['sheet'] = sheet; placed[sheet].append(name)
            x += w+1; shelf = max(shelf, h)
        return []
    rest = [shelve(sorted((n for n in sprites if sprites[n]['bank'] == bank), key=lambda n: (late(n), -sprites[n]['crop'][3], n)),
                   bank, bank) for bank in range(len(banks))]
    for bank, names in enumerate(rest):
        while names:
            sheet = len(sheet_banks); sheet_banks.append(bank); placed[sheet] = []
            left = shelve(names, bank, sheet)
            if len(left) == len(names):
                raise ValueError(f'{names[0]} does not fit a 256x256 sheet')
            names = left
    sheets = []
    for sheet, bank in enumerate(sheet_banks):
        names = placed[sheet]
        sheet_width = 256
        bottom = max(sprites[n]['at'][1]+sprites[n]['crop'][3] for n in names)
        sheet_height = max(8, 1 << (bottom-1).bit_length())
        if sheet_height > 256:
            raise ValueError('HUD sheet exceeds 256x256')
        pixels = [0]*(sheet_width*sheet_height)
        for name in names:
            record = sprites[name]; (cx, cy, w, h), (ax, ay) = record['crop'], record['at']
            for dy in range(h):
                for dx in range(w):
                    pixels[(ay+dy)*sheet_width+ax+dx] = record['pixels'][(cy+dy)*record['width']+cx+dx]
        lookup = {colour: i for i, colour in enumerate(palettes[bank]) if i == 0 or colour}
        payload = bytearray(sheet_width*sheet_height//2)
        for py in range(sheet_height):
            for px in range(sheet_width):
                at = block_index(px, py, sheet_width, sheet_height)
                payload[at//2] |= lookup[pixels[py*sheet_width+px]] << ((at & 1)*4)
        blob = bytearray(32)+payload
        struct.pack_into('<4sI', blob, 0, b'DcTx', len(blob)); blob[11] = 15
        struct.pack_into('<HHI', blob, 12, sheet_width, sheet_height,
                         0x28000000 | ((sheet_width.bit_length()-4) << 3) | (sheet_height.bit_length()-4))
        check_pal4(blob, b'DPAL'+struct.pack('<17I', 16, *palettes[bank]),
                   [struct.pack('<I', p) for p in pixels], sheet_width, sheet_height)
        sheets.append(dict(width=sheet_width, height=sheet_height, blob=bytes(blob), pixels=pixels, bank=bank))

    args.output.mkdir()  # Never overwrite an earlier asset pack.
    (args.output/'hud').mkdir()
    for i, sheet in enumerate(sheets):
        (args.output/'hud'/f'sheet-{i}.dt').write_bytes(sheet['blob'])
        preview = Image.new('RGBA', (sheet['width'], sheet['height']))
        preview.putdata([((p >> 16) & 255, (p >> 8) & 255, p & 255, p >> 24) for p in sheet['pixels']])
        preview.save(args.output/f'preview-sheet-{i}.png')
    palette_blob = b'DPAL'+struct.pack('<I', 16*len(palettes))+b''.join(struct.pack('<16I', *p) for p in palettes)
    (args.output/'hud'/'hud.dp').write_bytes(palette_blob)

    # ---- tight geometry ------------------------------------------------------
    # Tile sprites and the map lie on whole arcade pixels and never turn: one quad, or cores
    # and pieces, whichever the cost model prefers.
    # The markers turn with their cars: sub-rectangles of a turned quad cannot repeat its
    # pixels bit for bit, so they stay one punch-through quad (97 arcade pixels each). The
    # needle is one flat colour: one opaque quad.
    names = list(sprites)
    rectangles, tight = [], {}
    for name in names:
        record = sprites[name]; (cx, cy, w, h) = record['crop']
        mask = np.array([[bool(record['pixels'][(cy+y)*record['width']+cx+x]) for x in range(w)] for y in range(h)])
        fixed = record['scale'] == 1 and 'origin' not in record and name != 'NEEDLE'
        whole_cost = float(cost(w, h, 0 if mask.all() else w*h))
        if fixed:
            model_cost, cores, pieces, textured = tight_pieces(mask, cost, name.startswith('ROLL_'))
        else:
            cores, pieces = ([(0, 0, w, h)], []) if mask.all() else ([], [(0, 0, w, h)])
            textured = 0 if mask.all() else w*h
            model_cost = whole_cost
        check_tight(mask, cores, pieces)
        count = len(cores)+len(pieces)
        # What a rectangle beyond the first saves: sprites with the lowest value return to one
        # quad first when a frame would exceed the renderer's rectangle budget.
        priority = 255 if count == 1 else min(255, (w*h-textured)//(count-1))
        tight[name] = dict(first=len(rectangles), cores=len(cores), pieces=len(pieces), fixed=int(fixed), priority=priority,
                           textured=textured, core_texels=sum(c[2]*c[3] for c in cores), opaque=int(mask.sum()), texels=w*h,
                           rectangles=cores+pieces, cost_ns=round(model_cost), whole_ns=round(whole_cost))
        rectangles += cores+pieces
    lines = ['/* Generated by tools/prepare_hud.py from owned daytona93; ROM-derived, never commit. */',
             '#ifndef DAYTONA_HUD_DATA_H', '#define DAYTONA_HUD_DATA_H', '#include <stdint.h>',
             '/* at: atlas texel; origin: offset of the cropped rectangle from the element origin, in',
             ' * sprite texels; scale: sprite texels per arcade pixel. */',
             'typedef struct { uint8_t sheet, scale, width, height; int16_t origin_x, origin_y; uint16_t u, v; } nm_hud_sprite_t;',
             'enum {']
    lines += [f'    NM_HUD_{name}={i},' for i, name in enumerate(names)]
    lines += [f'    NM_HUD_SPRITE_COUNT={len(names)}, NM_HUD_SHEET_COUNT={len(sheets)}, NM_HUD_BANK_FIRST={args.first_bank},',
              f'    NM_HUD_PALETTE_COLORS={16*len(palettes)}', '};',
              'static const uint16_t nm_hud_sheets[NM_HUD_SHEET_COUNT][2]={'+','.join('{%d,%d}' % (s['width'], s['height']) for s in sheets)+'};',
              '/* Palette bank of each sheet (NM_HUD_BANK_FIRST + bank): a bank whose sprites fill one sheet goes on. */',
              '#define NM_HUD_SHEET_BANKS 1',
              'static const uint8_t nm_hud_sheet_banks[NM_HUD_SHEET_COUNT]={'+','.join(str(s['bank']) for s in sheets)+'};',
              'static const nm_hud_sprite_t nm_hud_sprites[NM_HUD_SPRITE_COUNT]={']
    for name in names:
        record = sprites[name]; (cx, cy, w, h) = record['crop']
        ox, oy = record.get('origin', (0, 0))
        lines.append('    {%d,%d,%d,%d,%d,%d,%d,%d}, /* %s */' % (record['sheet'], record['scale'], w, h,
                     cx+ox*record['scale'], cy+oy*record['scale'], record['at'][0], record['at'][1], name))
    lines += ['};',
              '/* Tight geometry (tools/prepare_hud.py, tight_pieces; cost model: %s):' % cost.describe(),
              ' * rectangles first..first+cores-1 hold no transparent texel (opaque list), the next `pieces`',
              ' * hold every opaque texel outside them (punch-through list; they may run under a core).',
              ' * x, y, width, height in texels inside the sprite. fixed: on whole arcade pixels, never turned.',
              ' * A sprite the model draws whole has one rectangle: a core when it has no transparent texel.',
              ' * priority: textured texels saved per rectangle beyond the first. */',
              '#define NM_HUD_TIGHT 1',
              'typedef struct { uint16_t first; uint8_t cores, pieces, fixed, priority; } nm_hud_tight_t;',
              'enum { NM_HUD_RECTANGLE_COUNT=%d, NM_HUD_TIGHT_LINE_NS=%d, NM_HUD_TIGHT_LINES=%d };' % (
                  len(rectangles), round(args.line_ns), args.lines),
              'static const uint8_t nm_hud_rectangles[NM_HUD_RECTANGLE_COUNT][4]={']
    for name in names:
        t = tight[name]
        lines.append('    '+''.join('{%d,%d,%d,%d},' % r for r in t['rectangles'])+' /* %s */' % name)
    tight_row = lambda n: '    {%d,%d,%d,%d,%d}, /* %s: %d of %d texels opaque; cores %d texels, pieces texture %d; model %d ns (one quad %d) */' % (
        tight[n]['first'], tight[n]['cores'], tight[n]['pieces'], tight[n]['fixed'], tight[n]['priority'], n,
        tight[n]['opaque'], tight[n]['texels'], tight[n]['core_texels'], tight[n]['textured'], tight[n]['cost_ns'],
        tight[n]['whole_ns'])
    lines += ['};', '/* Two parts: nm_hud_tight lies in the race\'s hot data block at a fixed size (config/hot_layout.tsv); the',
              ' * sprites of the start and the messages, placed out of line (nm_hud_late_sprite), follow in their own table. */',
              f'enum {{ NM_HUD_TIGHT_EARLY={early_sprites} }};',
              'static const nm_hud_tight_t nm_hud_tight[NM_HUD_TIGHT_EARLY]={']
    lines += [tight_row(n) for n in names[:early_sprites]]
    lines += ['};', 'static const nm_hud_tight_t nm_hud_tight_late[NM_HUD_SPRITE_COUNT-NM_HUD_TIGHT_EARLY]={']
    lines += [tight_row(n) for n in names[early_sprites:]]
    lines += ['};', '#define NM_HUD_TIGHT_OF(sprite) ((sprite)<NM_HUD_TIGHT_EARLY ? nm_hud_tight+(sprite) : '
              'nm_hud_tight_late+((sprite)-NM_HUD_TIGHT_EARLY))',
              '/* Element origins in the arcade 496x384 layout (tile destinations of a6b4..b25c, a818). */', 'enum {']
    lines += [f'    NM_HUD_{name}_X={x}, NM_HUD_{name}_Y={y},' for name, (x, y) in layout.items()]
    lines += ['    NM_HUD_LAP_ROW_Y=32, NM_HUD_LAP_NUMBER_X=24, NM_HUD_LAP_TIME_X=40', '};',
              '#define NM_HUD_REVERSE_BLOCK 1 /* a818 wrong-way message: sprite NM_HUD_REVERSE at NM_HUD_REVERSE_X/Y. */',
              '#define NM_HUD_EXTENSION_BLOCK 1 /* ae98: sprite NM_HUD_EXTENSION at NM_HUD_EXTENSION_X/Y (row 12) or _LOW_X/Y (row 16). */',
              '#define NM_HUD_PIT_BLOCK 1 /* ab2c "PIT WORK!": sprite NM_HUD_PIT at NM_HUD_PIT_X/Y. */',
              '#define NM_HUD_GEAR 1 /* b0c0: GEAR_LEVER_0..6 at NM_HUD_GEAR_LEVER_X/Y, GEAR_OPEN there too, GEAR_DIGIT_1..4, GEAR_CLOSE, GEAR_LABEL. */',
              '#define NM_HUD_ROLLING 1 /* ab78, d438: ROLL_ACCEL, ROLL_AUTO, ROLL_GO, ROLL_DIGIT_1..3, ROLL_CAR, ROLL_BANNER_0, _1. */',
              *(['#define NM_HUD_SECOND_MARKER 1 /* 12b34: MARKER_SECOND, the map\'s "2" (--second-marker). */'] if second_first else []),
              '#define NM_HUD_LAP_MESSAGE 1 /* ad34: MSG_BEST, MSG_LAP at NM_HUD_MSG_LABEL_X/Y, MSG_0.. (nm_hud_time glyphs) from NM_HUD_MSG_TIME_X/Y. */',
              '/* The banner\'s backdrop: translucent rectangles x0, y0, x1, y1 from its origin. */',
              '#define NM_HUD_ROLL_PANEL_ARGB 0x%08xu' % banner_panel_argb,
              f'enum {{ NM_HUD_ROLL_PANEL_RECTS={len(banner_panel)} }};',
              'static const uint16_t nm_hud_roll_panel[NM_HUD_ROLL_PANEL_RECTS][4]={'+','.join(
                  '{%d,%d,%d,%d}' % tuple(b) for b in banner_panel)+'};',
              '/* CONDITION panel (12c98..13208), fitted at zoom NM_HUD_RADAR_ZOOM (the icon sprites\' size): an',
              ' * opponent\'s icon at centre + B*zoom*(dx cos a0, dx sin a0, dz cos a0, dz sin a0), turned by its',
              ' * heading minus a0 (the sprite turn of nm_hud_sprite times NM_HUD_RADAR_TURN), every icon scaled by',
              ' * zoom/NM_HUD_RADAR_ZOOM; the player\'s at the centre, never turned. The panel: arcade rectangle',
              ' * x0, y0, x1, y1, drawn translucent (a stipple in the original). */',
              '#define NM_HUD_RADAR 1',
              '#define NM_HUD_RADAR_ZOOM %.9gf' % radar_zoom,
              '#define NM_HUD_RADAR_TURN %.1ff' % turn_sign,
              '#define NM_HUD_RADAR_PANEL_ARGB 0x%08xu' % panel_argb,
              'static const float nm_hud_radar_centre[2]={%s};' % ','.join(map(c_float, radar_centre)),
              'static const float nm_hud_radar_matrix[2][4]={{%s},{%s}};' % (','.join(map(c_float, radar_matrix[0])),
                                                                            ','.join(map(c_float, radar_matrix[1]))),
              'static const float nm_hud_radar_panel[4]={%s};' % ','.join(map(c_float, panel_rect)),
              '/* Course map (1299c..12c70): screen = M * (x, z, 1) of a car; marker offsets o turn as',
              ' * o*cos(yaw) + (J*o)*sin(yaw). Needle (afc0): pivot + P*cos(a) + Q*sin(a) per vertex. */',
              'static const float nm_hud_map_matrix[2][3]={{%.9gf,%.9gf,%.9gf},{%.9gf,%.9gf,%.9gf}};' % tuple(affine.flatten()),
              'static const float nm_hud_map_leader[2][3]={{%.9gf,%.9gf,%.9gf},{%.9gf,%.9gf,%.9gf}};' % tuple(affine_l.flatten()),
              'static const float nm_hud_map_turn[2][2]={{%.9gf,%.9gf},{%.9gf,%.9gf}};' % tuple(j_matrix.flatten()),
              *['static const float nm_hud_map_matrix_c%d[2][3]={{%.9gf,%.9gf,%.9gf},{%.9gf,%.9gf,%.9gf}};' % (
                  slot, *other['affine'].flatten()) for slot, other in sorted(course_maps.items())],
              *['static const float nm_hud_map_leader_c%d[2][3]={{%.9gf,%.9gf,%.9gf},{%.9gf,%.9gf,%.9gf}};' % (
                  slot, *other['affine_leader'].flatten()) for slot, other in sorted(course_maps.items())],
              '/* The course maps by slot 501460 (--course-map): sprite, origin, player and leader matrices; player NULL:',
              ' * no map for that course.',
              ' * turn, size: what a marker\'s heading adds on this map and its size against course 0\'s (the model\'s angle and depth). */',
              'typedef struct { uint8_t sprite; int16_t x, y; const float (*player)[3], (*leader)[3]; uint16_t turn; float size; } nm_hud_course_map_t;',
              'static const nm_hud_course_map_t nm_hud_course_maps[3]={' + ','.join(
                  ['{NM_HUD_MAP,NM_HUD_MAP_X,NM_HUD_MAP_Y,nm_hud_map_matrix,nm_hud_map_leader,0,1.0f}'] +
                  [('{NM_HUD_MAP_C%d,NM_HUD_MAP_C%d_X,NM_HUD_MAP_C%d_Y,nm_hud_map_matrix_c%d,nm_hud_map_leader_c%d,' % ((slot,)*5))+
                   '%d,%s}' % (map_turn(program, slot), c_float(course_maps[slot]['size'])) if slot in course_maps else '{0,0,0,0,0,0,0.0f}'
                   for slot in (1, 2)]) + '};',
              'static const float nm_hud_needle_pivot[2]={%.9gf,%.9gf};' % tuple(needle[:2]),
              'static const float nm_hud_needle_cos[4][2]={'+','.join('{%.9gf,%.9gf}' % (v[0][0], v[1][0]) for v in needle_pq)+'};',
              'static const float nm_hud_needle_sin[4][2]={'+','.join('{%.9gf,%.9gf}' % (v[0][1], v[1][1]) for v in needle_pq)+'};',
              '#endif', '']
    (args.output/'hud_data.h').write_text('\n'.join(lines))
    fit = dict(affine=affine.tolist(), affine_leader=affine_l.tolist(), turn=j_matrix.tolist(), needle_pivot=needle[:2].tolist(),
               needle_cos=[[v[0][0], v[1][0]] for v in needle_pq.tolist()], needle_sin=[[v[0][1], v[1][1]] for v in needle_pq.tolist()],
               player_first=[v.tolist() for v in player_local[0]], leader_first=[v.tolist() for v in leader_local[0]],
               residuals=dict(player=residual, leader=residual_l, turn=j_residual, needle=needle_residual),
               sheet_banks=sheet_banks,
               sprites={n: dict(sheet=sprites[n]['sheet'], bank=sprites[n]['bank'], scale=sprites[n]['scale'], crop=sprites[n]['crop'], at=sprites[n]['at'],
                                origin=sprites[n].get('origin', (0, 0))) for n in names},
               layout=layout, gear=gear_words, steady_frames=steady, reverse_row25=reverse_row, extension_row=extension_row,
               rolling=rolling_words, lap_message=lap_words, second_first=second_first,
               course_maps={slot: dict(affine=o['affine'].tolist(), affine_leader=o['affine_leader'].tolist(), polygons=o['polygons'],
                                       turn=map_turn(program, slot), size=o['size'],
                                       residual=o['residual'], residual_leader=o['residual_leader'], steady_frames=o['steady'])
                            for slot, o in course_maps.items()},
               radar=dict(zoom=radar_zoom, centre=radar_centre.tolist(), matrix=radar_matrix.tolist(), turn=turn_sign,
                          j=radar_j.tolist(), panel=panel_rect, panel_argb=panel_argb,
                          opponent_first=[v.tolist() for v in radar_local['RADAR_OPPONENT'][0]],
                          player_first=[v.tolist() for v in radar_local['RADAR_PLAYER'][0]],
                          residual=radar_residual, turn_residual=radar_j_residual),
               tight_model=dict(line_ns=args.line_ns, lines=args.lines, build_ns=args.build_ns, tile_ns=args.tile_ns,
                                px_ns=args.px_ns), tight=tight)
    (args.output/'fit.json').write_text(json.dumps(fit, indent=1)+'\n')
    vram = sum(s['width']*s['height']//2 for s in sheets)
    print(f'HUD export: sprites={len(names)} sheets={len(sheets)} '+' '.join(f"{s['width']}x{s['height']}" for s in sheets) +
          f' VRAM payload={vram} bytes; palette banks={len(palettes)} (first {args.first_bank}), colours per bank=' +
          ','.join(str(len(b)) for b in banks)+f'; files={sum(len(s["blob"]) for s in sheets)+len(palette_blob)} bytes')
    print(f'tight geometry ({cost.describe()}): {len(rectangles)} rectangles for {len(names)} sprites, '
          f'{sum(1 for n in names if tight[n]["cores"]+tight[n]["pieces"] > 1)} of them in more than one')
    print(f"  {'sprite':17}{'texels':>7}{'opaque':>7}{'cores':>6}{'texels':>7}{'pieces':>7}{'textured':>9}{'priority':>9}"
          f"{'model ns':>9}{'one quad':>9}")
    for n in names:
        t = tight[n]
        print(f"  {n:17}{t['texels']:>7}{t['opaque']:>7}{t['cores']:>6}{t['core_texels']:>7}{t['pieces']:>7}{t['textured']:>9}{t['priority']:>9}"
              f"{t['cost_ns']:>9}{t['whole_ns']:>9}")


if __name__ == '__main__':
    main()
