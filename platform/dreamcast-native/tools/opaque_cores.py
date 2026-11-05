"""Opaque geometry for punch-through scenery: cores, bars, merged runs.

Two uses:

  python3 platform/dreamcast-native/tools/opaque_cores.py RECIPE.json VIDEO_DUMP SOURCE.dnm NEW_SPEC.json
      designs, per texture of the recipe, the texel changes and the opaque
      polygons in texture space (the specification), from the textures of the
      reference video dump;
  prepare_face_order.py --opaque-cores SPEC.json     applies it to a mesh pack,
  prepare_palette_materials.py --texture-fill SPEC.json   to the textures.

Why: on the PowerVR every punch-through polygon is paid for in passes of the
tile renderer, also where its texels are opaque or hidden; an opaque-list
polygon costs next to nothing there. Measured on hardware (13 October 2026):
the same tree triangles on the opaque list cost what no trees cost, and
trimming transparent area while staying on the punch-through list does not
help. So what is opaque in a cutout texture is drawn as opaque polygons.

Per texture word (material header word 2) the recipe chooses one kind:

  overlay  The punch-through face stays as it is. Convex polygons that touch
           only texels opaque in the (filled) texture, and whose 8 neighbours
           are opaque too, are drawn on top of it on the opaque list, one
           layer above the face (written depth times 1+step, meshpack.h):
           there the punch-through fragments fail the depth test and never
           reach the alpha test. What still shows of the punch-through face
           is the ragged fringe between core and silhouette. No shared edge
           is involved: a gap between cores shows the face below.
           Option fringe: `face` keeps the whole punch-through face;
           `hull` trims it to one convex polygon around its opaque texels;
           `pieces` replaces it by one convex punch-through polygon per
           edge of the largest core, each reaching from just inside that
           edge to the silhouette (they overlap each other at the corners
           and the core by 1/8 texel, so no crack can open between them);
           `none` draws the cores alone. The transparent corners of the
           face are then no longer rasterised. A face whose trimmed
           polygons would not cover every opaque texel keeps its whole
           face, as does a quad cut as two triangles.
  outline  The punch-through face is removed. One convex polygon fitted to
           the silhouette (plus rectangles for thin parts such as a trunk)
           is drawn on the opaque list: a polygonal silhouette.
  rects    The punch-through face is removed. The opaque texels are covered
           by a few axis-aligned rectangles (bars, posts, panels).
  bands    A solid tapered body with two ragged long sides (a pine lying
           along the texture: tip at one end, trunk at the other). Per side
           an inner and an outer polyline of `points` points each are
           fitted: the inner one along the bottoms of the notches, the outer
           one along the branch tips. The body between the two inner lines
           is one opaque strip that zig-zags between the sides; each side
           gets one punch-through strip between its inner and outer line,
           sharing the body's vertices along the inner line; a punch-through
           quad closes each end; thin parts (the trunk) are opaque
           rectangles. Nothing punch-through lies behind the body and
           nothing is drawn beyond the outer lines: foliage texels out there
           are lost, transparent texels the body touches (and their
           neighbours, for the reason given under gutter) are filled, and
           the notches between the lines keep their punch-through texels.
           The fit minimises lost*(opaque texels outside) + fill*(transparent
           texels in the body) + band*(texels in the punch-through strips).
  cone     The same pieces fitted to what a pine is, a cone with sawtooth
           sides (design_cone; written to the specification as kind bands):
           the body starts at one apex vertex and may touch no texel that is
           transparent or next to a transparent one, so nothing is filled at
           the silhouette; the bands start at an outer apex vertex on the
           real tip and are fitted for little transparent area.
  tree     A pine whose pieces leave as triangle strips (design_tree; written
           as kind bands): the body is ONE strip of a few corners from an
           apex near the tip to the base, the punch-through ring between it
           and an enclosing outline (convex, or a simple polygon whose long
           sides may bend inwards; no crown texel outside it) is tiled by the
           two polygons' own corners into as few strips as found (the walk
           round the ring with the fewest strips, tile_strips), the trunk is
           one opaque quad that starts beside the body (two opaque faces
           that overlap in a plane get different draw layers, and a layer
           ends a strip). A strip's faces are quads where two of its
           triangles form a convex quad, the odd triangle last, so the
           renderer may start a run at any quad. A face that shows only part
           of the texture, or maps it mirrored (nearly all tree faces do: a
           strip must then start on a triangle that is negative in the
           texture), gets its own tiling at apply time (tree_pieces).
  far      A pine for far away (design_far; written as kind outline): one
           convex opaque polygon of 3 to 5 corners with its first corner on
           the tip, fitted for the least silhouette deviation, and the trunk
           quad; its filled texture is the specification's own.
  wall     A wall that is solid from some u on and ragged towards low u,
           repeating along v (a tree line; design_wall, written as kind
           bands): opaque beyond an inner line, a punch-through strip from
           there to an outer line that holds every opaque texel; both lines
           are unrolled over each face and tiled there (wall_pieces).

Texel changes, always within the texture's own 16-entry palette (no palette
changes, no new bank; the texture stays PAL4):
  fill     small transparent holes become opaque (overlay: enclosed holes up
           to `hole` texels and notches closed by `close` 3x3 closings;
           outline, rects: every transparent texel a polygon touches);
  gutter   outline and rects only: the transparent texels around a polygon
           take the colour of their opaque neighbour, so a sample that
           strays over the polygon's edge (float error, or a bilinear
           filter) finds the edge colour and not entry 15. Needs every user
           of the texture to be converted; apply() refuses otherwise.
Fill colour: `dark` the darkest neighbour (foliage holes read as shadow),
`near` the neighbour's own entry (edges are continued).

Also here, for punch-through materials that stay what they are:
  unwrap   adjacent faces whose texture coordinates agree up to whole texture
           periods get the same coordinates along the shared edge, so they
           share vertex records and prepare_face_order chains them into
           strips. The texels sampled are the same.
  merge    two coplanar quads of one material, group, normal and affine
           mapping that share an edge become one quad.

Coordinates: the pack's UV units, 1/8 texel. Every vertex of a piece lies on
that lattice and gets its position from the source triangle's own affine
map, so a piece samples exactly the texels the source face sampled there. A
quad whose corners are not an affine image of its UVs is handled as the two
triangles the renderer draws (fan 0-1-2, 0-2-3): each is cut on its own.
"""
from collections import Counter, defaultdict
from fractions import Fraction
import math
import struct

import numpy as np

import prepare_models as pm
import split_cutouts as sc


# ---- texels -----------------------------------------------------------------

def _neighbours(mask, periodic):
    """Mask of texels with a set texel among the 8 neighbours or themselves."""
    out = mask.copy()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if not dy and not dx:
                continue
            moved = np.roll(np.roll(mask, dy, 0), dx, 1)
            if not periodic[1]:
                if dy == 1:
                    moved[0, :] = False
                if dy == -1:
                    moved[-1, :] = False
            if not periodic[0]:
                if dx == 1:
                    moved[:, 0] = False
                if dx == -1:
                    moved[:, -1] = False
            out |= moved
    return out


def _erode(mask, periodic):
    return ~_neighbours(~mask, periodic)


def _colour(image, targets, mode, inverted, periodic):
    """Entries for the transparent texels `targets`, grown from the opaque
    ones: each pass gives every target with an opaque or already coloured
    8-neighbour the darkest of them (`dark`) or the first in a fixed order
    (`near`). Returns [(x, y, entry)]."""
    work = image.astype(np.int64)
    known = image != 15
    todo = targets & ~known
    out = []
    position = (15-work) if inverted else work           # larger = brighter
    while todo.any():
        best = np.full(image.shape, 99, np.int64)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0), (1, 1), (1, -1), (-1, 1), (-1, -1)):
            value = np.roll(np.roll(position, dy, 0), dx, 1)
            have = np.roll(np.roll(known, dy, 0), dx, 1)
            if not periodic[1]:
                if dy == 1:
                    have[0, :] = False
                if dy == -1:
                    have[-1, :] = False
            if not periodic[0]:
                if dx == 1:
                    have[:, 0] = False
                if dx == -1:
                    have[:, -1] = False
            if mode == 'dark':
                best = np.where(have & (value < best), value, best)
            else:
                best = np.where(have & (best == 99), value, best)
        ready = todo & (best != 99)
        if not ready.any():
            raise ValueError('a texel to fill has no opaque texel to take its colour from')
        position = np.where(ready, best, position)
        known = known | ready
        todo = todo & ~ready
        for y, x in zip(*np.nonzero(ready)):
            entry = int(15-position[y, x]) if inverted else int(position[y, x])
            if entry == 15:          # the hole entry itself: its ramp neighbour
                entry = 14
            out.append((int(x), int(y), entry))
    return out


def holes(image, hole, close, periodic=(False, False)):
    """Transparent texels to fill: 4-connected transparent regions that do not
    reach the texture's (non-periodic) border and have at most `hole`
    texels, then what `close` closings with the 3x3 square add."""
    clear = image == 15
    height, width = image.shape
    outside = np.zeros_like(clear)
    if not periodic[1]:
        outside[0, :] |= clear[0, :]
        outside[-1, :] |= clear[-1, :]
    if not periodic[0]:
        outside[:, 0] |= clear[:, 0]
        outside[:, -1] |= clear[:, -1]
    if periodic[0] and periodic[1]:
        raise ValueError('a texture periodic both ways has no outside')
    while True:
        grown = outside.copy()
        for axis, step in ((0, 1), (0, -1), (1, 1), (1, -1)):
            moved = np.roll(outside, step, axis)
            if not periodic[1 - axis]:
                index = [slice(None), slice(None)]
                index[axis] = 0 if step == 1 else -1
                moved[tuple(index)] = False
            grown |= moved & clear
        if (grown == outside).all():
            break
        outside = grown
    enclosed = clear & ~outside
    fill = np.zeros_like(clear)
    seen = np.zeros_like(clear)
    for y, x in zip(*np.nonzero(enclosed)):
        if seen[y, x]:
            continue
        stack, members = [(y, x)], []
        seen[y, x] = True
        while stack:
            cy, cx = stack.pop()
            members.append((cy, cx))
            for ny, nx in ((cy+1, cx), (cy-1, cx), (cy, cx+1), (cy, cx-1)):
                if periodic[1]:
                    ny %= height
                if periodic[0]:
                    nx %= width
                if 0 <= ny < height and 0 <= nx < width and enclosed[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    stack.append((ny, nx))
        if len(members) <= hole:
            for cy, cx in members:
                fill[cy, cx] = True
    solid = ~clear | fill
    closed = solid
    for _ in range(close):
        closed = _neighbours(closed, periodic)
    for _ in range(close):
        closed = _erode(closed, periodic)   # (beyond a non-periodic border counts as solid)
    if close:
        fill |= closed & clear
    return fill


def filled(image, fill):
    out = image.copy()
    for x, y, entry in fill:
        out[y, x] = entry
    return out


# ---- design -----------------------------------------------------------------

def _lattice(poly, tex, shrink=40):
    """A polygon of exact points on the 1/8 texel lattice, still touching only
    safe texels of `tex` (shrunk towards its centre until it does)."""
    count = len(poly)
    cx = sum(p[0] for p in poly)/count
    cy = sum(p[1] for p in poly)/count
    for step in range(shrink):
        scale = Fraction(200-step, 200)
        snapped = []
        for x, y in poly:
            point = (int(round(cx+(x-cx)*scale)), int(round(cy+(y-cy)*scale)))
            if not snapped or snapped[-1] != point:
                snapped.append(point)
        if len(snapped) > 1 and snapped[0] == snapped[-1]:
            snapped.pop()
        snapped = _convex(snapped)
        if snapped and not (tex.touched_by(snapped) & ~tex.safe).any():
            return snapped
    return None


def _convex(poly):
    """The polygon without collinear corners, or None unless strictly convex
    and counter-clockwise."""
    poly = list(poly)
    changed = True
    while changed and len(poly) >= 3:
        changed = False
        for i in range(len(poly)):
            if sc._cross(poly[i-1], poly[i], poly[(i+1) % len(poly)]) == 0:
                del poly[i]
                changed = True
                break
    if len(poly) < 3 or any(sc._cross(poly[i-1], poly[i], poly[(i+1) % len(poly)]) <= 0 for i in range(len(poly))):
        return None
    return poly


def design_overlay(image, hole=64, close=1, cores=2, vertices=6, least=0.02, colour='dark', inverted=False,
                   periodic=(False, False), **applied):
    """Fill and cores of an overlay texture: see the module text. applied:
    the options apply() reads from the specification (fringe, hull_cuts,
    fringe_least)."""
    if set(applied)-{'fringe', 'hull_cuts', 'fringe_least'}:
        raise ValueError(f'unknown overlay options {sorted(applied)}')
    fill_mask = holes(image, hole, close, periodic)
    fill = _colour(image, fill_mask, colour, inverted, periodic)
    variant = filled(image, fill)
    height, width = image.shape
    out = dict(kind='overlay', fill=fill, polygons=[], bands=[])
    if periodic[1] or periodic[0]:
        # A band across the whole period: the widest run of columns (rows)
        # whose texels are all safe.
        axis = 0 if periodic[1] else 1                    # coordinate the band bounds
        wide = np.concatenate([variant, variant, variant], 0 if periodic[1] else 1)
        opaque = wide != 15
        safe = opaque.copy()
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                moved = np.roll(np.roll(opaque, dy, 0), dx, 1)
                if periodic[1]:
                    if dx == 1:
                        moved[:, 0] = False
                    if dx == -1:
                        moved[:, -1] = False
                else:
                    if dy == 1:
                        moved[0, :] = False
                    if dy == -1:
                        moved[-1, :] = False
                safe &= moved
        line = safe.all(axis=0 if periodic[1] else 1)
        best, start = (0, 0, 0), None
        for i, value in enumerate(list(line)+[False]):
            if value and start is None:
                start = i
            if not value and start is not None:
                best = max(best, (i-start, start, i))
                start = None
        if best[0] >= 2:
            out['bands'].append([axis, 8*best[1], 8*best[2]])
        return out
    footprint = [(Fraction(0), Fraction(0)), (Fraction(8*width), Fraction(0)), (Fraction(8*width), Fraction(8*height)),
                 (Fraction(0), Fraction(8*height))]
    tex = sc.Texels(variant, 0, (0, 0), (8*width, 8*height))
    # The texture does not repeat for these faces: what lies beyond it is not safe.
    inside = (tex.cx >= 0) & (tex.cx < 8*width) & (tex.cy >= 0) & (tex.cy < 8*height)
    tex.opaque = tex.opaque & inside
    edge = inside & (tex.cx >= 8) & (tex.cx < 8*width-8) & (tex.cy >= 8) & (tex.cy < 8*height-8)
    tex.safe = tex.safe & edge
    total = sc._area2(footprint)
    planes = [sc._plane(p, q) for p, q in zip(footprint, footprint[1:]+footprint[:1])]
    regions = [(footprint, tex.touched(planes))]
    while regions and len(out['polygons']) < cores:
        regions.sort(key=lambda region: -int((region[1] & tex.safe).sum()))
        poly, mask = regions.pop(0)
        if (mask & tex.safe).sum()*64 < least*total:
            break
        core = sc._inner(tex, poly, mask, vertices)
        if not core or sc._area2(core) < least*total:
            continue
        for p, q in zip(core, core[1:]+core[:1]):
            a, b, c = sc._plane(p, q)
            beyond = sc._clip(poly, -a, -b, -c)
            if beyond:
                regions.append((beyond, mask & (tex.low(-a, -b) < -c)))
            poly = sc._clip(poly, a, b, c)
            mask = mask & (tex.low(a, b) < c)
        snapped = _lattice(core, tex)
        if snapped:
            out['polygons'].append([list(p) for p in snapped])
    return out


def _inside(poly, xs, ys):
    """Points strictly inside a counter-clockwise convex polygon."""
    mask = np.ones(xs.shape, bool)
    for (ax, ay), (bx, by) in zip(poly, poly[1:]+poly[:1]):
        mask &= (bx-ax)*(ys-ay)-(by-ay)*(xs-ax) > 0
    return mask


def _hull(points):
    points = sorted(set(points))
    def half(seq):
        out = []
        for p in seq:
            while len(out) >= 2 and sc._cross(out[-2], out[-1], p) <= 0:
                out.pop()
            out.append(p)
        return out
    lower, upper = half(points), half(points[::-1])
    return lower[:-1]+upper[:-1]


def _fit(mask, vertices, weight, steps=(4, 2, 1), reach=2):
    """A convex polygon of `vertices` corners on texel corners with the largest
    (opaque texel centres inside) - weight * (transparent centres inside):
    the convex hull of the mask reduced to that many corners, then every
    corner moved in turn while the score rises."""
    height, width = mask.shape
    ys, xs = np.mgrid[0:height, 0:width]
    xs, ys = xs+0.5, ys+0.5
    gain = np.where(mask, 1.0, -weight)

    def score(poly):
        return float(gain[_inside(poly, xs, ys)].sum())
    rows, columns = np.nonzero(mask)
    points = []
    for y, x in zip(rows, columns):
        points += [(int(x), int(y)), (int(x)+1, int(y)), (int(x), int(y)+1), (int(x)+1, int(y)+1)]
    poly = _hull(points)
    while len(poly) > vertices:
        loss = [abs(sc._cross(poly[i-1], poly[i], poly[(i+1) % len(poly)])) for i in range(len(poly))]
        del poly[loss.index(min(loss))]
    best = score(poly)
    for step in steps:
        improved = True
        while improved:
            improved = False
            for i in range(len(poly)):
                for dx in range(-reach, reach+1):
                    for dy in range(-reach, reach+1):
                        if not dx and not dy:
                            continue
                        point = (poly[i][0]+dx*step, poly[i][1]+dy*step)
                        if not 0 <= point[0] <= width or not 0 <= point[1] <= height:
                            continue
                        trial = poly[:i]+[point]+poly[i+1:]
                        if _convex(trial) != trial:
                            continue
                        value = score(trial)
                        if value > best:
                            best, poly, improved = value, trial, True
    return poly


def _touched(poly, shape, grow=0.0):
    """Texels whose open square meets the open convex polygon (texel units),
    the polygon grown by `grow` on every side."""
    height, width = shape
    cy, cx = np.mgrid[0:height, 0:width]
    mask = np.ones(shape, bool)
    for (ax, ay), (bx, by) in zip(poly, poly[1:]+poly[:1]):
        a, b = by-ay, ax-bx                                # a*x+b*y <= c on the left
        c = a*ax+b*ay
        mask &= a*cx+b*cy+min(0, a)+min(0, b) < c+grow*(abs(a)+abs(b))
    xs, ys = [p[0] for p in poly], [p[1] for p in poly]
    return mask & (cx+1 > min(xs)-grow) & (cx < max(xs)+grow) & (cy+1 > min(ys)-grow) & (cy < max(ys)+grow)


def _components(mask):
    seen = np.zeros_like(mask)
    out = []
    for y, x in zip(*np.nonzero(mask)):
        if seen[y, x]:
            continue
        stack, members = [(y, x)], []
        seen[y, x] = True
        while stack:
            cy, cx = stack.pop()
            members.append((cy, cx))
            for ny in (cy-1, cy, cy+1):
                for nx in (cx-1, cx, cx+1):
                    if 0 <= ny < mask.shape[0] and 0 <= nx < mask.shape[1] and mask[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
        out.append(members)
    return out


def _finish(image, polygons, colour, inverted, kind, limit=None):
    """Fill and gutter of a texture whose users are all replaced by `polygons`
    (texel units), and what the polygons lose and add. limit: only the
    texels x < limit[0], y < limit[1] are ever mapped."""
    opaque = image != 15
    touched = np.zeros(image.shape, bool)
    centre = np.zeros(image.shape, bool)
    height, width = image.shape
    ys, xs = np.mgrid[0:height, 0:width]
    for poly in polygons:
        touched |= _touched(poly, image.shape)
        centre |= _inside(poly, xs+0.5, ys+0.5)
    used = np.ones(image.shape, bool)
    if limit:
        used = (xs < limit[0]) & (ys < limit[1])
    # The ring wraps: a polygon at the texture's edge has its neighbours on the other side.
    ring = _neighbours(touched, (True, True)) & ~touched
    fill = _colour(image, (touched | ring) & ~opaque, colour, inverted, (True, True))
    inner = {(x, y) for y, x in zip(*np.nonzero(touched & ~opaque))}
    lost = opaque & ~centre & used
    added = touched & ~opaque & centre

    def depth(mask, into):
        """Largest number of 3x3 erosions a texel of `mask` survives inside `into`."""
        level, left = 0, into.copy()
        while (left & mask).any():
            level += 1
            left = _erode(left, (False, False))
        return level
    union = centre
    return dict(kind=kind, polygons=[[[8*int(x), 8*int(y)] for x, y in poly] for poly in polygons], bands=[],
                fill=[entry for entry in fill if (entry[0], entry[1]) in inner],
                gutter=[entry for entry in fill if (entry[0], entry[1]) not in inner],
                report=dict(opaque_texels=int((opaque & used).sum()), lost_texels=int(lost.sum()),
                            filled_texels=int(added.sum()),
                            lost_depth_texels=depth(lost, ~union), filled_depth_texels=depth(added, ~opaque)))


def design_outline(image, vertices=7, weight=1.0, thin=5, least=48, colour='near', inverted=False, limit=None):
    """One convex polygon for the body of the silhouette and a rectangle for
    each thin part left over (a trunk): see the module text. thin: 3x3
    erosions that remove the thin parts before the fit; least: fewest texels
    of a thin part that gets a rectangle."""
    opaque = image != 15
    body = opaque.copy()
    for _ in range(thin):
        body = _erode(body, (False, False))
    for _ in range(thin):
        body = _neighbours(body, (False, False))
    body &= opaque
    parts = _components(body)
    main = np.zeros_like(opaque)
    for y, x in max(parts, key=len):
        main[y, x] = True
    # The fit sees every opaque texel that hangs on the body, thin parts excepted.
    poly = _fit(opaque & _neighbours(_neighbours(_neighbours(main, (False, False)), (False, False)), (False, False)) | main,
                vertices, weight)
    polygons = [poly]
    height, width = image.shape
    ys, xs = np.mgrid[0:height, 0:width]
    rest = opaque & ~_inside(poly, xs+0.5, ys+0.5) & ~_neighbours(main, (False, False))
    for members in _components(rest):
        if len(members) < least:
            continue
        y0, y1 = min(m[0] for m in members), max(m[0] for m in members)+1
        x0, x1 = min(m[1] for m in members), max(m[1] for m in members)+1
        if len(members) < 0.6*(y1-y0)*(x1-x0) or max(y1-y0, x1-x0) < 3*min(y1-y0, x1-x0):
            continue                                      # not a bar: a tip of the body the polygon cut off
        # Rows and columns that are mostly opaque; then reach into the body's polygon so no gap opens.
        block = opaque[y0:y1, x0:x1]
        rows = np.nonzero(block.mean(1) >= 0.5)[0]
        columns = np.nonzero(block.mean(0) >= 0.5)[0]
        y0, y1, x0, x1 = y0+int(rows[0]), y0+int(rows[-1])+1, x0+int(columns[0]), x0+int(columns[-1])+1
        horizontal = x1-x0 >= y1-y0
        if horizontal:
            if _inside(poly, xs[y0:y1, :x0]+0.5, ys[y0:y1, :x0]+0.5).any():      # body on the left
                while x0 > 0 and not _inside(poly, xs[y0:y1, x0]+0.5, ys[y0:y1, x0]+0.5).all():
                    x0 -= 1
            else:
                while x1 < width and not _inside(poly, xs[y0:y1, x1-1]+0.5, ys[y0:y1, x1-1]+0.5).all():
                    x1 += 1
        else:
            if _inside(poly, xs[:y0, x0:x1]+0.5, ys[:y0, x0:x1]+0.5).any():      # body above
                while y0 > 0 and not _inside(poly, xs[y0, x0:x1]+0.5, ys[y0, x0:x1]+0.5).all():
                    y0 -= 1
            else:
                while y1 < height and not _inside(poly, xs[y1-1, x0:x1]+0.5, ys[y1-1, x0:x1]+0.5).all():
                    y1 += 1
        polygons.append([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])
    return _finish(image, polygons, colour, inverted, 'outline', limit)


def _triangles(mask_of, triangles):
    out = None
    for triangle in triangles:
        inside = mask_of(triangle)
        out = inside if out is None else out | inside
    return out


def _band_quads(lines):
    """Quads (corner order: see design_bands) of the body, the two side
    bands and the two end caps from the four polylines."""
    it, ot, ib, ob = lines
    count = len(it)
    core = [[ib[i+1], ib[i], it[i], it[i+1]] for i in range(count-1)]
    top = [[it[i+1], it[i], ot[i], ot[i+1]] for i in range(count-1)]
    bottom = [[ob[i+1], ob[i], ib[i], ib[i+1]] for i in range(count-1)]
    caps = [[ot[0], it[0], ib[0], ob[0]], [it[-1], ot[-1], ob[-1], ib[-1]]]
    return core, top, bottom, caps


def _fan(quads):
    return [t for q in quads for t in ((q[0], q[1], q[2]), (q[0], q[2], q[3]))]


def design_bands(image, points=4, lost=3.0, fill=1.0, band=0.3, thin=5, least=48, colour='dark', inverted=False,
                 steps=(4, 2, 1), reach=2):
    """Body, side bands, end caps and trunk of a `bands` texture: see the
    module text. The body runs along u. A quad is given as four corners
    c0..c3, drawn as the fan (c0,c1,c2),(c0,c2,c3): c1-c2 is the edge it
    shares with the previous quad of its strip and c3-c0 the edge it shares
    with the next, so consecutive quads chain into one triangle strip
    (prepare_face_order's strips cross a quad exactly between those edges)."""
    if points < 2:
        raise ValueError('bands need at least 2 points per line')
    opaque = image != 15
    height, width = image.shape
    ys, xs = np.mgrid[0:height, 0:width]
    cx, cy = xs+0.5, ys+0.5
    body = opaque.copy()
    for _ in range(thin):
        body = _erode(body, (False, False))
    for _ in range(thin):
        body = _neighbours(body, (False, False))
    body &= opaque
    main = np.zeros_like(opaque)
    for y, x in max(_components(body), key=len):
        main[y, x] = True
    # Thin parts: bars standing off the body (the trunk); they are not the fit's business.
    grown = main
    for _ in range(3):
        grown = _neighbours(grown, (False, False))
    bars = []
    crown = opaque.copy()
    for members in _components(opaque & ~grown):
        y0, y1 = min(m[0] for m in members), max(m[0] for m in members)+1
        x0, x1 = min(m[1] for m in members), max(m[1] for m in members)+1
        if len(members) < least or len(members) < 0.6*(y1-y0)*(x1-x0) or x1-x0 < 3*(y1-y0):
            continue
        rows = np.nonzero(opaque[y0:y1, x0:x1].mean(1) >= 0.5)[0]
        bars.append([x0, y0+int(rows[0]), x1, y0+int(rows[-1])+1])
        crown[y0:y1, x0:x1] = False
    columns = np.nonzero(crown.any(0))[0]
    u0, u1 = int(columns[0]), int(columns[-1])+1
    solid_top, solid_bottom, tip_top, tip_bottom = {}, {}, {}, {}
    for u in range(u0, u1):
        rows = np.nonzero(crown[:, u])[0]
        centre = int(np.median(rows))
        top = centre
        while top > 0 and crown[top-1, u]:
            top -= 1
        bottom = centre+1
        while bottom < height and crown[bottom, u]:
            bottom += 1
        solid_top[u], solid_bottom[u], tip_top[u], tip_bottom[u] = top, bottom, int(rows[0]), int(rows[-1])+1
    knots = [int(round(u0+(u1-u0)*i/(points-1))) for i in range(points)]
    half = max(2, (u1-u0)//(2*(points-1)))
    lines = [[], [], [], []]
    for k, knot in enumerate(knots):
        window = [u for u in range(max(u0, knot-half), min(u1, knot+half+1))]
        inner_top = max(solid_top[u] for u in window)
        inner_bottom = min(solid_bottom[u] for u in window)
        if inner_bottom-inner_top < 2:
            middle = (inner_top+inner_bottom)//2
            inner_top, inner_bottom = middle-1, middle+1
        outward = -3 if k == 0 else 3 if k == points-1 else 0
        lines[0].append((knot, inner_top))
        lines[1].append((min(max(knot+outward, 0), width), min(tip_top[u] for u in window)))
        lines[2].append((knot, inner_bottom))
        lines[3].append((min(max(knot+outward, 0), width), max(tip_bottom[u] for u in window)))

    def inside(triangle):
        return _inside(list(triangle), cx, cy)

    def valid(lines):
        for line in lines:
            if any(b[0]-a[0] < 2 for a, b in zip(line, line[1:])):
                return False
            if any(not 0 <= p[0] <= width or not 0 <= p[1] <= height for p in line):
                return False
        core, top, bottom, caps = _band_quads(lines)
        # Strictly convex quads: a face whose UVs run the other way round is drawn
        # on the quad's other diagonal (its corners reversed), which must be the same picture.
        return all(sc._cross(q[i-1], q[i], q[(i+1) % 4]) >= 4 for q in core+top+bottom+caps for i in range(4))

    def score(lines):
        core, top, bottom, caps = _band_quads(lines)
        body = _triangles(inside, _fan(core))
        strips = _triangles(inside, _fan(top+bottom+caps)) & ~body
        near = _neighbours(body, (False, False))
        return lost*float((crown & ~body & ~strips).sum())+fill*float((near & ~opaque).sum())+band*float(strips.sum())
    if not valid(lines):
        # A silhouette without any raggedness, or one that touches the texture's edge: start
        # with the end points of the inner lines two texels in and the outer lines a texel out.
        for k in (0, points-1):
            inward = 2 if k == 0 else -2
            lines[0][k] = (lines[0][k][0]+inward, lines[0][k][1])
            lines[2][k] = (lines[2][k][0]+inward, lines[2][k][1])
        lines[1] = [(u, max(min(v, lines[0][k][1]-1), 0)) for k, (u, v) in enumerate(lines[1])]
        lines[3] = [(u, min(max(v, lines[2][k][1]+1), height)) for k, (u, v) in enumerate(lines[3])]
    if not valid(lines):
        raise ValueError('the texture is not a body with two ragged sides along u')
    best = score(lines)
    for step in steps:
        improved = True
        while improved:
            improved = False
            for line in range(4):
                for k in range(points):
                    for du in range(-reach, reach+1):
                        for dv in range(-reach, reach+1):
                            if not du and not dv:
                                continue
                            trial = [list(l) for l in lines]
                            trial[line][k] = (lines[line][k][0]+du*step, lines[line][k][1]+dv*step)
                            if not valid(trial):
                                continue
                            value = score(trial)
                            if value < best:
                                best, lines, improved = value, trial, True
    core, top, bottom, caps = _band_quads(lines)
    # The trunk reaches into the body, so no gap opens between them.
    body = _triangles(inside, _fan(core))
    polygons = []
    for x0, y0, x1, y1 in bars:
        if body[y0:y1, :x0].any():
            while x0 > 0 and not body[y0:y1, x0].all():
                x0 -= 1
        else:
            while x1 < width and not body[y0:y1, x1-1].all():
                x1 += 1
        polygons.append([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])
    touched = np.zeros(image.shape, bool)
    for triangle in _fan(core)+[tuple(p[:3]) for p in polygons]+[(p[0], p[2], p[3]) for p in polygons]:
        touched |= _touched(list(triangle), image.shape, 0.125)
    # The ring wraps, as the sampler does at the texture's edge (a trunk that ends there).
    solid = _neighbours(touched, (True, True))
    filled_texels = _colour(image, solid & ~opaque, colour, inverted, (True, True))
    drawn = body | _triangles(inside, _fan(top+bottom+caps))
    for poly in polygons:
        drawn |= _inside(poly, cx, cy)
    gone = opaque & ~drawn
    strips = _triangles(inside, _fan(top+bottom+caps)) & ~body
    scale = lambda quads: [[[8*int(u), 8*int(v)] for u, v in quad] for quad in quads]
    return dict(kind='bands', polygons=[[[8*int(x), 8*int(y)] for x, y in poly] for poly in polygons], bands=[],
                quads=[dict(list='M', corners=q) for q in scale(caps[:1]+top+caps[1:]+bottom)] +
                      [dict(list='O', corners=q) for q in scale(core)],
                lines=dict(inner_top=lines[0], outer_top=lines[1], inner_bottom=lines[2], outer_bottom=lines[3]),
                fill=filled_texels,
                report=dict(opaque_texels=int(opaque.sum()), lost_texels=int(gone.sum()),
                            lost_percent=round(100*float(gone.sum())/float(opaque.sum()), 2),
                            filled_texels=len(filled_texels),
                            filled_percent=round(100*len(filled_texels)/float(opaque.sum()), 2),
                            body_share_of_opaque=round(float((body & opaque).sum())/float(opaque.sum()), 3),
                            band_texels=int(strips.sum()), band_transparent_texels=int((strips & ~opaque & ~solid).sum())))


def enclosing(mask, vertices, margin=Fraction(1, 2)):
    """A convex polygon (exact points, texel units, counter-clockwise) with at
    most about `vertices` corners that contains the whole square of every set
    texel and `margin` texels around it (what a sampler at the edge needs),
    inside the texture: the convex hull of those squares, reduced by removing
    again and again the edge whose two neighbours, extended until they meet,
    add the least area; then cut to the texture (which can add corners)."""
    height, width = mask.shape
    points = []
    for y in range(height):
        row = np.nonzero(mask[y])[0]
        if len(row):
            for x in (int(row[0])-margin, int(row[-1])+1+margin):
                points += [(x, y-margin), (x, y+1+margin)]
    poly = [(Fraction(x), Fraction(y)) for x, y in _hull(points)]
    while len(poly) > vertices:
        best = None
        for i in range(len(poly)):
            a, b, c, d = poly[i-1], poly[i], poly[(i+1) % len(poly)], poly[(i+2) % len(poly)]
            r, t = (b[0]-a[0], b[1]-a[1]), (c[0]-d[0], c[1]-d[1])
            det = r[0]*t[1]-r[1]*t[0]
            if det == 0:
                continue
            k = ((d[0]-a[0])*t[1]-(d[1]-a[1])*t[0])/det
            if k <= 1:                                   # the neighbours do not meet beyond this edge
                continue
            meet = (a[0]+k*r[0], a[1]+k*r[1])
            added = sc._cross(b, meet, c)
            if added > 0 and (best is None or added < best[0]):
                best = (added, i, meet)
        if best is None:
            break
        _, i, meet = best
        poly[i] = meet
        del poly[(i+1) % len(poly)]
    for a, b, c in ((-1, 0, 0), (1, 0, width), (0, -1, 0), (0, 1, height)):
        poly = sc._clip(poly, a, b, c)
    return poly


def ring_pieces(outer, inner, pair=True):
    """Triangles, paired into strictly convex quads where they can be, that
    tile a polygon minus a polygon inside it, using only their corners (both
    counter-clockwise, lattice points): a sweep round both that always takes
    the shorter new diagonal. None if no start gives an exact tiling."""
    want = sc._area2(outer)-sc._area2(inner)
    for start in range(len(outer)):
        i, o, done_i, done_o, triangles = 0, start, 0, 0, []
        nearest = min(range(len(inner)), key=lambda k: (inner[k][0]-outer[start][0])**2+(inner[k][1]-outer[start][1])**2)
        i = nearest
        while done_i < len(inner) or done_o < len(outer):
            a, b = inner[i % len(inner)], outer[o % len(outer)]
            options = []
            if done_o < len(outer):
                c = outer[(o+1) % len(outer)]
                if sc._cross(a, b, c) > 0:
                    options.append(((c[0]-a[0])**2+(c[1]-a[1])**2, 'o', (a, b, c)))
            if done_i < len(inner):
                c = inner[(i+1) % len(inner)]
                if sc._cross(a, b, c) > 0:
                    options.append(((c[0]-b[0])**2+(c[1]-b[1])**2, 'i', (a, b, c)))
            if not options:
                triangles = None
                break
            _, which, triangle = min(options)
            triangles.append(list(triangle))
            if which == 'o':
                o, done_o = o+1, done_o+1
            else:
                i, done_i = i+1, done_i+1
        if triangles and sum(sc._cross(*t) for t in triangles) == want:
            return sc._quads(triangles) if pair else triangles
    return None


def design_cone(image, points=1, skirt=True, hole=64, lost=8.0, clear=1.0, area=0.1, thin=5, least=48,
                colour='dark', inverted=False, steps=(8, 4, 2, 1), reach=2, trunk_overlap=8, outline=None, curve=None,
                sampler=None, ring='quads', close=0):
    """A pine as a cone with sawtooth sides (kind `bands` in the specification):

    body   one apex vertex just inside the real tip, widening along two inner
           lines of `points` points each to the base: a triangle at the apex,
           then a quad per further point. HARD rule: every texel the body
           touches, and each of its 8 neighbours, is opaque once the enclosed
           holes of at most `hole` texels are filled; so nothing is filled at
           the silhouette. Largest such body (local search).
    bands  per side one punch-through strip from an outer apex vertex at the
           real tip (shared by both sides) along `points` outer points: its
           first quad has the two apex vertices as one short edge. Fitted
           after the body: lost*(foliage texels outside everything)
           + clear*(transparent texels in the bands) + area*(band texels).
    skirt  a punch-through quad across the wide end, if asked for.
    trunk  opaque rectangle, as for design_bands (its ring of neighbours is
           filled so the sampler finds no hole beside it).
    The tip must be at the low-u end (the image is mirrored for the fit when
    it is at the other).

    outline=k: no fitted outer lines and no skirt; the punch-through region
    is an enclosing polygon of about k corners (enclosing(): every opaque
    crown texel with half a texel around it, so NO foliage is lost) minus
    the body, tiled by ring_pieces(). curve: corner counts to tabulate in
    the report (band texels and transparent texels per count).

    sampler=r (texels): the body's hard rule becomes "every texel within r of
    the body is opaque" instead of "every touched texel and its 8 neighbours":
    0.5 is what a bilinear sample at the body's edge can reach (a point
    sample reaches nothing beyond the texel it is in), so the body may come
    half a texel closer to a notch than under the neighbour rule. The search
    then also runs its step sizes twice. ring='triangles': the ring's pieces
    stay triangles in sweep order (consecutive ones share an edge, so they
    chain into strips wherever the sweep alternates sides) instead of being
    paired into quads. close=n: slits that n closings with the 3x3 square
    close (one texel wide for n=1) count as solid for the body too, but only
    the part of a slit within the body's reach is filled: where it opens to
    the silhouette it stays a slit."""
    if sampler is not None:
        steps = tuple(steps)+tuple(steps[1:])
    opaque = image != 15
    height, width = image.shape
    columns = np.nonzero(opaque.any(0))[0]
    spans = opaque.sum(0)
    if spans[columns[0]:columns[0]+8].sum() > spans[columns[-1]-7:columns[-1]+1].sum()*4:
        mirrored = design_cone(image[:, ::-1], points, skirt, hole, lost, clear, area, thin, least, colour, inverted, steps, reach,
                               trunk_overlap, outline, curve, sampler, ring, close)
        flip = lambda p: [8*width-p[0], p[1]]
        mirrored['quads'] = [dict(list=q['list'], corners=[flip(p) for p in q['corners']][::-1]) for q in mirrored['quads']]
        mirrored['polygons'] = [[flip(p) for p in poly][::-1] for poly in mirrored['polygons']]
        mirrored['fill'] = [(width-1-x, y, v) for x, y, v in mirrored['fill']]
        return mirrored
    ys, xs = np.mgrid[0:height, 0:width]
    cx, cy = xs+0.5, ys+0.5
    pinholes = holes(image, hole, 0)
    slits = holes(image, hole, close) & ~pinholes if close else np.zeros_like(opaque)
    solid = opaque | pinholes | slits
    padded = np.pad(solid, 1)
    safe = np.ones_like(solid)
    for dy in range(3):
        for dx in range(3):
            safe &= padded[dy:dy+height, dx:dx+width]
    body_mask = opaque.copy()
    for _ in range(thin):
        body_mask = _erode(body_mask, (False, False))
    for _ in range(thin):
        body_mask = _neighbours(body_mask, (False, False))
    body_mask &= opaque
    main = np.zeros_like(opaque)
    for y, x in max(_components(body_mask), key=len):
        main[y, x] = True
    grown = main
    for _ in range(3):
        grown = _neighbours(grown, (False, False))
    bars, crown = [], opaque.copy()
    for members in _components(opaque & ~grown):
        y0, y1 = min(m[0] for m in members), max(m[0] for m in members)+1
        x0, x1 = min(m[1] for m in members), max(m[1] for m in members)+1
        if len(members) < least or len(members) < 0.6*(y1-y0)*(x1-x0) or x1-x0 < 3*(y1-y0):
            continue
        rows = np.nonzero(opaque[y0:y1, x0:x1].mean(1) >= 0.5)[0]
        bars.append([x0, y0+int(rows[0]), x1, y0+int(rows[-1])+1])
        crown[y0:y1, x0:x1] = False
    columns = np.nonzero(crown.any(0))[0]
    u0, u1 = int(columns[0]), int(columns[-1])+1
    axis = int(round(float((cy*crown).sum()/crown.sum())))
    tip = int(np.median(np.nonzero(crown[:, u0:u0+3].any(1))[0]))

    def inside(triangle):
        return _inside(list(triangle), cx, cy)

    def convex(ring):
        return all(sc._cross(ring[i-1], ring[i], ring[(i+1) % len(ring)]) >= 4 for i in range(len(ring)))

    def body_rings(apex, top, bottom):
        return [[apex, top[0], bottom[0]]]+[[top[i], top[i+1], bottom[i+1], bottom[i]] for i in range(points-1)]

    def body_ok(apex, top, bottom):
        line = [apex]+top
        if any(b[0]-a[0] < 2 for a, b in zip(line, line[1:])) or any(b[0]-a[0] < 2 for a, b in zip([apex]+bottom, bottom)):
            return False
        for ring in body_rings(apex, top, bottom):
            if not convex(ring) or any(not 0 <= p[0] <= width or not 0 <= p[1] <= height for p in ring):
                return False
            for i in range(1, len(ring)-1):
                if sampler is None and (_touched([ring[0], ring[i], ring[i+1]], image.shape, 0.125) & ~safe).any():
                    return False
                if sampler is not None and (_touched([ring[0], ring[i], ring[i+1]], image.shape, sampler) & ~solid).any():
                    return False
        return True

    def body_area(apex, top, bottom):
        return float(_triangles(inside, [(r[0], r[i], r[i+1]) for r in body_rings(apex, top, bottom) for i in range(1, len(r)-1)]).sum())
    # A sliver along the axis to start from, wherever one fits.
    start = None
    for lead in range(4, (u1-u0)//2, 4):
        for centre in sorted(range(height), key=lambda v: abs(v-axis)):
            apex = (u0+lead, centre)
            us = [u0+lead+(u1-lead-6-u0-lead)*(i+1)//points for i in range(points)]
            top, bottom = [(u, centre-3) for u in us], [(u, centre+3) for u in us]
            if body_ok(apex, top, bottom):
                start = (apex, top, bottom)
                break
            if abs(centre-axis) > 12:
                break
        if start:
            break
    if not start:
        raise ValueError('no solid axis to start the body from')
    apex, top, bottom = start
    best = body_area(apex, top, bottom)
    for step in steps:
        improved = True
        while improved:
            improved = False
            for which in range(1+2*points):
                for du in range(-reach, reach+1):
                    for dv in range(-reach, reach+1):
                        if not du and not dv:
                            continue
                        a, t, b = apex, list(top), list(bottom)
                        if which == 0:
                            a = (apex[0]+du*step, apex[1]+dv*step)
                        elif which <= points:
                            t[which-1] = (t[which-1][0]+du*step, t[which-1][1]+dv*step)
                        else:
                            b[which-1-points] = (b[which-1-points][0]+du*step, b[which-1-points][1]+dv*step)
                        if not body_ok(a, t, b):
                            continue
                        value = body_area(a, t, b)
                        if value > best:
                            best, apex, top, bottom, improved = value, a, t, b, True
    body = _triangles(inside, [(r[0], r[i], r[i+1]) for r in body_rings(apex, top, bottom) for i in range(1, len(r)-1)])

    tiling = None
    if outline:
        inner = [(8*p[0], 8*p[1]) for p in [apex]+top+bottom[::-1]]
        table = []
        asked = sorted(set(curve or [])|{outline})
        for count in asked+[outline+extra for extra in range(1, 5)]:
            if count not in asked and tiling is not None:
                break
            exact = enclosing(crown, count)
            shape = _hull([(int(round(8*x)), int(round(8*y))) for x, y in exact])
            hull_mask = _inside([(x/8, y/8) for x, y in shape], cx, cy)
            escaped = int((crown & ~hull_mask).sum())
            band_mask = hull_mask & ~body
            table.append(dict(asked=count, corners=len(shape), band_texels=int(band_mask.sum()),
                              band_transparent_texels=int((band_mask & ~opaque).sum()), lost_texels=escaped))
            if (count == outline or count not in asked) and tiling is None and not escaped:
                # (a corner count whose outline cannot be tiled round the body, because the two touch, is passed over)
                tiling, outer_shape, ring_mask = ring_pieces(shape, inner, ring == 'quads'), shape, band_mask
                if tiling is None:
                    # The sweep found no exact tiling (a body with a waist): the shortest-edge
                    # triangulation round the body's convex pieces, which has no strip order.
                    parts = [[(8*p[0], 8*p[1]) for p in r] for r in body_rings(apex, top, bottom)]
                    tiling = sc._tile(shape, parts)
                    if tiling is not None and ring == 'quads':
                        tiling = sc._quads(tiling)
        if tiling is None:
            raise ValueError('the enclosing outline cannot be tiled round the body, or loses a texel')

    def band_rings(outer_apex, outer_top, outer_bottom):
        """Top and bottom quads from the apex to the base, then the skirt (positive rings)."""
        it, ib, ot, ob = [apex]+top, [apex]+bottom, [outer_apex]+outer_top, [outer_apex]+outer_bottom
        rings = [[ot[i], ot[i+1], it[i+1], it[i]] for i in range(points)]
        rings += [[ib[i], ib[i+1], ob[i+1], ob[i]] for i in range(points)]
        if skirt:
            rings.append([it[-1], ot[-1], ob[-1], ib[-1]])
        return rings

    def band_ok(rings):
        return all(convex(r) and all(0 <= p[0] <= width and 0 <= p[1] <= height for p in r) for r in rings)

    def band_score(rings):
        strips = _triangles(inside, [(r[0], r[i], r[i+1]) for r in rings for i in range(1, len(r)-1)]) & ~body
        return lost*float((crown & ~body & ~strips).sum())+clear*float((strips & ~opaque).sum())+area*float(strips.sum())
    if tiling is not None:
        steps = ()
    outer_apex = (max(u0-1, 0), tip if tip != apex[1] else tip)
    # Start on the outermost foliage between the neighbouring points; the search then pulls the lines in.
    def reach_of(line, i, upward):
        low = (apex[0] if i == 0 else line[i-1][0])
        high = line[i+1][0] if i+1 < points else u1
        rows = np.nonzero(crown[:, max(low, u0):max(high, low+1)].any(1))[0]
        return int(rows[0]) if upward else int(rows[-1])+1
    outer_top = [(min(p[0]+(3 if i == points-1 else 0), width), min(reach_of(top, i, True), p[1]-2)) for i, p in enumerate(top)]
    outer_bottom = [(min(p[0]+(3 if i == points-1 else 0), width), max(reach_of(bottom, i, False), p[1]+2)) for i, p in enumerate(bottom)]
    if not band_ok(band_rings(outer_apex, outer_top, outer_bottom)):
        outer_top = [(min(p[0]+(3 if i == points-1 else 0), width), max(p[1]-6, 0)) for i, p in enumerate(top)]
        outer_bottom = [(min(p[0]+(3 if i == points-1 else 0), width), min(p[1]+6, height)) for i, p in enumerate(bottom)]
    if tiling is None and not band_ok(band_rings(outer_apex, outer_top, outer_bottom)):
        raise ValueError('no room for the bands around the body')
    best = band_score(band_rings(outer_apex, outer_top, outer_bottom)) if tiling is None else 0.0
    for step in steps:
        improved = True
        while improved:
            improved = False
            for which in range(1+2*points):
                for du in range(-reach, reach+1):
                    for dv in range(-reach, reach+1):
                        if not du and not dv:
                            continue
                        a, t, b = outer_apex, list(outer_top), list(outer_bottom)
                        if which == 0:
                            a = (a[0]+du*step, a[1]+dv*step)
                        elif which <= points:
                            t[which-1] = (t[which-1][0]+du*step, t[which-1][1]+dv*step)
                        else:
                            b[which-1-points] = (b[which-1-points][0]+du*step, b[which-1-points][1]+dv*step)
                        rings = band_rings(a, t, b)
                        if not band_ok(rings):
                            continue
                        value = band_score(rings)
                        if value < best:
                            best, outer_apex, outer_top, outer_bottom, improved = value, a, t, b, True
    it, ib, ot, ob = [apex]+top, [apex]+bottom, [outer_apex]+outer_top, [outer_apex]+outer_bottom
    # Corner orders that chain (see design_bands): c1-c2 the edge towards the apex, c3-c0 the edge towards the base.
    quads = [dict(list='M', corners=[it[i+1], it[i], ot[i], ot[i+1]]) for i in range(points)]
    if skirt:
        quads.append(dict(list='M', corners=[it[-1], ot[-1], ob[-1], ib[-1]]))
    quads += [dict(list='M', corners=[ob[i+1], ob[i], ib[i], ib[i+1]]) for i in range(points)]
    if tiling is not None:
        quads = [dict(list='M', corners=[(Fraction(x, 8), Fraction(y, 8)) for x, y in piece]) for piece in tiling]
    quads.append(dict(list='O', corners=[bottom[0], apex, top[0]]))
    quads += [dict(list='O', corners=[bottom[i+1], bottom[i], top[i], top[i+1]]) for i in range(points-1)]
    polygons, trunks = [], []
    for x0, y0, x1, y1 in bars:
        while x0 > 0 and not body[y0:y1, x0].all() and body[y0:y1, :x0].any():
            x0 -= 1
        # ... and on under the crown (the body is drawn over it), as far as the texels there are safe,
        # so that no straight end of the trunk can show where the skirt's texels are holes.
        under = 0
        while under < trunk_overlap and x0 > 1 and safe[y0:y1, x0-1].all():
            x0, under = x0-1, under+1
        polygons.append([(int(x0), int(y0)), (int(x1), int(y0)), (int(x1), int(y1)), (int(x0), int(y1))])
        trunks.append([int(8*x0+8*under), int(8*y0), int(8*x1), int(8*y1)])
    touched = np.zeros(image.shape, bool)
    for poly in polygons:
        touched |= _touched(poly, image.shape, 0.125)
    ring = _neighbours(touched, (True, True)) & ~opaque
    under = np.zeros(image.shape, bool)
    if close:
        for r in body_rings(apex, top, bottom):
            for i in range(1, len(r)-1):
                under |= _touched([r[0], r[i], r[i+1]], image.shape, sampler if sampler is not None else 1.0)
        if sampler is None:
            under = _neighbours(under, (False, False))
    slits = slits & under
    fill = _colour(image, pinholes | slits | ring, colour, inverted, (True, True))
    strips = _triangles(inside, [(r[0], r[i], r[i+1]) for r in band_rings(outer_apex, outer_top, outer_bottom)
                                 for i in range(1, len(r)-1)]) & ~body
    if tiling is not None:
        strips = ring_mask
    drawn = body | strips
    for poly in polygons:
        drawn |= _inside(poly, cx, cy)
    gone = opaque & ~drawn
    scale = lambda p: [int(8*p[0]), int(8*p[1])]
    extra = dict(outline=[list(p) for p in outer_shape], curve=table) if tiling is not None else {}
    return dict(kind='bands', polygons=[[scale(p) for p in poly] for poly in polygons], bands=[], **extra,
                quads=[dict(list=q['list'], corners=[scale(p) for p in q['corners']]) for q in quads],
                lines=dict(apex=apex, inner_top=top, inner_bottom=bottom, outer_apex=outer_apex, outer_top=outer_top,
                           outer_bottom=outer_bottom),
                trunks=trunks,       # per trunk rectangle: the part outside the crown [u0, v0, u1, v1], ground at u1
                fill=fill,
                report=dict(opaque_texels=int(opaque.sum()), lost_texels=int(gone.sum()),
                            lost_percent=round(100*float(gone.sum())/float(opaque.sum()), 2),
                            filled_pinhole_texels=int(pinholes.sum()), filled_slit_texels_under_the_body=int(slits.sum()),
                            filled_beside_trunk_texels=int(ring.sum()),
                            body_share_of_opaque=round(float((body & opaque).sum())/float(opaque.sum()), 3),
                            body_transparent_texels=int((body & ~solid).sum()),
                            band_texels=int(strips.sum()), band_transparent_texels=int((strips & ~opaque).sum()),
                            band_transparent_share=round(float((strips & ~opaque).sum())/max(float(strips.sum()), 1.0), 3)))


# ---- tilings that leave as strips ---------------------------------------------

def _on_segment(p, a, b):
    """p strictly between a and b on their line."""
    return sc._cross(a, b, p) == 0 and (p[0]-a[0])*(p[0]-b[0])+(p[1]-a[1])*(p[1]-b[1]) < 0


def _proper(p, q, r, s):
    """Open segments p-q and r-s cross in one point inside both."""
    d1, d2 = sc._cross(p, q, r), sc._cross(p, q, s)
    d3, d4 = sc._cross(r, s, p), sc._cross(r, s, q)
    return ((d1 > 0 > d2) or (d2 > 0 > d1)) and ((d3 > 0 > d4) or (d4 > 0 > d3))


def _within(poly, point):
    """1 inside, 0 on the boundary, -1 outside a simple polygon (exact)."""
    inside = False
    for a, b in zip(poly, poly[1:]+poly[:1]):
        if point == a or _on_segment(point, a, b):
            return 0
        if (a[1] > point[1]) != (b[1] > point[1]):
            side = sc._cross(a, b, point)
            if (side > 0) == (b[1] > a[1]):
                inside = not inside
    return 1 if inside else -1


class _Region:
    """A simple polygon (positive order) minus a simple polygon inside it
    (which may share parts of its boundary): what a triangle of its own
    corners may be. Points are doubled so mid points stay integers."""

    def __init__(self, outer, inner=None):
        loops = [[tuple(p) for p in outer]]+([[tuple(p) for p in inner]] if inner else [])
        for loop in loops:
            if sc._area2(loop) <= 0:
                raise ValueError('a region loop is not in positive order')
        # A corner of one loop on an edge of the other becomes a corner of both: no T-junction.
        for k, loop in enumerate(loops):
            others = [p for j, other in enumerate(loops) if j != k for p in other]
            refined = []
            for a, b in zip(loop, loop[1:]+loop[:1]):
                refined.append(a)
                between = [p for p in set(others) if _on_segment(p, a, b)]
                refined += sorted(between, key=lambda p: (p[0]-a[0])**2+(p[1]-a[1])**2)
            loops[k] = refined
        self.loops = loops
        count = Counter()
        for loop in loops:
            for a, b in zip(loop, loop[1:]+loop[:1]):
                count[frozenset((a, b))] += 1
        self.all_edges = [tuple(e) for e in count]
        self.border = {e for e, n in count.items() if n == 1}
        self.points = sorted({p for loop in loops for p in loop})
        self.area2 = sc._area2(loops[0])-(sc._area2(loops[1]) if inner else 0)
        self._diagonal, self._triangle = {}, {}

    def holds(self, point, scale):
        """A point given times `scale` lies strictly inside the region."""
        outer = [(scale*x, scale*y) for x, y in self.loops[0]]
        if _within(outer, point) != 1:
            return False
        return len(self.loops) == 1 or _within([(scale*x, scale*y) for x, y in self.loops[1]], point) == -1

    def diagonal(self, a, b):
        key = frozenset((a, b))
        if key not in self._diagonal:
            ok = key not in self.border and a != b
            ok = ok and not any(_proper(a, b, r, s) for r, s in self.all_edges)
            ok = ok and not any(_on_segment(p, a, b) for p in self.points)
            ok = ok and self.holds((a[0]+b[0], a[1]+b[1]), 2)
            self._diagonal[key] = ok
        return self._diagonal[key]

    def edge(self, a, b):
        return frozenset((a, b)) in self.border or self.diagonal(a, b)

    def triangle(self, a, b, c):
        """a, b, c in positive order is a triangle of the region with no other corner in or on it."""
        key = (a, b, c)
        if key not in self._triangle:
            ok = sc._cross(a, b, c) > 0 and self.edge(a, b) and self.edge(b, c) and self.edge(c, a)
            ok = ok and self.holds((a[0]+b[0]+c[0], a[1]+b[1]+c[1]), 3)
            ok = ok and not any(p not in key and sc._cross(a, b, p) >= 0 and sc._cross(b, c, p) >= 0 and sc._cross(c, a, p) >= 0
                                for p in self.points)
            self._triangle[key] = ok
        return self._triangle[key]


def strips_of(triangles):
    """Triangle strips over triangles given in positive order, greedily the
    longest first (as prepare_face_order chains faces): [corner list]; the
    first triangle of a strip is corners 0, 1, 2 in positive order, the next
    turns the other way, as on the PowerVR."""
    left = {tuple(t) for t in triangles}
    out = []
    while left:
        directed = {}
        for t in left:
            for i in range(3):
                directed[t[i], t[(i+1) % 3]] = (t, t[(i+2) % 3])
        best = None
        for t in sorted(left):
            for i in range(3):
                strip, used = [t[i], t[(i+1) % 3], t[(i+2) % 3]], {t}
                while True:
                    # (an odd triangle of a strip holds its first edge the other way round)
                    step = directed.get((strip[-1], strip[-2])) if len(strip) % 2 else directed.get((strip[-2], strip[-1]))
                    if step is None or step[0] in used:
                        break
                    used.add(step[0])
                    strip.append(step[1])
                if best is None or len(strip) > len(best[0]):
                    best = (strip, used)
        out.append(best[0])
        left -= best[1]
    return out


def _strip_check(strip, triangles):
    """The triangles of a strip, in positive order."""
    out = []
    for j in range(len(strip)-2):
        a, b, c = strip[j:j+3]
        out.append((a, b, c) if j % 2 == 0 else (b, a, c))
    return out


def strip_faces(strip):
    """Faces (corner lists) of a strip for the pack: quads of two consecutive
    triangles where they form a strictly convex quad, corners c0..c3 with the
    fan (c0,c1,c2),(c0,c2,c3) such that c1-c2 is the edge towards the
    strip's start and c3-c0 the edge towards its end; triangles otherwise.
    An odd triangle is the strip's last face, so every quad starts on an even
    triangle and the renderer may begin a run at any of them."""
    faces, j = [], 0
    while j+2 < len(strip):
        a, b, c = strip[j:j+3]
        if j+3 < len(strip):
            d = strip[j+3]
            ring = [c, a, b, d]
            if all(sc._cross(ring[i-1], ring[i], ring[(i+1) % 4]) > 0 for i in range(4)):
                faces.append(ring)
                j += 2
                continue
            faces += [[a, b, c], [c, b, d]]
            j += 2
            continue
        faces.append([a, b, c] if j % 2 == 0 else [b, a, c])
        j += 1
    return faces


def _sweep(region):
    """Ring tilings in which every triangle has corners on both loops, as a
    walk round the ring from one cross edge: the walk with the fewest strips
    (then the shortest cross edges) by dynamic programming over (inner
    corners passed, outer corners passed, state of the current strip). A
    strip may hold two equal steps in a row only at its start or its end.
    Returns the triangles in walk order, or None."""
    outer, inner = region.loops
    m, k = len(inner), len(outer)
    best = None
    for i0 in range(m):
        for o0 in range(k):
            if not region.diagonal(inner[i0], outer[o0]):
                continue
            # state: (strip length 1/2/3, last step, last two steps equal)
            table = {(0, 0, None): ((0, 0), None)}
            for total in range(m+k):
                for (di, do, state), (cost, back) in sorted(((key, value) for key, value in table.items()
                                                             if key[0]+key[1] == total), key=lambda item: repr(item[0])):
                    a, b = inner[(i0+di) % m], outer[(o0+do) % k]
                    steps = []
                    if do < k:
                        c = outer[(o0+do+1) % k]
                        if region.triangle(b, c, a):
                            steps.append(('O', di, do+1, (b, c, a), (c[0]-a[0])**2+(c[1]-a[1])**2))
                    if di < m:
                        c = inner[(i0+di+1) % m]
                        if region.triangle(c, a, b):
                            steps.append(('I', di+1, do, (c, a, b), (c[0]-b[0])**2+(c[1]-b[1])**2))
                    for step, ni, no, triangle, length in steps:
                        options = [((1, step, False), 1)]
                        if state is not None and (state[0] < 3 or not state[2]):
                            options.append(((min(state[0]+1, 3), step, step == state[1]), 0))
                        for new, more in options:
                            value = (cost[0]+more, cost[1]+length)
                            key = (ni, no, new)
                            if key not in table or value < table[key][0]:
                                table[key] = (value, ((di, do, state), triangle))
            ends = [(value, key) for key, (value, _) in table.items() if key[0] == m and key[1] == k]
            if not ends:
                continue
            value, key = min(ends, key=lambda item: (item[0], repr(item[1])))
            if best is None or value < best[0]:
                triangles = []
                while table[key][1] is not None:
                    key, triangle = table[key][1]
                    triangles.append(triangle)
                best = (value, triangles[::-1])
    return best[1] if best else None


def _greedy_triangles(region, rng):
    """A triangulation of the region by its own corners: valid diagonals in
    random order (short ones tend to come first), each kept unless it
    crosses one kept before; then the triangles of the edge set."""
    points = region.points
    pairs = [(a, b) for i, a in enumerate(points) for b in points[i+1:] if region.diagonal(a, b)]
    pairs.sort(key=lambda e: ((e[0][0]-e[1][0])**2+(e[0][1]-e[1][1])**2)*rng.uniform(0.3, 3.0))
    kept = []
    for a, b in pairs:
        if not any(_proper(a, b, r, s) for r, s in kept):
            kept.append((a, b))
    near = defaultdict(set)
    for a, b in kept+[tuple(e) for e in region.border]:
        near[a].add(b)
        near[b].add(a)
    triangles = []
    for a in points:
        for b in near[a]:
            if b <= a:
                continue
            for c in near[a] & near[b]:
                if c <= b:
                    continue
                t = (a, b, c) if sc._cross(a, b, c) > 0 else (a, c, b)
                if region.triangle(*t):
                    triangles.append(t)
    return triangles if sum(sc._cross(*t) for t in triangles) == region.area2 else None


def _flip(region, triangles, rng, rounds=4):
    """Fewer strips by turning diagonals: every shared edge of two triangles
    that form a strictly convex quad is tried; kept when the strips do not
    increase and the number of strips falls at least once per round."""
    best, count = list(triangles), len(strips_of(triangles))
    for _ in range(rounds):
        improved = False
        edges = defaultdict(list)
        for t in best:
            for i in range(3):
                edges[frozenset((t[i], t[(i+1) % 3]))].append(t)
        shared = sorted((sorted(e), pair) for e, pair in edges.items() if len(pair) == 2)
        rng.shuffle(shared)
        for (a, b), (t1, t2) in shared:
            if t1 not in best or t2 not in best:
                continue
            c = next(p for p in t1 if p not in (a, b))
            d = next(p for p in t2 if p not in (a, b))
            if sc._cross(a, b, c) < 0:
                a, b = b, a                               # c left of a->b, d right of it
            new = [(c, a, d), (c, d, b)]
            if not all(region.triangle(*t) for t in new):
                continue
            trial = [t for t in best if t not in (t1, t2)]+new
            value = len(strips_of(trial))
            if value < count:
                best, count, improved = trial, value, True
        if not improved:
            break
    return best


def tile_strips(outer, inner=None, tries=60, seed=1):
    """Triangle strips that tile a simple polygon, minus a simple polygon
    inside it, by their own corners alone (lattice points, positive order;
    the inner polygon may lie against the outer one, as a body cut by its
    face does): the walk round the ring where one exists, and `tries`
    triangulations from diagonals in random order, each improved by turning
    diagonals; the tiling with the fewest strips. Every tiling is exact: its
    doubled area is the region's. Returns (strips, triangles) or None."""
    import random
    region = _Region(outer, inner)
    rng = random.Random(seed)
    candidates = []
    if inner and not (set(region.loops[0]) & set(region.loops[1])):
        swept = _sweep(region)
        if swept and sum(sc._cross(*t) for t in swept) == region.area2:
            candidates.append(swept)
    for _ in range(tries):
        triangles = _greedy_triangles(region, rng)
        if triangles:
            candidates.append(triangles)
    best = None
    for index, triangles in enumerate(candidates):
        if index:
            triangles = _flip(region, triangles, rng)
        strips = strips_of(triangles)
        longest = sum((p[0]-q[0])**2+(p[1]-q[1])**2 for t in triangles for p, q in zip(t, t[1:]+t[:1]))
        if best is None or (len(strips), longest) < best[0]:
            best = ((len(strips), longest), strips, triangles)
    if best is None:
        return None
    return best[1], best[2]


# ---- pines: body strip, enclosing outline (convex or not), ring strips ----------

def _crown(image, thin=5, least=48, whole=True):
    """(crown mask, trunk bars [x0, y0, x1, y1]) of a pine lying along u: the
    bars are thin parts standing off the eroded body, as in design_cone.
    whole: the crown is every opaque texel outside the bars' rectangles, so
    stray texels beside a trunk are enclosed with the crown and nothing at
    all is lost (False: the crown ends at the box around each bar's texels,
    as design_cone has it)."""
    opaque = image != 15
    body_mask = opaque.copy()
    for _ in range(thin):
        body_mask = _erode(body_mask, (False, False))
    for _ in range(thin):
        body_mask = _neighbours(body_mask, (False, False))
    body_mask &= opaque
    main = np.zeros_like(opaque)
    for y, x in max(_components(body_mask), key=len):
        main[y, x] = True
    grown = main
    for _ in range(3):
        grown = _neighbours(grown, (False, False))
    bars, crown = [], opaque.copy()
    for members in _components(opaque & ~grown):
        y0, y1 = min(m[0] for m in members), max(m[0] for m in members)+1
        x0, x1 = min(m[1] for m in members), max(m[1] for m in members)+1
        if len(members) < least or len(members) < 0.6*(y1-y0)*(x1-x0) or x1-x0 < 3*(y1-y0):
            continue
        rows = np.nonzero(opaque[y0:y1, x0:x1].mean(1) >= 0.5)[0]
        bars.append([int(x0), int(y0+rows[0]), int(x1), int(y0+rows[-1])+1])
        if whole:
            crown[y0+int(rows[0]):y0+int(rows[-1])+1, x0:x1] = False
        else:
            crown[y0:y1, x0:x1] = False
    return crown, bars


def _strip_positive(strip):
    return [t if sc._cross(*t) > 0 else (t[1], t[0], t[2]) for t in (tuple(strip[j:j+3]) for j in range(len(strip)-2))]


def fit_body(solid, crown, corners, sampler=0.5, steps=(8, 4, 2, 1, 4, 2, 1), reach=2, limit=None, margin=1, starts=3, apex=8):
    """The largest body of `corners` corners that is one triangle strip from
    an apex near the tip: apex, then corners alternately on the two long
    sides, towards the base (texel units, in strip order). Every triangle
    obeys the hard rule: each texel within `sampler` texels of it is set in
    `solid` (sampler None: each touched texel and its 8 neighbours). limit:
    largest u a corner may have. Local search from several starts (both ways
    of dealing the corners to the sides, `starts` spacings of the corners
    along the axis, and the best body of fewer corners with a corner added
    at the base); the largest result with at most `corners` corners. apex:
    the apex lies within that many texels of the crown's first column (the
    body ends in a point at the tip)."""
    height, width = solid.shape
    limit = width-margin if limit is None else min(limit, width-margin)
    ys, xs = np.mgrid[0:height, 0:width]
    cx, cy = xs+0.5, ys+0.5
    if sampler is None:
        padded = np.pad(solid, 1)
        safe = np.ones_like(solid)
        for dy in range(3):
            for dx in range(3):
                safe &= padded[dy:dy+height, dx:dx+width]
    columns = np.nonzero(crown.any(0))[0]
    u0, u1 = int(columns[0]), min(int(columns[-1])+1, limit)
    axis = int(round(float((cy*crown).sum()/crown.sum())))
    memo = {}

    def triangle(a, b, c):
        """Texel centres in a positive triangle that obeys the rule, else -1."""
        key = (a, b, c)
        if key not in memo:
            if sampler is None:
                bad = (_touched([a, b, c], solid.shape, 0.125) & ~safe).any()
            else:
                bad = (_touched([a, b, c], solid.shape, sampler) & ~solid).any()
            memo[key] = -1 if bad else int(_inside([a, b, c], cx, cy).sum())
        return memo[key]

    def value(strip, first):
        """Texel centres in the strip's triangles; -1 if it is no valid body.
        first: +1 when corner 1 is on the low-v side."""
        for side in (strip[1::2], strip[2::2]):
            line = [strip[0]]+side
            if any(b[0]-a[0] < 2 for a, b in zip(line, line[1:])):
                return -1
        if any(not margin <= p[0] <= limit or not margin <= p[1] <= height-margin for p in strip) or strip[0][0] > u0+apex:
            return -1
        total = 0
        for j in range(len(strip)-2):
            a, b, c = strip[j:j+3]
            turn = sc._cross(a, b, c)
            if turn*(first if j % 2 == 0 else -first) < 4:
                return -1
            inside = triangle(a, b, c) if turn > 0 else triangle(b, a, c)
            if inside < 0:
                return -1
            total += inside
        return total

    def climb(strip, first):
        best = value(strip, first)
        for step in steps:
            improved = True
            while improved:
                improved = False
                for which in range(len(strip)):
                    for du in range(-reach, reach+1):
                        for dv in range(-reach, reach+1):
                            if not du and not dv:
                                continue
                            trial = list(strip)
                            trial[which] = (strip[which][0]+du*step, strip[which][1]+dv*step)
                            got = value(trial, first)
                            if got > best:
                                best, strip, improved = got, trial, True
        return best, strip
    results = {}
    for count in range(3, corners+1):
        found = []
        for first in (1, -1):
            seeds = []
            for power in (1.0, 0.6, 1.7)[:starts]:
                for lead in range(4, (u1-u0)//2, 4):
                    seed = None
                    for centre in sorted(range(height), key=lambda v: abs(v-axis)):
                        if abs(centre-axis) > 12:
                            break
                        near = [(count-1)-(count-1)//2, (count-1)//2]
                        strip = [(u0+lead, centre)]
                        for j in range(count-1):
                            side, index = j % 2, j//2
                            share = ((index+1)/near[side])**power
                            strip.append((int(u0+lead+(u1-6-u0-lead)*share)-(1 if side else 0),
                                          centre-3*first if side == 0 else centre+3*first))
                        if value(strip, first) >= 0:
                            seed = strip
                            break
                    if seed:
                        seeds.append(seed)
                        break
            if count > 3 and (count-1, first) in results:
                # The body of one corner less, with a corner added beyond its last edge.
                old = results[count-1, first][1]
                a, b = old[-2], old[-1]
                for du in (2, 4, 8):
                    for dv in (-4, -2, 0, 2, 4):
                        point = ((a[0]+b[0])//2+du+abs(b[0]-a[0])//2, (a[1]+b[1])//2+dv)
                        for trial in (old+[point], old[:-1]+[point, old[-1]]):
                            if value(trial, first) >= 0:
                                seeds.append(trial)
                                break
            for seed in seeds:
                found.append(climb(seed, first))
            if found:
                results[count, first] = max(found)
            found = []
    if not results:
        raise ValueError('no solid axis to start the body from')
    return max(results.values(), key=lambda r: (r[0], -len(r[1])))[1]


def body_ring(strip):
    """The outline of a body strip, positive order, from the apex."""
    first, second = strip[1::2], strip[2::2]
    ring = [strip[0]]+first+second[::-1]
    return ring if sc._area2(ring) > 0 else ring[::-1]


def _envelopes(crown, margin=0.5):
    """Per point of the half-texel grid along u (index g at u = g/2): the
    largest v the upper line of an enclosing outline may have there and the
    smallest v its lower line may have (the crown's texel squares with
    `margin` texels around them, cut to the texture); beside the crown both
    are the tip's (or base's) middle, so the lines can meet in a point."""
    height, width = crown.shape
    top = np.full(2*width+1, np.inf)
    bottom = np.full(2*width+1, -np.inf)
    reach = int(round(2*margin))
    for c in np.nonzero(crown.any(0))[0]:
        rows = np.nonzero(crown[:, c])[0]
        low, high = max(2*int(c)-reach, 0), min(2*int(c)+2+reach, 2*width)
        top[low:high+1] = np.minimum(top[low:high+1], max(float(rows[0])-margin, 0.0))
        bottom[low:high+1] = np.maximum(bottom[low:high+1], min(float(rows[-1])+1+margin, float(height)))
    have = np.nonzero(np.isfinite(top))[0]
    first, last = int(have[0]), int(have[-1])
    for span, at in ((slice(0, first), first), (slice(last+1, None), last)):
        middle = round(4*(top[at]+bottom[at]))/8.0
        top[span], bottom[span] = middle, middle
    return top, bottom, first, last


def _chain_best(limit, left, right):
    """For every grid point g strictly between the chain points left=(g, v)
    and right: the largest v (a multiple of 1/8) such that left-(g, v) and
    (g, v)-right both stay at or below `limit`; -inf where there is none.
    left or right None: no neighbour on that side."""
    count = len(limit)
    out = limit.copy()
    index = np.arange(count, dtype=float)
    with np.errstate(divide='ignore', invalid='ignore'):
        if left is not None:
            gl, vl = left
            slope = np.where(index > gl, (limit-vl)/(index-gl), np.inf)
            slope[:gl+1] = np.inf
            run = np.minimum.accumulate(slope)
            out = np.minimum(out, np.where(index > gl, vl+run*(index-gl), np.inf))
        if right is not None:
            gr, vr = right
            slope = np.where(index < gr, (limit-vr)/(gr-index), np.inf)
            slope[gr:] = np.inf
            run = np.minimum.accumulate(slope[::-1])[::-1]
            out = np.minimum(out, np.where(index < gr, vr+run*(gr-index), np.inf))
    out = np.floor(out*8+1e-7)/8
    if left is not None:
        out[:left[0]+1] = -np.inf
    if right is not None:
        out[right[0]:] = -np.inf
    return out


def _chain_settle(limit, chain, rounds=60, pin=(False, False)):
    """Every point of a chain moved in turn to the place between its
    neighbours (its ends: up or down) that leaves the largest area under the
    chain, while it stays at or below `limit`, until none moves."""
    def local(values, left, right, at=None):
        g = np.arange(len(values), dtype=float) if at is None else np.array([float(at)])
        with np.errstate(invalid='ignore'):
            area = np.zeros(len(g))
            if left is not None:
                area = area+(g-left[0])*(left[1]+values)/2
            if right is not None:
                area = area+(right[0]-g)*(values+right[1])/2
        return np.where(np.isfinite(values), area, -np.inf)
    chain = list(chain)
    moved = True
    while moved and rounds:
        moved, rounds = False, rounds-1
        for i in range(len(chain)):
            if (i == 0 and pin[0]) or (i == len(chain)-1 and pin[1]):
                continue
            left = chain[i-1] if i else None
            right = chain[i+1] if i+1 < len(chain) else None
            values = _chain_best(limit, left, right)
            if i == 0 or i == len(chain)-1:
                keep = np.full(len(limit), -np.inf)
                keep[chain[i][0]] = values[chain[i][0]]
                values = keep
            gain = local(values, left, right)
            now = local(np.array([chain[i][1]]), left, right, chain[i][0])[0]
            g = int(np.argmax(gain))
            if gain[g] > now+1e-9:
                chain[i] = (g, float(values[g]))
                moved = True
    return chain


LEVELS = tuple(range(0, 33))+tuple(range(36, 132, 4))


def _chain_fit(limit, first, last, most, pin=(False, False)):
    """Chains (g strictly rising from grid point first to last, v) that stay
    at or below `limit` with the largest area under them, for 2..most
    points: dynamic programming over candidate points (every second grid
    place; v the limit there less one of LEVELS, or 0), then _chain_settle on the
    full lattice. pin: the first (last) point is the limit's own value
    there, so that the two lines of an outline meet in it. Returns
    {points: chain}."""
    places = sorted(set(range(first, last+1, 2)) | {last})
    where = np.array(places)
    levels = np.array(LEVELS, dtype=float)
    values = limit[where][:, None]-levels[None, :]            # [place, level]
    values[:, -1] = 0.0                                       # the texture's edge is always a candidate
    usable = values >= 0
    count, deep = len(places), len(levels)
    best = [None, np.full((count, deep), -np.inf)]
    best[1][0] = np.where(usable[0], 0.0, -np.inf)
    if pin[0]:
        best[1][0, 1:] = -np.inf
    back = [None, None]
    for points in range(2, most+1):
        table = np.full((count, deep), -np.inf)
        source = np.zeros((count, deep), np.int64)
        for i in range(count-1):
            have = np.isfinite(best[points-1][i])
            if not have.any():
                continue
            v1 = values[i][have]                               # [l1]
            dx = (where[i+1:]-where[i]).astype(float)          # [j]
            reach_all = np.arange(where[i]+1, last+1)
            slope = (limit[None, where[i]+1:last+1]-v1[:, None])/(reach_all-where[i])[None, :]
            run = np.minimum.accumulate(slope, axis=1)[:, where[i+1:]-where[i]-1]      # [l1, j]
            top = v1[:, None]+run*dx[None, :]                  # largest v at each later place
            ok = (values[None, i+1:, :] <= top[:, :, None]+1e-9) & usable[None, i+1:, :]
            gain = best[points-1][i][have][:, None, None]+dx[None, :, None]*(v1[:, None, None]+values[None, i+1:, :])/2
            gain = np.where(ok, gain, -np.inf)
            pick = np.argmax(gain, axis=0)                     # [j, l2]
            got = np.take_along_axis(gain, pick[None], 0)[0]
            better = got > table[i+1:]
            table[i+1:] = np.where(better, got, table[i+1:])
            source[i+1:] = np.where(better, i*deep+np.nonzero(have)[0][pick], source[i+1:])
        best.append(table)
        back.append(source)
    out = {}
    for points in range(2, most+1):
        if not np.isfinite(best[points][-1]).any():
            continue
        level = 0 if pin[1] else int(np.argmax(best[points][-1]))
        if not np.isfinite(best[points][-1][level]):
            continue
        chain, at = [], (count-1)*deep+level
        for n in range(points, 0, -1):
            i, l = divmod(at, deep)
            chain.append((int(where[i]), float(values[i, l])))
            if n > 1:
                at = int(back[n][i, l])
        out[points] = _chain_settle(limit, chain[::-1], pin=pin)
    return out


def enclosing_free(crown, vertices, margin=0.5, reach=6):
    """An enclosing outline that need not be convex: a simple polygon of at
    most `vertices` corners (lattice points, positive order) that holds the
    whole square of every crown texel and `margin` texels around it, inside
    the texture, with little area. The crown lies along u, so the outline is
    an upper and a lower line over u (each may bend both ways, _chain_fit)
    joined at each end in one point, where the texture has room for it, or
    by an edge. All ways of dealing the corners to the two lines and three
    places of each end (at the crown's end and `reach`/2 and `reach` texels
    beyond it) are tried; the smallest area wins. Returns ({corners: polygon}, {corners: area in texels}) for every
    corner count up to `vertices` that was reached."""
    height, width = crown.shape
    top, bottom, first, last = _envelopes(crown, margin)
    results = {}
    ends = []
    for back in (0, reach, 2*reach):
        for beyond in (0, reach, 2*reach):
            pair = (max(first-back, 0), min(last+beyond, 2*width))
            if pair not in ends:
                ends.append(pair)
    for start, end in ends:
        pin = (start < first, end > last)
        upper = _chain_fit(top, start, end, vertices-2, pin)
        lower = _chain_fit(height-bottom, start, end, vertices-2, pin)
        for a, up in upper.items():
            for b, low in lower.items():
                low = [(g, height-v) for g, v in low]
                if up[0][1] > low[0][1] or up[-1][1] > low[-1][1] or a+b-2 > vertices:
                    continue
                poly = [(4*g, int(round(8*v))) for g, v in up]+[(4*g, int(round(8*v))) for g, v in low[::-1]]
                clean = []
                for p in poly:
                    if not clean or clean[-1] != p:
                        clean.append(p)
                if clean[0] == clean[-1]:
                    clean.pop()
                clean = _straighten(clean)
                if len(clean) < 3 or len(clean) > vertices or sc._area2(clean) <= 0:
                    continue
                area = sc._area2(clean)/128.0
                if len(clean) not in results or area < results[len(clean)][0]:
                    results[len(clean)] = (area, clean)
    # More corners never cost area: a count takes the best of the smaller ones too.
    polygons, areas, best = {}, {}, None
    for count in sorted(results):
        if best is None or results[count][0] < best[0]:
            best = results[count]
        polygons[count], areas[count] = best[1], best[0]
    return polygons, areas


def _straighten(poly):
    """Without corners on the straight line between their neighbours."""
    poly = list(poly)
    changed = True
    while changed and len(poly) > 3:
        changed = False
        for i in range(len(poly)):
            if sc._cross(poly[i-1], poly[i], poly[(i+1) % len(poly)]) == 0:
                del poly[i]
                changed = True
                break
    return poly


def encloses(poly, crown, margin=0.5):
    """Every crown texel's square with `margin` around it (cut to the
    texture) lies in the polygon (lattice points): number of texels that do
    not. Exact for a polygon whose lines run over u (enclosing_free) and for
    convex ones: the four corners of each grown square are tested."""
    height, width = crown.shape
    bad = 0
    edges = list(zip(poly, poly[1:]+poly[:1]))
    for y, x in zip(*np.nonzero(crown & ~_erode(crown, (False, False)))):
        box = (max(8*x-int(8*margin), 0), max(8*y-int(8*margin), 0), min(8*x+8+int(8*margin), 8*width), min(8*y+8+int(8*margin), 8*height))
        corners = [(box[0], box[1]), (box[2], box[1]), (box[2], box[3]), (box[0], box[3])]
        if any(_within(poly, (int(p[0]), int(p[1]))) < 0 for p in corners) or \
                any(_proper(corners[i], corners[(i+1) % 4], a, b) for i in range(4) for a, b in edges):
            bad += 1
    return bad


def _cut_to(poly, footprint, grow):
    """A simple polygon cut to a convex footprint (both lattice points,
    positive order), every new corner a lattice point ON the footprint's
    edge, the next one outwards along the edge (grow: the polygon gets no
    smaller), or, to shrink, the nearest lattice point in the footprint that
    is not outside the polygon's crossing edge (on the footprint's edge
    where that has lattice points close by, as an edge along an axis has;
    a sliver beside a slanted edge is then left to whatever is tiled round
    the polygon). None if nothing is left."""
    planes = [sc._plane(p, q) for p, q in zip(footprint, footprint[1:]+footprint[:1])]
    for start, end in zip(footprint, footprint[1:]+footprint[:1]):
        a, b, c = sc._plane(start, end)
        along = (end[0]-start[0], end[1]-start[1])
        count = math.gcd(abs(along[0]), abs(along[1]))
        step = (along[0]//count, along[1]//count)
        clipped = []
        for p, q in zip(poly, poly[1:]+poly[:1]):
            dp, dq = a*p[0]+b*p[1]-c, a*q[0]+b*q[1]-c
            if dp <= 0:
                clipped.append(p)
            if (dp < 0 < dq) or (dq < 0 < dp):
                t = Fraction(dp, dp-dq)
                x, y = p[0]+(q[0]-p[0])*t, p[1]+(q[1]-p[1])*t
                at = ((x-start[0])*step[0]+(y-start[1])*step[1])/Fraction(step[0]**2+step[1]**2)
                # Leaving the footprint here, the polygon runs on along the edge to where it comes back in.
                down = (dp < 0) == bool(grow)
                j = math.floor(at) if down else math.ceil(at)     # (may lie beyond the edge's end: the next edge cuts there)
                point = (start[0]+j*step[0], start[1]+j*step[1])
                if grow is not True:
                    # (grow 'near': the nearest lattice point in the footprint not inside the crossing edge,
                    # for an edge without lattice points close by; a sliver beside that edge is then not covered)
                    turn = 1 if not grow else -1
                    near = sorted({(fx, fy) for fx in (math.floor(x), math.ceil(x)) for fy in (math.floor(y), math.ceil(y))},
                                  key=lambda r: ((r[0]-x)**2+(r[1]-y)**2, r))
                    near = [r for r in near if all(pa*r[0]+pb*r[1] <= pc for pa, pb, pc in planes)]
                    sided = [r for r in near if sc._cross(p, q, r)*turn >= 0]
                    if sided or (near and count == 1):
                        point = (sided or near)[0]
                clipped.append(point)
        poly = []
        for p in clipped:
            if not poly or poly[-1] != p:
                poly.append(p)
        if len(poly) > 1 and poly[0] == poly[-1]:
            poly.pop()
        poly = _straighten(poly) if len(poly) >= 3 else []
        if len(poly) < 3 or sc._area2(poly) <= 0:
            return None
    return poly


def _cut_inside(original, cut):
    """The middle of every edge of a cut polygon lies in or on the original:
    it is one part of it, not several joined along the footprint's edge."""
    doubled = [(2*x, 2*y) for x, y in original]
    return all(_within(doubled, (p[0]+q[0], p[1]+q[1])) >= 0 for p, q in zip(cut, cut[1:]+cut[:1]))


def wall_pieces(inner, outer, period, footprint, mirrored=False, tries=6):
    """Pieces of a wall with one ragged edge for one face: [(corner list,
    opaque)], or None. inner, outer: one period of the two lines along v
    (lattice points, v rising by `period` from first to last point, u equal
    at both): the opaque part lies beyond the inner line (larger u), the
    punch-through strip between the lines. Both are unrolled over the
    face's v range, cut to its footprint (_cut_to: the strip outwards, the
    opaque part inwards) and tiled into strips. The face's u range must lie
    inside the texture."""
    if mirrored:
        flip = lambda p: (-p[0], p[1])
        made = wall_pieces([flip(p) for p in inner], [flip(p) for p in outer], period, [flip(p) for p in footprint][::-1], False, tries)
        return None if made is None else [([flip(p) for p in face][::-1], solid) for face, solid in made]
    footprint = [tuple(p) for p in footprint]
    low, high = min(p[1] for p in footprint), max(p[1] for p in footprint)
    side = 1 if inner[0][0] > outer[0][0] else -1               # +1: the solid part is at larger u
    far = (max(p[0] for p in footprint)+64) if side > 0 else (min(p[0] for p in footprint)-64)

    def unrolled(line):
        points = []
        for k in range((low-line[-1][1])//period, (high-line[0][1])//period+2):
            for u, v in (line if not points else line[1:]):
                points.append((u, v+k*period))
        poly = points+[(far, points[-1][1]), (far, points[0][1])]
        return poly if sc._area2(poly) > 0 else poly[::-1]
    whole = unrolled([tuple(p) for p in inner])
    solid = _cut_to(whole, footprint, False)
    if solid is not None and not _cut_inside(whole, solid):
        solid = None                                         # (several parts: left to the punch-through strip)
    tiled = None
    # Last resort (a slanted edge that the lines cross more than once): no opaque part on this face.
    for grow, keep in ((True, True), ('near', True), (True, False), ('near', False)):
        region = _cut_to(unrolled([tuple(p) for p in outer]), footprint, grow)
        if region is None:
            return []
        try:
            tiled = tile_strips(region, solid if keep else None, tries)
            if tiled is not None and keep and solid is not None and tile_strips(solid, None, tries) is None:
                tiled = None
        except ValueError:
            tiled = None
        if tiled is not None:
            if not keep:
                solid = None
            break
    pieces = []
    if tiled is None:
        return None
    pieces += [(face, False) for strip in tiled[0] for face in strip_faces(strip)]
    if solid is not None:
        tiled = tile_strips(solid, None, tries)
        if tiled is None:
            return None
        pieces += [(face, True) for strip in tiled[0] for face in strip_faces(strip)]
    return pieces


def design_wall(image, outer=8, inner=8, hole=64, sampler=0.5, colour='dark', inverted=False, margin=0.5):
    """A wall with one ragged edge that repeats along v (a tree line: solid
    from some u on, ragged towards low u; kind `bands` in the
    specification):

    solid   opaque: everything beyond an inner line of `inner` points per
            period (it may bend both ways). Hard rule as for a pine's body:
            every texel within `sampler` texels of it is opaque once the
            enclosed holes of at most `hole` texels are filled; only the
            holes within its reach are filled.
    strip   punch-through, between the inner line and an outer line of
            `outer` points per period that holds every opaque texel with
            `margin` texels around it: nothing is lost.
    Both lines are fitted by _chain_fit over the period, from the row where
    they must reach furthest. A face gets its pieces from wall_pieces()."""
    opaque = image != 15
    height, width = image.shape
    pinholes = holes(image, hole, 0, (False, True))
    solid = opaque | pinholes
    if not opaque.any(1).all() or not solid[:, -1].all():
        raise ValueError('not a wall that is solid at high u in every row')
    first = np.array([int(np.nonzero(row)[0][0]) for row in opaque])                       # first opaque column per row
    run = np.array([width-int(np.nonzero(~row[::-1])[0][0]) if not row.all() else 0 for row in solid])   # solid from here on

    def limit(values, reach):
        """Smallest value over the rows within `reach` half texels of each half-texel grid point along v (periodic)."""
        out = np.full(2*height, np.inf)
        for r in range(height):
            for g in range(2*r-reach, 2*r+2+reach+1):
                out[g % (2*height)] = min(out[g % (2*height)], values[r])
        return out

    def line(values, points):
        seam = int(np.argmin(values))
        turned = np.concatenate([values[seam:], values[:seam+1]])
        chains = _chain_fit(turned, 0, 2*height, points+1, (True, True))
        if not chains:
            raise ValueError('no line fits the wall')
        chain = chains[max(chains)]
        return [(int(round(8*u)), 4*(g+seam)) for g, u in chain]
    outer_line = line(limit(np.maximum(first-margin, 0.0), int(round(2*margin))), outer)
    # The inner line on mirrored values (it must stay at or beyond run+sampler).
    reach = int(math.ceil(2*sampler))
    inner_line = [(8*width-u, v) for u, v in line(limit(width-(run+sampler), reach), inner)]
    ys, xs = np.mgrid[0:height, 0:width]

    def beyond(chain, px, py):
        """Points at larger u than a periodic line (texel units)."""
        v = np.array([p[1]/8 for p in chain])
        u = np.array([p[0]/8 for p in chain])
        phase = (py-v[0]) % height+v[0]
        return px > np.interp(phase, v, u)
    body = beyond(inner_line, xs+0.5, ys+0.5)
    # Texels within the sampler's reach of the solid part: the inner line's smallest u over the rows
    # of the texel's square grown by the reach (the line is straight between its points).
    line_v = np.array([p[1]/8 for p in inner_line])
    line_u = np.array([p[0]/8 for p in inner_line])
    least = np.zeros(height)
    for y in range(height):
        a, b = y-sampler, y+1+sampler
        at = [a, b]
        for k in (-1, 0, 1):
            at += [v+k*height for v in line_v if a < v+k*height < b]
        phase = (np.array(at)-line_v[0]) % height+line_v[0]
        values = np.interp(phase, line_v, line_u)
        # (a point of the line at the period's seam is the same at both ends)
        least[y] = values.min()
    near = xs+1+sampler > least[:, None]
    if (near & ~solid).any():
        raise ValueError('the solid part reaches a texel that is not opaque')
    if (body & ~solid).any():
        raise ValueError('the solid part holds a transparent texel')
    region = beyond(outer_line, xs+0.5, ys+0.5)
    strip = region & ~body
    fill = _colour(image, pinholes & near, colour, inverted, (False, True))
    return dict(kind='bands', polygons=[], bands=[], quads=[],
                wall=dict(inner=[list(p) for p in inner_line], outer=[list(p) for p in outer_line], period=8*height),
                fill=fill, options=dict(sampler=sampler),
                report=dict(opaque_texels=int(opaque.sum()), lost_texels=int((opaque & ~region).sum()),
                            filled_pinhole_texels=len(fill), solid_share_of_opaque=round(float((body & opaque).sum())/float(opaque.sum()), 3),
                            strip_texels=int(strip.sum()), strip_transparent_texels=int((strip & ~opaque).sum()),
                            strip_transparent_share=round(float((strip & ~opaque).sum())/max(float(strip.sum()), 1.0), 3),
                            inner_points=len(inner_line)-1, outer_points=len(outer_line)-1,
                            triangles_per_period=len(inner_line)-1+len(outer_line)-1+2))


def tree_pieces(body, outline, footprint=None, mirrored=False):
    """Pieces of a pine for one face: [(corner list, opaque)] in strip order,
    or None. body: its strip (lattice points), outline: the enclosing
    polygon. footprint: the face's convex polygon in the texture (positive
    order) when it does not hold the whole outline: outline and body are cut
    to it and tiled again. A point where a line crosses the footprint's edge
    becomes the next lattice point ON that edge, outwards for the outline
    and inwards for the body: nothing is lost, the body stays inside itself,
    and the face on the other side of the edge makes the same points.
    Without a footprint the body leaves as its own strip.
    mirrored: the face maps the texture mirrored, so a strip must start on
    a triangle that is negative in the texture: the strips are made on the
    mirrored shape. Every face comes back in positive order (apply() turns
    the faces of a mirrored face round once more)."""
    if mirrored:
        flip = lambda p: (-p[0], p[1])
        made = tree_pieces([flip(p) for p in body], [flip(p) for p in outline][::-1],
                           None if footprint is None else [flip(p) for p in footprint][::-1])
        return None if made is None else [([flip(p) for p in face][::-1], solid) for face, solid in made]
    body = [tuple(p) for p in body]
    ring = body_ring(body)
    outline = [tuple(p) for p in outline]
    # The body's own triangles, as the strip they form in the direction that starts on a positive triangle.
    body_strips = strips_of(_strip_positive(body))
    if len(body_strips) > 1:
        body_strips = None
    if footprint is not None:
        footprint = [tuple(p) for p in footprint]
        cut_ring, outline = _cut_to(ring, footprint, False), _cut_to(outline, footprint, True)
        if outline is None:
            return []
        if cut_ring is not None and not _cut_inside(ring, cut_ring):
            return None
        if cut_ring is None:
            body_strips = []
        elif set(cut_ring) != set(ring):
            tiled = tile_strips(cut_ring)
            if tiled is None:
                return None
            body_strips = tiled[0]
        ring = cut_ring
    if body_strips is None:
        tiled = tile_strips(ring)
        if tiled is None:
            return None
        body_strips = tiled[0]
    tiled = tile_strips(outline, ring)
    if tiled is None:
        return None
    pieces = [(face, False) for strip in tiled[0] for face in strip_faces(strip)]
    pieces += [(face, True) for strip in body_strips for face in strip_faces(strip)]
    return pieces


def design_tree(image, corners=5, outline=8, concave=False, sampler=0.5, close=1, hole=64, thin=5, least=48,
                colour='dark', inverted=False, trunk_overlap=8, body=None, outline_points=None, body_limit=None,
                steps=(8, 4, 2, 1, 4, 2, 1), reach=2, stray=True):
    """A pine that leaves as strips (kind `bands` in the specification):

    body     opaque, one triangle strip of `corners` corners from an apex
             just inside the tip to the base (fit_body). Hard rule: every
             texel within `sampler` texels of it (None: the touched texel
             and its 8 neighbours) is opaque once enclosed holes of at most
             `hole` texels and slits that `close` closings shut are filled;
             of the slits only the part under the body is filled.
    outline  a polygon of at most `outline` corners that holds every opaque
             crown texel with half a texel around it, so nothing is lost:
             convex (enclosing()), or with concave=True a simple polygon
             whose two long lines may bend inwards (enclosing_free());
             concave='best' takes whichever has the smaller area.
    ring     punch-through, the outline minus the body, tiled by their own
             corners into as few triangle strips as found (tile_strips).
    trunk    one opaque quad, running `trunk_overlap` texels under the crown.
    body, outline_points: given geometry instead of a fit (body in texels and
    strip order, outline in lattice points). body_limit: largest u of a body
    corner (texels), so that faces cut short of the texture's end keep the
    body whole. stray=False: opaque texels in the box around the trunk's
    texels but beside its rectangle need not be enclosed (they are lost, as
    in design_cone). The tip must be at the low-u end.
    The specification keeps body and outline (`tree`): a face that shows
    only part of the texture gets its own tiling (tree_pieces)."""
    opaque = image != 15
    height, width = image.shape
    ys, xs = np.mgrid[0:height, 0:width]
    cx, cy = xs+0.5, ys+0.5
    pinholes = holes(image, hole, 0)
    slits = holes(image, hole, close) & ~pinholes if close else np.zeros_like(opaque)
    solid = opaque | pinholes | slits
    crown, bars = _crown(image, thin, least, stray)
    if body is None:
        body = fit_body(solid, crown, corners, sampler, steps, reach, body_limit)
    body = [(int(p[0]), int(p[1])) for p in body]
    positive = _strip_positive(body)
    padded = np.pad(solid, 1)
    safe = np.ones_like(solid)
    for dy in range(3):
        for dx in range(3):
            safe &= padded[dy:dy+height, dx:dx+width]
    for triangle in positive:
        bad = (_touched(list(triangle), image.shape, 0.125) & ~safe) if sampler is None else \
            (_touched(list(triangle), image.shape, sampler) & ~solid)
        if bad.any():
            raise ValueError('the body reaches a texel that is not opaque')
    body_mask = _triangles(lambda t: _inside(list(t), cx, cy), positive)
    kinds = {}
    if outline_points is not None:
        kinds['given'] = [tuple(int(v) for v in p) for p in outline_points]
    else:
        if concave in (False, 'best'):
            for count in range(outline, 2, -1):
                shape = _hull([(int(round(8*x)), int(round(8*y))) for x, y in enclosing(crown, count)])
                if len(shape) <= outline and not encloses(shape, crown, 0.375):
                    kinds['convex'] = shape
                    break
        if concave in (True, 'best'):
            free = enclosing_free(crown, outline)[0]
            if free:
                kinds['free'] = free[max(free)]
    if not kinds:
        raise ValueError('no enclosing outline with that many corners')
    name = min(kinds, key=lambda k: sc._area2(kinds[k]))
    shape = kinds[name]
    hull_mask = _inside_any(shape, cx, cy)
    escaped = int((crown & ~hull_mask).sum())
    if escaped or encloses(shape, crown, 0.375):          # (a convex outline's corners are rounded: up to 1/16 texel)
        raise ValueError('the outline does not hold every crown texel with its margin')
    lattice_body = [(8*u, 8*v) for u, v in body]
    pieces = tree_pieces(lattice_body, shape)
    if pieces is None:
        raise ValueError('the outline cannot be tiled round the body')
    ring_mask = hull_mask & ~body_mask
    polygons, trunks = [], []
    for x0, y0, x1, y1 in bars:
        # From under the crown, but not under the body: two opaque faces that overlap in a plane get
        # different draw layers, and layers end strips. Between body and trunk the ring draws the
        # texels, so no gap opens; the trunk must only start inside the outline.
        bare = x0
        start = max(x0-trunk_overlap, max(p[0] for p in body))
        while start < x1-1 and not hull_mask[y0:y1, start].all():
            start += 1
        if start > bare or not hull_mask[y0:y1, start].all():
            raise ValueError('the trunk does not start inside the outline')
        x0 = start
        polygons.append([(int(x0), int(y0)), (int(x1), int(y0)), (int(x1), int(y1)), (int(x0), int(y1))])
        trunks.append([int(8*bare), int(8*y0), int(8*x1), int(8*y1)])
    touched = np.zeros(image.shape, bool)
    for poly in polygons:
        touched |= _touched(poly, image.shape, 0.125)
    beside = _neighbours(touched, (True, True)) & ~opaque
    under = np.zeros(image.shape, bool)
    if close:
        for triangle in positive:
            under |= _touched(list(triangle), image.shape, sampler if sampler is not None else 1.0)
        if sampler is None:
            under = _neighbours(under, (False, False))
    slits = slits & under
    fill = _colour(image, pinholes | slits | beside, colour, inverted, (True, True))
    drawn = body_mask | ring_mask
    for poly in polygons:
        drawn |= _inside(poly, cx, cy)
    gone = opaque & ~drawn
    ring_faces = [face for face, solid_piece in pieces if not solid_piece]
    body_faces = [face for face, solid_piece in pieces if solid_piece]
    triangles = sum(len(face)-2 for face, _ in pieces)
    strips = len(tile_strips(shape, body_ring(lattice_body))[0])+1
    return dict(kind='bands', polygons=[[[8*x, 8*y] for x, y in poly] for poly in polygons], bands=[],
                outline=[list(p) for p in shape], tree=dict(body=[list(p) for p in lattice_body], outline=[list(p) for p in shape]),
                quads=[dict(list='M', corners=[list(p) for p in face]) for face in ring_faces] +
                      [dict(list='O', corners=[list(p) for p in face]) for face in body_faces],
                trunks=trunks, fill=fill, options=dict(sampler=sampler),
                report=dict(opaque_texels=int(opaque.sum()), lost_texels=int(gone.sum()), lost_crown_texels=int((crown & ~drawn).sum()),
                            filled_pinhole_texels=int(pinholes.sum()), filled_slit_texels_under_the_body=int(slits.sum()),
                            filled_beside_trunk_texels=int(beside.sum()), outline_kind=name, outline_corners=len(shape),
                            outline_reflex_corners=sum(sc._cross(shape[i-1], shape[i], shape[(i+1) % len(shape)]) < 0
                                                       for i in range(len(shape))),
                            body_corners=len(body), body_share_of_opaque=round(float((body_mask & opaque).sum())/float(opaque.sum()), 3),
                            body_transparent_texels=int((body_mask & ~solid).sum()),
                            band_texels=int(ring_mask.sum()), band_transparent_texels=int((ring_mask & ~opaque).sum()),
                            band_transparent_share=round(float((ring_mask & ~opaque).sum())/max(float(ring_mask.sum()), 1.0), 3),
                            triangles_per_plane=triangles+2*len(polygons), strips_per_plane=strips+len(polygons),
                            vertices_per_plane=triangles+2*strips+4*len(polygons)))


def silhouette_deviation(mask, polygons, step=0.5):
    """How far the outline of a union of convex polygons (texel units) is
    from the silhouette of `mask` (enclosed holes do not count), in texels:
    dict(max, rms) over both directions: from every texel edge of the
    silhouette to the nearest polygon edge on the union's outline, and from
    points every `step` texels along that outline to the nearest silhouette
    edge. Edges of one polygon inside another (a trunk under the crown) and
    silhouette edges inside the union's interior are measured like the rest:
    a notch the polygon fills counts with its depth, a tip it cuts with its
    length."""
    height, width = mask.shape
    solid = ~holes_outside(mask)
    points = []
    for y, x in zip(*np.nonzero(solid)):
        if y == 0 or not solid[y-1, x]:
            points.append((x+0.5, y))
        if y == height-1 or not solid[y+1, x]:
            points.append((x+0.5, y+1))
        if x == 0 or not solid[y, x-1]:
            points.append((x, y+0.5))
        if x == width-1 or not solid[y, x+1]:
            points.append((x+1, y+0.5))
    edge = np.array(points, float)

    def inside(poly, pts, margin):
        out = np.ones(len(pts), bool)
        for (ax, ay), (bx, by) in zip(poly, poly[1:]+poly[:1]):
            length = math.hypot(bx-ax, by-ay)
            out &= ((bx-ax)*(pts[:, 1]-ay)-(by-ay)*(pts[:, 0]-ax))/length > margin
        return out
    outline = []
    for k, poly in enumerate(polygons):
        for (ax, ay), (bx, by) in zip(poly, poly[1:]+poly[:1]):
            count = max(int(math.hypot(bx-ax, by-ay)/step), 1)
            t = (np.arange(count)+0.5)/count
            pts = np.stack([ax+(bx-ax)*t, ay+(by-ay)*t], 1)
            keep = np.ones(count, bool)
            for j, other in enumerate(polygons):
                if j != k:
                    keep &= ~inside(other, pts, -1e-6)        # (in or on another polygon: not on the union's outline)
            outline.append(pts[keep])
    outline = np.concatenate(outline)
    # Silhouette edges to the union's outline, and back (the outline samples are dense enough for both).
    d2 = ((edge[:, None, :]-outline[None, :, :])**2).sum(-1)
    there, back = np.sqrt(d2.min(1)), np.sqrt(d2.min(0))
    return dict(max=float(max(there.max(), back.max())), rms=float(math.sqrt((np.mean(there**2)+np.mean(back**2))/2)),
                silhouette_to_shape_max=float(there.max()), shape_to_silhouette_max=float(back.max()))


def holes_outside(mask):
    """Clear texels 4-connected to the border of the mask's box (the outside)."""
    clear = ~np.pad(mask, 1)
    outside = np.zeros_like(clear)
    outside[0, :] = outside[-1, :] = outside[:, 0] = outside[:, -1] = True
    while True:
        grown = outside.copy()
        grown[1:, :] |= outside[:-1, :]
        grown[:-1, :] |= outside[1:, :]
        grown[:, 1:] |= outside[:, :-1]
        grown[:, :-1] |= outside[:, 1:]
        grown &= clear
        if (grown == outside).all():
            return outside[1:-1, 1:-1]
        outside = grown


def design_far(image, corners=5, thin=5, least=48, colour='near', inverted=False, steps=(8, 4, 2, 1), reach=2, peak=0.25):
    """A pine for far away (kind `outline` in the specification: opaque
    only, every transparent texel under it filled, a gutter around it): one
    convex polygon of `corners` corners (3, 4 or 5: 1, 2 or 3 triangles, one
    strip) with its first corner on the real tip and a base edge across the
    trunk's start, and the trunk as one quad from that edge: 3 to 5
    triangles a plane. Fitted on texel corners for the least silhouette
    deviation, mean square both ways plus `peak` times the largest squared
    (silhouette_deviation): it cuts branch tips and fills notches alike. The
    tip must be at the low-u end. The filled texture is this
    specification's own: no other user of the texture may remain."""
    opaque = image != 15
    height, width = image.shape
    crown, bars = _crown(image, thin, least)
    if len(bars) != 1:
        raise ValueError('a far shape needs exactly one trunk')
    columns = np.nonzero(crown.any(0))[0]
    u0 = int(columns[0])
    tip = int(round(float(np.mean(np.nonzero(crown[:, u0:u0+2].any(1))[0])+0.5)))
    x0, y0, x1, y1 = bars[0]
    base = int(x0)

    def rings(points):
        """Crown polygon (apex first) and the trunk from its base edge."""
        return [[(u0, tip)]+points, [(points[len(points)//2-1][0], y0), (x1, y0), (x1, y1), (points[len(points)//2-1][0], y1)]]

    def ok(points):
        poly = [(u0, tip)]+points
        top, bottom = points[len(points)//2-1], points[len(points)//2]
        if top[0] != bottom[0] or not top[1] <= y0 or not bottom[1] >= y1 or top[0] >= x1-1:
            return False
        if any(not 0 <= p[0] <= width or not 0 <= p[1] <= height for p in points):
            return False
        return all(sc._cross(poly[i-1], poly[i], poly[(i+1) % len(poly)]) > 0 for i in range(len(poly)))

    def cost(points):
        d = silhouette_deviation(opaque, rings(points), 1.0)
        return d['rms']**2+peak*d['max']**2
    rows = np.nonzero(crown[:, max(base-12, 0):base].any(1))[0]
    high, low = int(rows[0]), int(rows[-1])+1
    middle = (u0+base)//2
    span = np.nonzero(crown[:, middle-4:middle+4].any(1))[0]
    if corners == 3:
        points = [(base, high), (base, low)]
    elif corners == 4:
        points = [(base, high), (base, low), (middle, int(span[-1])+1)]
    elif corners == 5:
        points = [(middle, int(span[0])), (base, high), (base, low), (middle, int(span[-1])+1)]
    else:
        raise ValueError('a far shape has 3, 4 or 5 crown corners')
    if not ok(points):
        raise ValueError('no start for the far shape')
    best = cost(points)
    for step in steps:
        improved = True
        while improved:
            improved = False
            for which in range(len(points)+1):
                for du in range(-reach, reach+1):
                    for dv in range(-reach, reach+1):
                        if not du and not dv:
                            continue
                        trial = list(points)
                        if which == len(points):          # the base edge as a whole, along u
                            if dv:
                                continue
                            for k in (len(points)//2-1, len(points)//2):
                                trial[k] = (points[k][0]+du*step, points[k][1])
                        else:
                            if du and which in (len(points)//2-1, len(points)//2):
                                continue
                            trial[which] = (points[which][0]+du*step, points[which][1]+dv*step)
                        if not ok(trial):
                            continue
                        value = cost(trial)
                        if value < best-1e-9:
                            best, points, improved = value, trial, True
    polygons = rings(points)
    entry = _finish(image, polygons, colour, inverted, 'outline')
    deviation = silhouette_deviation(opaque, polygons, 0.25)
    entry['report'].update(crown_corners=corners, triangles_per_plane=corners-2+2, vertices_per_plane=corners+4,
                           deviation_max_texels=round(deviation['max'], 2), deviation_rms_texels=round(deviation['rms'], 2),
                           cut_tips_max_texels=round(deviation['silhouette_to_shape_max'], 2),
                           texture_length_texels=width,
                           tree_pixels_for_max_under_1=round(width/deviation['max'], 1),
                           tree_pixels_for_max_under_half=round(width/deviation['max']/2, 1),
                           tree_pixels_for_rms_under_1=round(width/deviation['rms'], 1),
                           tree_pixels_for_rms_under_half=round(width/deviation['rms']/2, 1))
    return entry


def _inside_any(poly, xs, ys):
    """Points strictly inside a simple polygon in lattice units (texel-centre arrays in texels): even-odd rule."""
    px, py = xs*8.0, ys*8.0
    inside = np.zeros(xs.shape, bool)
    for (ax, ay), (bx, by) in zip(poly, poly[1:]+poly[:1]):
        if ay == by:
            continue
        crosses = ((ay > py) != (by > py)) & (px < ax+(py-ay)*(bx-ax)/(by-ay))
        inside ^= crosses
    return inside


def design_rects(image, most=6, tolerance=0.06, least=0.01, colour='near', inverted=False, limit=None):
    """Axis-aligned rectangles over the opaque texels, largest gain first: each
    is the rectangle with the most opaque texels not yet covered among those
    with at most `tolerance` transparent texels; stops after `most` or when
    a rectangle adds less than `least` of the opaque texels."""
    opaque = image != 15
    height, width = image.shape
    if limit:
        opaque = opaque.copy()
        opaque[:, limit[0]:] = False
        opaque[limit[1]:, :] = False
    left = opaque.copy()
    total = int(opaque.sum())
    polygons = []
    clear_sum = np.zeros((height+1, width+1), np.int64)
    clear_sum[1:, 1:] = np.cumsum(np.cumsum(image == 15, 0), 1)
    while len(polygons) < most:
        gain_sum = np.zeros((height+1, width+1), np.int64)
        gain_sum[1:, 1:] = np.cumsum(np.cumsum(left, 0), 1)
        best = (0, None)
        x0s, x1s = np.triu_indices(width+1, 1)
        for y0 in range(height):
            for y1 in range(y0+1, height+1):
                gain = gain_sum[y1, x1s]-gain_sum[y0, x1s]-gain_sum[y1, x0s]+gain_sum[y0, x0s]
                clear = clear_sum[y1, x1s]-clear_sum[y0, x1s]-clear_sum[y1, x0s]+clear_sum[y0, x0s]
                ok = clear <= tolerance*(y1-y0)*(x1s-x0s)
                if not ok.any():
                    continue
                # Most gain, then least transparent, then smallest.
                value = np.where(ok, gain*1000000-clear*1000-(y1-y0)*(x1s-x0s)//64, -1)
                k = int(np.argmax(value))
                if value[k] > best[0]:
                    best = (int(value[k]), (int(x0s[k]), y0, int(x1s[k]), y1), int(gain[k]))
        if best[1] is None or best[2] < least*total:
            break
        x0, y0, x1, y1 = best[1]
        polygons.append([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])
        left[y0:y1, x0:x1] = False
    return _finish(image, polygons, colour, inverted, 'rects', limit)


# ---- application to a mesh pack ---------------------------------------------

def _affine(corners):
    uv = [(c[3], c[4]) for c in corners]
    return not any(uv[0][a]+uv[2][a] != uv[1][a]+uv[3][a] for a in range(2)) and \
        max(abs(corners[0][a]+corners[2][a]-corners[1][a]-corners[3][a]) for a in range(3)) <= sc.AFFINE_POSITION


def _pieces(tex, footprint, entry, width, height, flip, least_area):
    """Lattice polygons of the texture's cores inside one counter-clockwise
    footprint, and counters."""
    report = Counter()
    planes = [sc._plane(p, q) for p, q in zip(footprint, footprint[1:]+footprint[:1])]
    exact = [(Fraction(p[0]), Fraction(p[1])) for p in footprint]
    low = [min(p[a] for p in footprint) for a in range(2)]
    high = [max(p[a] for p in footprint) for a in range(2)]
    period = (8*width, 8*height)
    candidates = []
    for poly in entry['polygons']:
        if flip and (low[0] < 0 or low[1] < 0 or high[0] > period[0] or high[1] > period[1]):
            report['mirrored_repeat_not_supported'] += 1
            continue
        for j in range(low[1]//period[1], -(-high[1]//period[1])):
            for i in range(low[0]//period[0], -(-high[0]//period[0])):
                candidates.append([(Fraction(x+i*period[0]), Fraction(y+j*period[1])) for x, y in poly])
    for axis, start, end in entry['bands']:
        for k in range(low[axis]//period[axis], -(-high[axis]//period[axis])):
            a, b = start+k*period[axis], end+k*period[axis]
            other = (Fraction(low[1-axis]-8), Fraction(high[1-axis]+8))
            if axis == 0:
                candidates.append([(Fraction(a), other[0]), (Fraction(b), other[0]), (Fraction(b), other[1]), (Fraction(a), other[1])])
            else:
                candidates.append([(other[0], Fraction(a)), (other[1], Fraction(a)), (other[1], Fraction(b)), (other[0], Fraction(b))])
    out = []
    for poly in candidates:
        own = [sc._plane(p, q) for p, q in zip(poly, poly[1:]+poly[:1])]
        for plane in planes:
            poly = sc._clip(poly, *plane)
            if not poly:
                break
        if not poly:
            continue
        # On the lattice, inside the footprint and inside the core itself.
        snapped = _snap(poly, planes+own)
        if not snapped or sc._area2(snapped) < 2*64*least_area:
            report['pieces_too_small'] += 1
            continue
        if (tex.touched_by(snapped) & ~tex.safe).any():
            report['pieces_unsafe'] += 1
            continue
        out.append(snapped)
    return out, report


def _band_pieces(tex, footprint, entry, report, mirrored=False):
    """The quads of a `bands` texture inside one counter-clockwise footprint:
    [(lattice points, opaque)]. A quad wholly inside keeps its corner order
    (and so its place in a strip); otherwise its two triangles are cut by
    the footprint, every new point snapped by a rule that depends on the
    point and the footprint alone, so neighbours still meet in one point."""
    planes = [sc._plane(p, q) for p, q in zip(footprint, footprint[1:]+footprint[:1])]
    out = []
    quads = entry['quads']
    wall = entry.get('wall')
    if wall:
        tiled = wall_pieces(wall['inner'], wall['outer'], wall['period'], list(footprint), mirrored)
        if tiled is None:
            raise ValueError('a wall face could not be tiled')
        quads = [dict(list='O' if solid else 'M', corners=face) for face, solid in tiled]
    tree = entry.get('tree')
    if tree:
        # A face that shows part of the tree: outline and body cut to it and tiled again, instead of
        # cutting the pieces (which would break their strips and, on a slanted edge, lose slivers).
        whole = all(a*p[0]+b*p[1] <= c for p in tree['outline'] for a, b, c in planes)
        if all(sc._cross(footprint[i-1], footprint[i], footprint[(i+1) % len(footprint)]) > 0 for i in range(len(footprint))):
            tiled = tree_pieces(tree['body'], tree['outline'], None if whole else list(footprint), mirrored) if mirrored or not whole else None
            if tiled is not None:
                quads = [dict(list='O' if solid else 'M', corners=face) for face, solid in tiled]
                report['tree_faces_tiled_for_their_cut'] += not whole
    for quad in quads:
        corners = [tuple(p) for p in quad['corners']]
        opaque = quad['list'] == 'O'
        if all(a*p[0]+b*p[1] <= c for p in corners for a, b, c in planes):
            parts = [corners]
        else:
            parts = []
            for triangle in [(corners[0], corners[i], corners[i+1]) for i in range(1, len(corners)-1)]:
                poly = [(Fraction(p[0]), Fraction(p[1])) for p in triangle]
                for plane in planes:
                    poly = sc._clip(poly, *plane)
                    if not poly:
                        break
                if not poly:
                    continue
                snapped = _snap(poly, planes)
                if snapped:
                    parts.append(snapped)
                    report['band_pieces_cut_by_the_face'] += 1
        for part in parts:
            sampler = entry.get('options', {}).get('sampler')
            for i in range(1, len(part)-1) if opaque else ():
                triangle = [part[0], part[i], part[i+1]]
                if sampler is None:
                    bad = tex.touched_by(triangle) & ~tex.safe
                else:
                    # Every texel within `sampler` texels of the piece (a little less: the lattice is 1/8 texel).
                    grow = int(8*sampler)-1
                    bad = np.ones(tex.opaque.shape, bool)
                    for p, q in zip(triangle, triangle[1:]+triangle[:1]):
                        a, b, c = sc._plane(p, q)
                        scale = math.sqrt(a*a+b*b)
                        bad &= tex.low(a, b) < c+grow*scale
                    xs, ys = [p[0] for p in triangle], [p[1] for p in triangle]
                    bad &= (tex.cx+8 > min(xs)-grow) & (tex.cx < max(xs)+grow) & (tex.cy+8 > min(ys)-grow) & (tex.cy < max(ys)+grow)
                    bad &= ~tex.opaque
                if bad.any():
                    raise ValueError('a body piece reaches a texel that is not opaque')
            out.append((part, opaque))
    return out


def _snap(poly, planes):
    """A convex polygon on the lattice from exact points, every corner inside
    the footprint's planes; None if it collapses."""
    snapped = []
    for x, y in poly:
        options = sorted({(fx, fy) for fx in (math.floor(x), math.ceil(x)) for fy in (math.floor(y), math.ceil(y))},
                         key=lambda p: ((p[0]-x)**2+(p[1]-y)**2, p))
        point = next((p for p in options if all(a*p[0]+b*p[1] <= c for a, b, c in planes)), None)
        if point is None:
            return None
        if not snapped or snapped[-1] != point:
            snapped.append(point)
    if len(snapped) > 1 and snapped[0] == snapped[-1]:
        snapped.pop()
    return _convex(_hull(snapped)) if len(snapped) >= 3 else None


def _fringe(tex, footprint, cores, mode, hull_cuts, least):
    """Punch-through polygons (lattice) that, with the cores, cover every
    opaque texel of a counter-clockwise footprint; None when they would not
    (the caller keeps the whole face)."""
    exact = [(Fraction(p[0]), Fraction(p[1])) for p in footprint]
    planes = [sc._plane(p, q) for p, q in zip(exact, exact[1:]+exact[:1])]
    total = sc._area2(exact)
    mask = tex.touched(planes)
    regions = []
    if mode == 'hull' or not cores:
        regions.append((list(exact), mask))
    else:
        core = max(cores, key=sc._area2)
        for p, q in zip(core, core[1:]+core[:1]):
            a, b, c = sc._plane(p, q)
            c -= abs(a)+abs(b)                          # one lattice unit inside the core's edge
            beyond = sc._clip(exact, -a, -b, -c)
            if beyond:
                regions.append((beyond, mask & (tex.low(-a, -b) < -c)))
    pieces = []
    for poly, part in regions:
        tight = sc._tighten(tex, poly, part, hull_cuts, least*total)
        if tight is None:
            continue
        snapped = _snap(tight[0], planes)
        if snapped is None:
            return None
        pieces.append(snapped)
    # Every opaque texel of the footprint, at 16 points each, lies in a core or a piece.
    left = np.zeros(tex.opaque.shape, bool)
    for ox in (1, 3, 5, 7):
        for oy in (1, 3, 5, 7):
            xs, ys = tex.cx+ox, tex.cy+oy
            need = tex.opaque.copy()
            for a, b, c in planes:
                need &= a*xs+b*ys < c
            for poly in list(cores)+pieces:
                inside = np.ones(need.shape, bool)
                for (ax, ay), (bx, by) in zip(poly, poly[1:]+poly[:1]):
                    inside &= (bx-ax)*(ys-ay)-(by-ay)*(xs-ax) >= 0
                need &= ~inside
            left |= need
    return None if left.any() else pieces


def apply(pack, spec, images, skip=(), least_area=4.0, lumas=None):
    """Cut the punch-through faces named by the specification, in place
    (prepare_face_order.read_pack tables). images(header) is the index image
    of a material header WITH the specification's fill and gutter applied.

    Trunk variation (spec['trunk_variation'], needs `lumas`, the baked
    (front, back) luma per face): a tree's trunk rectangle takes the trunk
    texels of one of the specification's trunk textures (its own, another
    crown's, or a `rects` texture named there) and one of three widths, both
    chosen from a hash of where the tree stands, so crossed planes of one
    tree agree and nothing changes between builds. The crown is untouched. A
    foreign trunk is drawn with the split material of a material of that
    texture that some face shows under the same lumas (so its binding
    exists), stretched over the whole rectangle. A trunk that differs from
    the source (foreign or resized) lies BELOW the tree's punch-through
    pieces: the skirt's foliage covers it, its holes show it.

    pack['split_materials'] maps each new opaque material to its source,
    pack['origin'] the faces to source faces, pack['overlay'] every overlay
    piece (face index) to the punch-through faces it lies on. Returns the report and a log
    of the pieces per source face."""
    size = pm.MATERIAL.size
    headers = [struct.unpack_from('<4H', pack['materials'], m*size) for m in range(pack['header'][5])]
    faces = pack['faces']
    origin = pack.setdefault('origin', list(range(len(faces))))
    records = [pm.VERTEX.unpack_from(pack['vertices'], i*pm.VERTEX.size) for i in range(pack['header'][3])]
    refs = struct.unpack(f'<{pack["header"][3]}H', pack['refs'])
    textures = {int(word): entry for word, entry in spec['textures'].items()}
    materials = bytearray(pack['materials'])
    split_material, memo, tex_memo = {}, {}, {}
    report, per_word = Counter(), defaultdict(Counter)
    new_faces, new_origin, new_meshes, overlay, foreign_log = [], [], [], {}, []
    vertices_out, refs_out, log = bytearray(), [], []
    group_points = defaultdict(list)
    variation = spec.get('trunk_variation') or {}
    sources, shown = [], defaultdict(dict)
    if variation:
        import zlib
        if lumas is None:
            raise ValueError('trunk variation needs the baked lumas')
        for word in variation['textures']:
            entry = textures[int(word)]
            if entry.get('trunks'):
                sources.append((int(word), entry['trunks'][0]))
            else:
                xs = [p[0] for p in entry['polygons'][0]]
                ys = [p[1] for p in entry['polygons'][0]]
                sources.append((int(word), [min(xs), min(ys), max(xs), max(ys)]))
        for f, face in enumerate(faces):
            header = headers[face[4]]
            if (header[0] >> 13) & 7 == 3 and not header[1] & 0xff00 and header not in skip:
                shown[header[2]].setdefault(tuple(lumas[f]), face[4])
    # A replaced texture carries a gutter: no face may keep sampling it.
    for m, header in enumerate(headers):
        entry = textures.get(header[2])
        if entry and entry['kind'] != 'overlay' and ((header[0] >> 13) & 7 != 3 or header[1] & 0xff00 or header in skip):
            raise ValueError(f'texture {header[2]} is replaced but material {header} cannot be converted')
        if entry and (header[0] >> 13) & 7 == 2 and not header[1] & 0xff00:
            raise ValueError(f'texture {header[2]} also has an opaque user: its texels must not change')
    for mesh in pack['meshes']:
        mesh = list(mesh)
        raw = [pack['vertices'][(mesh[0]+i)*pm.VERTEX.size:(mesh[0]+i+1)*pm.VERTEX.size] for i in range(mesh[1])]
        local_refs = list(refs[mesh[0]:mesh[0]+mesh[1]])
        by_raw, by_position = {}, {}
        for i, record in enumerate(raw):
            by_raw.setdefault(record, i)
            by_position.setdefault(record[:12], local_refs[i])
        first_face = len(new_faces)
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = faces[f]
            header = headers[face[4]]
            entry = textures.get(header[2])
            if entry is None or (header[0] >> 13) & 7 != 3 or header[1] & 0xff00 or header in skip:
                new_faces.append(face)
                new_origin.append(origin[f])
                continue
            stats = per_word[header[2]]
            stats['faces'] += 1
            stats['triangles_before'] += face[5]-2
            corner_index = face[:face[5]]
            corners = [records[mesh[0]+i] for i in corner_index]
            if not face[7] & (3 << 8):
                if entry['kind'] != 'overlay':
                    raise ValueError(f'face {origin[f]}: an undrawable face of a replaced texture')
                new_faces.append(face)
                new_origin.append(origin[f])
                continue
            fans = [list(range(face[5]))] if face[5] == 3 or _affine(corners) else [[0, 1, 2], [0, 2, 3]]
            stats['quads_cut_as_two_triangles'] += len(fans) == 2
            width, height = 32 << (header[0] & 7), 32 << ((header[0] >> 3) & 7)
            flip = (header[0] >> 8) & 3
            made, under, placed, behind = [], [], {}, []
            trunk = None
            if variation and entry.get('trunks') and str(header[2]) in [str(w) for w in variation['textures']]:
                # Where the trunk's axis meets the ground: one point for both halves of a plane and for
                # crossed planes (the face's own affine map, also beyond a cropped face).
                own = entry['trunks'][0]
                foot = (own[2], (own[1]+own[3])//2)
                tri = [corners[0], corners[1], corners[2]]
                det = sc._cross(tri[0][3:5], tri[1][3:5], tri[2][3:5])
                if not det:
                    raise ValueError('a tree face without a texture mapping')
                w1 = sc._cross(tri[0][3:5], foot, tri[2][3:5])/det
                w2 = sc._cross(tri[0][3:5], tri[1][3:5], foot)/det
                centre = [(1-w1-w2)*tri[0][a]+w1*tri[1][a]+w2*tri[2][a] for a in (0, 1, 2)]
                seed = zlib.crc32(struct.pack('<3i', *(int(round(2*v)) for v in centre)))
                kind_word, rect = sources[seed % len(sources)]
                width_factor = Fraction(variation.get('widths', [85, 100, 120])[(seed >> 8) % 3], 100)
                donor = face[4] if kind_word == header[2] else shown[kind_word].get(tuple(lumas[f]))
                if donor is None:
                    stats['trunks_kept_own_no_binding_for_these_lumas'] += 1
                    kind_word, rect, donor = header[2], entry['trunks'][0], face[4]
                xs = [p[0] for p in entry['polygons'][0]]
                trunk = dict(word=kind_word, rect=rect, donor=donor, width=width_factor, own=own, start=min(xs),
                             foreign=kind_word != header[2], varied=kind_word != header[2] or width_factor != 1)
                stats['trunks_of_texture_%d' % kind_word] += 1
                stats['trunks_width_%d' % int(100*width_factor)] += 1
            options = entry.get('options', {})
            fringe = options.get('fringe', 'face') if entry['kind'] == 'overlay' else 'none'
            if len(fans) > 1 and fringe in ('hull', 'pieces'):
                fringe = 'face'
            for fan in fans:
                uv = [(corners[k][3], corners[k][4]) for k in fan]
                turn = sc._area2(uv)
                if not turn:
                    stats['degenerate_uv'] += 1
                    continue
                order = fan if turn > 0 else fan[::-1]
                footprint = tuple((corners[k][3], corners[k][4]) for k in order)
                key = (header[0] & 0x33f, header[2], footprint, turn < 0)
                if key not in memo:
                    low = (min(p[0] for p in footprint), min(p[1] for p in footprint))
                    high = (max(p[0] for p in footprint), max(p[1] for p in footprint))
                    tex = sc.Texels(images(header), flip, low, high)
                    found = _pieces(tex, footprint, entry, width, height, flip, least_area)
                    if entry['kind'] == 'bands':
                        found = (found[0], found[1], _band_pieces(tex, footprint, entry, found[1], turn < 0))
                    else:
                        found = (found[0], found[1], [])
                    trimmed = _fringe(tex, footprint, found[0], fringe, options.get('hull_cuts', 2),
                                      options.get('fringe_least', 0.03)) if fringe in ('hull', 'pieces') else None
                    memo[key] = (*found, trimmed)
                pieces, counters, ordered, trimmed = memo[key]
                for name, value in counters.items():
                    stats[name] += value
                if fringe in ('hull', 'pieces') and trimmed is None:
                    stats['fringe_kept_whole_face'] += 1
                    fringe = 'face'
                for piece, opaque_piece in [(piece, 'trunk' if trunk else True) for piece in pieces] + ordered + \
                        [(piece, False) for piece in (trimmed or []) if fringe in ('hull', 'pieces')]:
                    index = []
                    for point in piece:
                        if opaque_piece == 'trunk':
                            # Position: the own rectangle, widened about its centre line. Texture: the own
                            # texels there, or the foreign trunk stretched over the whole rectangle.
                            middle = Fraction(trunk['own'][1]+trunk['own'][3], 2)
                            where = (Fraction(point[0]), middle+(point[1]-middle)*trunk['width'])
                            if trunk['foreign']:
                                r = trunk['rect']
                                along = Fraction(point[0]-trunk['start'], trunk['own'][2]-trunk['start'])
                                across = Fraction(point[1]-trunk['own'][1], trunk['own'][3]-trunk['own'][1])
                                texel = (int(round(r[0]+along*(r[2]-r[0]))), int(round(r[1]+across*(r[3]-r[1]))))
                            else:
                                texel = point
                            for second in range(1, len(order)-1):
                                tri = [corners[order[0]], corners[order[second]], corners[order[second+1]]]
                                det = sc._cross(tri[0][3:5], tri[1][3:5], tri[2][3:5])
                                w1 = Fraction(sc._cross(tri[0][3:5], where, tri[2][3:5]), det)
                                w2 = Fraction(sc._cross(tri[0][3:5], tri[1][3:5], where), det)
                                if w1 >= 0 and w2 >= 0 and w1+w2 <= 1:
                                    break
                            w = (float(1-w1-w2), float(w1), float(w2))
                            position = [sum(w[n]*tri[n][k] for n in range(3)) for k in range(3)]
                            record = pm.VERTEX.pack(*position, *texel)
                            if record not in by_raw:
                                by_raw[record] = len(raw)
                                raw.append(record)
                                local_refs.append(by_position.setdefault(record[:12], by_raw[record]))
                            index.append(by_raw[record])
                            continue
                        if point in placed:              # one record per point of a face, whichever triangle asks
                            index.append(placed[point])
                            continue
                        at = next((k for k in order if (corners[k][3], corners[k][4]) == point), None)
                        if at is not None:
                            index.append(corner_index[at])
                            placed[point] = corner_index[at]
                            continue
                        # Barycentric in a fan triangle of the footprint that holds the point.
                        for second in range(1, len(order)-1):
                            tri = [corners[order[0]], corners[order[second]], corners[order[second+1]]]
                            det = sc._cross(tri[0][3:5], tri[1][3:5], tri[2][3:5])
                            w1 = Fraction(sc._cross(tri[0][3:5], point, tri[2][3:5]), det)
                            w2 = Fraction(sc._cross(tri[0][3:5], tri[1][3:5], point), det)
                            if w1 >= 0 and w2 >= 0 and w1+w2 <= 1:
                                break
                        w = (float(1-w1-w2), float(w1), float(w2))
                        position = [sum(w[n]*tri[n][k] for n in range(3)) for k in range(3)]
                        box = [(min(c[a] for c in corners), max(c[a] for c in corners)) for a in range(3)]
                        position = [min(max(position[k], box[k][0]), box[k][1]) for k in range(3)]
                        if not 0 <= point[0] <= 65535 or not 0 <= point[1] <= 65535:
                            raise ValueError('core vertex UV out of range')
                        record = pm.VERTEX.pack(*position, *point)
                        if record not in by_raw:
                            by_raw[record] = len(raw)
                            raw.append(record)
                            local_refs.append(by_position.setdefault(record[:12], by_raw[record]))
                        index.append(by_raw[record])
                        placed[point] = by_raw[record]
                    if turn < 0:
                        index = index[::-1]
                    # A convex ring as quads and a last triangle, all from its first corner.
                    at = 1
                    while at+1 < len(index):
                        members = [index[0]]+index[at:at+3]
                        (behind if opaque_piece == 'trunk' and trunk['varied'] else made if opaque_piece else under).append(
                            (members, [list(piece[0])]+[list(p) for p in piece[at:at+3]]))
                        at += 2
            material = face[4]
            for wanted in ([trunk['donor']] if behind else []):
                if wanted not in split_material:
                    split = sc.split_header(headers[wanted])
                    record = bytearray(materials[wanted*size:(wanted+1)*size])
                    struct.pack_into('<4H', record, 0, *split)
                    split_material[wanted] = len(materials)//size
                    materials += record
            if made and material not in split_material:
                split = sc.split_header(header)
                if split in headers:
                    raise ValueError('split header collides with a material of the pack')
                record = bytearray(materials[material*size:(material+1)*size])
                struct.pack_into('<4H', record, 0, *split)
                split_material[material] = len(materials)//size
                materials += record
            below = []
            if fringe == 'face':
                below.append(len(new_faces))
                new_faces.append(face)
                new_origin.append(origin[f])
                stats['punch_through_faces_after'] += 1
                stats['punch_through_triangles_after'] += face[5]-2
            elif not made and not under:
                stats['faces_without_any_piece'] += 1
            back = []
            for members, _ in behind:
                back.append(len(new_faces))
                new_faces.append((*members, *([0]*(4-len(members))), split_material[trunk['donor']], len(members), face[6],
                                  (face[7] & ~3) | (1 if len(members) == 4 else 2), *face[8:]))
                new_origin.append(origin[f])
                group_points[face[11]].append(struct.unpack('<3f', raw[members[2]][:12]))
                stats['opaque_faces'] += 1
                stats['opaque_triangles'] += len(members)-2
                foreign_log.append(len(new_faces)-1)
            for members, _ in under:
                if back:
                    overlay[len(new_faces)] = list(back)
                below.append(len(new_faces))
                new_faces.append((*members, *([0]*(4-len(members))), material, len(members), face[6],
                                  (face[7] & ~3) | (1 if len(members) == 4 else 2), *face[8:]))
                new_origin.append(origin[f])
                group_points[face[11]].append(struct.unpack('<3f', raw[members[2]][:12]))
                stats['punch_through_faces_after'] += 1
                stats['punch_through_triangles_after'] += len(members)-2
            for members, _ in made:
                attribute = (face[7] & ~3) | (1 if len(members) == 4 else 2)
                if below:
                    overlay[len(new_faces)] = list(below)
                new_faces.append((*members, *([0]*(4-len(members))), split_material[material], len(members), face[6],
                                  attribute, *face[8:]))
                new_origin.append(origin[f])
                group_points[face[11]].append(struct.unpack('<3f', raw[members[2]][:12]))
                stats['opaque_faces'] += 1
                stats['opaque_triangles'] += len(members)-2
            stats['faces_with_cores'] += bool(made)
            log.append(dict(face=origin[f], header=list(header), kind=entry['kind'],
                            footprint=[[c[3], c[4]] for c in corners], pieces=[uv for _, uv in made],
                            fringe=fringe, punch_through=[uv for _, uv in under]))
        if len(raw) > 65536:
            raise ValueError('a mesh exceeds 65536 vertices')
        mesh[0], mesh[1] = len(refs_out), len(raw)
        mesh[2], mesh[3] = first_face, len(new_faces)-first_face
        vertices_out += b''.join(raw)
        refs_out += local_refs
        new_meshes.append(mesh)
    groups = bytearray(pack['groups'])
    if groups and len(groups) % pm.GROUP.size == 0 and pm.GROUP.size == 64:
        for group, points in group_points.items():
            values = list(pm.GROUP.unpack_from(groups, group*pm.GROUP.size))
            radius = max(math.dist(p, values[10:13]) for p in points)
            if radius > values[13]:
                values[13] = pm.outward_float(radius)
                pm.GROUP.pack_into(groups, group*pm.GROUP.size, *values)
                report['facing_spheres_enlarged'] += 1
    pack['faces'], pack['origin'], pack['meshes'] = new_faces, new_origin, new_meshes
    pack['vertices'], pack['refs'] = bytes(vertices_out), struct.pack(f'<{len(refs_out)}H', *refs_out)
    pack['materials'], pack['groups'] = bytes(materials), bytes(groups)
    pack['header'][3], pack['header'][4], pack['header'][5] = len(refs_out), len(new_faces), len(materials)//size
    pack['split_materials'] = {new: old for old, new in split_material.items()}
    pack['overlay'] = overlay
    pack['foreign_trunks'] = foreign_log
    return dict(report, split_materials=len(split_material), vertices=len(refs_out),
                textures={str(word): dict(kind=textures[word]['kind'], **stats) for word, stats in sorted(per_word.items())}), log


def _rewrite(pack, mesh_faces):
    """Write back per-mesh lists of (face tuple with corner RECORDS in place of
    indices, origin): vertex table, references, mesh ranges and counts."""
    refs = struct.unpack(f'<{pack["header"][3]}H', pack['refs'])
    new_faces, new_origin, new_meshes, vertices_out, refs_out = [], [], [], bytearray(), []
    for mesh, members in zip(pack['meshes'], mesh_faces):
        mesh = list(mesh)
        raw = [pack['vertices'][(mesh[0]+i)*pm.VERTEX.size:(mesh[0]+i+1)*pm.VERTEX.size] for i in range(mesh[1])]
        local_refs = list(refs[mesh[0]:mesh[0]+mesh[1]])
        by_raw, by_position = {}, {}
        for i, record in enumerate(raw):
            by_raw.setdefault(record, i)
            by_position.setdefault(record[:12], local_refs[i])
        first = len(new_faces)
        for face, source in members:
            index = []
            for record in face[:face[5]]:
                if record not in by_raw:
                    by_raw[record] = len(raw)
                    raw.append(record)
                    local_refs.append(by_position.setdefault(record[:12], by_raw[record]))
                index.append(by_raw[record])
            new_faces.append((*index, *([0]*(4-len(index))), *face[4:]))
            new_origin.append(source)
        if len(raw) > 65536:
            raise ValueError('a mesh exceeds 65536 vertices')
        mesh[0], mesh[1], mesh[2], mesh[3] = len(refs_out), len(raw), first, len(new_faces)-first
        vertices_out += b''.join(raw)
        refs_out += local_refs
        new_meshes.append(mesh)
    pack['faces'], pack['origin'], pack['meshes'] = new_faces, new_origin, new_meshes
    pack['vertices'], pack['refs'] = bytes(vertices_out), struct.pack(f'<{len(refs_out)}H', *refs_out)
    pack['header'][3], pack['header'][4] = len(refs_out), len(new_faces)


def _by_record(pack, words):
    """Per mesh: [(face with corner records, origin, selected)]."""
    size = pm.MATERIAL.size
    headers = [struct.unpack_from('<4H', pack['materials'], m*size) for m in range(pack['header'][5])]
    origin = pack.setdefault('origin', list(range(len(pack['faces']))))
    out = []
    for mesh in pack['meshes']:
        members = []
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = pack['faces'][f]
            header = headers[face[4]]
            corners = tuple(pack['vertices'][(mesh[0]+i)*pm.VERTEX.size:(mesh[0]+i+1)*pm.VERTEX.size] for i in face[:face[5]])
            chosen = header[2] in words and (header[0] >> 13) & 7 == 3 and not header[1] & 0xff00 and \
                not (header[0] >> 8) & 3 and bool(face[7] & (3 << 8))
            members.append([(*corners, *([b'']*(4-face[5])), *face[4:]), origin[f], chosen])
        out.append(members)
    return out, headers


def unwrap(pack, words, limit=1024):
    """Whole texture periods taken out of the UVs of adjacent faces: see the
    module text. limit: largest texture coordinate span, in texels, a set of
    faces brought together may reach (and no coordinate leaves 0..8191)."""
    meshes, headers = _by_record(pack, words)
    report = Counter()
    for members in meshes:
        chosen = [k for k, member in enumerate(members) if member[2]]
        edges = defaultdict(list)
        for k in chosen:
            face = members[k][0]
            for i in range(face[5]):
                a, b = face[i], face[(i+1) % face[5]]
                edges[frozenset((a[:12], b[:12]))].append((k, i))
        links = defaultdict(list)
        for pair in edges.values():
            if len(pair) != 2:
                continue
            (k, i), (n, j) = pair
            fa, fb = members[k][0], members[n][0]
            if fa[4] != fb[4]:
                continue
            a0, a1 = fa[i], fa[(i+1) % fa[5]]
            b0, b1 = fb[j], fb[(j+1) % fb[5]]
            if a0[:12] != b1[:12] or a1[:12] != b0[:12]:
                continue
            ua, ub = struct.unpack('<2H', a0[12:]), struct.unpack('<2H', b1[12:])
            va, vb = struct.unpack('<2H', a1[12:]), struct.unpack('<2H', b0[12:])
            shift = (ua[0]-ub[0], ua[1]-ub[1])
            header = headers[fa[4]]
            period = (8*(32 << (header[0] & 7)), 8*(32 << ((header[0] >> 3) & 7)))
            if shift != (va[0]-vb[0], va[1]-vb[1]) or shift[0] % period[0] or shift[1] % period[1]:
                report['edges_uv_not_a_period_apart'] += 1
                continue
            links[k].append((n, shift))
            links[n].append((k, (-shift[0], -shift[1])))
        done, joined, apart = {}, set(), set()
        for start in chosen:
            if start in done:
                continue
            done[start] = (0, 0)
            group, queue = [start], [start]
            box = None

            def span(k, shift, box):
                uv = [struct.unpack('<2H', r[12:]) for r in members[k][0][:members[k][0][5]]]
                low = [min(p[a]+shift[a] for p in uv) for a in range(2)]
                high = [max(p[a]+shift[a] for p in uv) for a in range(2)]
                if box:
                    low = [min(low[a], box[0][a]) for a in range(2)]
                    high = [max(high[a], box[1][a]) for a in range(2)]
                return low, high
            box = span(start, (0, 0), None)
            while queue:
                k = queue.pop(0)
                for n, shift in links[k]:
                    total = (done[k][0]+shift[0], done[k][1]+shift[1])
                    if n in done:
                        (joined if done[n] == total else apart).add(frozenset((k, n)))
                        continue
                    grown = span(n, total, box)
                    if max(grown[1][a]-grown[0][a] for a in range(2)) > 8*limit:
                        report['edges_left_over_limit'] += 1
                        continue
                    done[n], box = total, grown
                    joined.add(frozenset((k, n)))
                    group.append(n)
                    queue.append(n)
            header = headers[members[start][0][4]]
            period = (8*(32 << (header[0] & 7)), 8*(32 << ((header[0] >> 3) & 7)))
            base = [-(box[0][a]//period[a])*period[a] for a in range(2)]     # smallest coordinates into the first period
            if any(box[1][a]+base[a] > 65535 for a in range(2)):
                raise ValueError('unwrapped coordinates leave the 16-bit range')
            for k in group:
                shift = (done[k][0]+base[0], done[k][1]+base[1])
                if shift == (0, 0):
                    continue
                face = members[k][0]
                moved = []
                for record in face[:face[5]]:
                    u, v = struct.unpack('<2H', record[12:])
                    moved.append(record[:12]+struct.pack('<2H', u+shift[0], v+shift[1]))
                members[k][0] = (*moved, *face[face[5]:])
                report['faces_shifted'] += 1
        report['faces'] += len(chosen)
        report['edges_joined'] += len(joined)
        report['edges_left_inconsistent'] += len(apart-joined)
    _rewrite(pack, [[(m[0], m[1]) for m in members] for members in meshes])
    return dict(report, limit_texels=limit)


def merge(pack, words, limit=1024):
    """Coplanar quads joined: see the module text. Run after unwrap(). The
    absorbed source faces are listed in pack['merged'] under the face kept."""
    meshes, headers = _by_record(pack, words)
    report = Counter()
    merged = pack.setdefault('merged', {})
    use = Counter()
    for members in meshes:
        for face, _, _ in members:
            for position in {record[:12] for record in face[:face[5]]}:
                use[position] += 1
    groups = bytearray(pack['groups'])
    for members in meshes:
        changed = True
        while changed:
            changed = False
            edges = defaultdict(list)
            for k, (face, _, chosen) in enumerate(members):
                if chosen and face is not None and face[5] == 4:
                    for i in range(4):
                        edges[frozenset((face[i], face[(i+1) % 4]))].append((k, i))
            for pair in edges.values():
                if len(pair) != 2:
                    continue
                (k, i), (n, j) = pair
                fa, fb = members[k][0], members[n][0]
                if fa is None or fb is None or fa[4:] != fb[4:] or fa[i] != fb[(j+1) % 4] or fa[(i+1) % 4] != fb[j]:
                    continue
                p, q = fa[i], fa[(i+1) % 4]
                if use[p[:12]] != 2 or use[q[:12]] != 2:
                    report['left_corner_used_elsewhere'] += 1
                    continue
                # Ring: A from q's successor round to p's predecessor, then B likewise.
                ring = [fa[(i+2) % 4], fa[(i+3) % 4], fb[(j+2) % 4], fb[(j+3) % 4]]
                corners = [pm.VERTEX.unpack(r) for r in ring]
                old = [[pm.VERTEX.unpack(r) for r in face[:4]] for face in (fa, fb)]
                if not all(_affine(c) for c in old) or not _affine(corners):
                    report['left_not_affine'] += 1
                    continue
                # p lies on the edge ring[1]-ring[2], q on ring[3]-ring[0], at the same share in position and UV.
                straight = True
                for record, a, b in ((p, corners[1], corners[2]), (q, corners[3], corners[0])):
                    c = pm.VERTEX.unpack(record)
                    if sc._cross(a[3:5], b[3:5], c[3:5]):
                        straight = False
                        break
                    length = math.dist(a[:3], b[:3])
                    share = math.dist(a[:3], c[:3])/length if length else 0.0
                    if any(abs(a[x]+(b[x]-a[x])*share-c[x]) > sc.AFFINE_POSITION for x in range(3)) or \
                            any(abs(a[x]+(b[x]-a[x])*share-c[x]) > 0.51 for x in (3, 4)):
                        straight = False
                        break
                if not straight:
                    report['left_bent'] += 1
                    continue
                uv = [c[3:5] for c in corners]
                if max(max(p[a] for p in uv)-min(p[a] for p in uv) for a in range(2)) > 8*limit:
                    report['left_over_limit'] += 1
                    continue
                if sc._area2(uv) == 0:
                    continue
                members[k][0] = (*ring, *fa[4:])
                members[n][0] = None
                merged.setdefault(members[k][1], []).extend([members[n][1]]+merged.pop(members[n][1], []))
                use[p[:12]] -= 2
                use[q[:12]] -= 2
                report['faces_merged_away'] += 1
                group = fa[11]
                if groups and pm.GROUP.size == 64:
                    values = list(pm.GROUP.unpack_from(groups, group*pm.GROUP.size))
                    radius = math.dist(corners[2][:3], values[10:13])
                    if radius > values[13]:
                        values[13] = pm.outward_float(radius)
                        pm.GROUP.pack_into(groups, group*pm.GROUP.size, *values)
                        report['facing_spheres_enlarged'] += 1
                changed = True
                break
    pack['groups'] = bytes(groups)
    _rewrite(pack, [[(m[0], m[1]) for m in members if m[0] is not None] for members in meshes])
    return dict(report, limit_texels=limit)


def run(pack, spec, images, skip=(), light=None):
    """Everything a specification asks for, in place. Returns (report, log)."""
    report, log = {}, []
    words = set(spec.get('unwrap', []))
    if words:
        report['unwrap'] = unwrap(pack, words, spec.get('uv_limit', 1024))
    words = set(spec.get('merge', []))
    if words:
        report['merge'] = merge(pack, words, spec.get('uv_limit', 1024))
    if spec.get('textures'):
        fills = {int(word): entry.get('fill', [])+entry.get('gutter', []) for word, entry in spec['textures'].items()}
        lumas = None
        if spec.get('trunk_variation'):
            import prepare_face_order
            lumas = prepare_face_order.static_lumas(pack, light)
        cores, log = apply(pack, spec, lambda header: filled(images(header), fills[header[2]]), skip,
                           spec.get('least_area', 4.0), lumas)
        report['cores'] = cores
    return report, log


if __name__ == '__main__':
    import argparse
    import json
    from pathlib import Path
    import prepare_face_order
    from prepare_palette_materials import texture_indices
    from prepare_palette_probe import video, ramp
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('recipe', type=Path, help='JSON: {"textures": {WORD: {"kind": overlay|outline|rects|bands|cone|tree|far|wall, options}}, '
                        '"unwrap": [WORD...], "merge": [WORD...], "uv_limit": texels}')
    parser.add_argument('video', type=Path, help='reference video dump (as for prepare_palette_materials.py)')
    parser.add_argument('source', type=Path, help='the DNM2/4/5 scenery pack the specification is for')
    parser.add_argument('output', type=Path, help='NEW specification (ROM-derived: under build/ only)')
    args = parser.parse_args()
    recipe = json.loads(args.recipe.read_text())
    pack = prepare_face_order.read_pack(args.source.read_bytes())
    _, luma, sheets, _ = video(args.video)
    headers = {struct.unpack_from('<4H', pack['materials'], m*pm.MATERIAL.size) for m in range(pack['header'][5])}
    spec = {key: value for key, value in recipe.items() if key != 'textures'}
    spec['textures'] = {}
    for word, options in recipe.get('textures', {}).items():
        users = sorted(h for h in headers if h[2] == int(word) and (h[0] >> 13) & 7 == 3 and not h[1] & 0xff00)
        if not users or len({h[0] & 0x33f for h in users}) != 1:
            raise SystemExit(f'texture {word}: no punch-through material, or materials of different size or wrap')
        inverted = {bool(ramp(luma, h)[0] > ramp(luma, h)[15]) for h in users}
        if len(inverted) != 1:
            raise SystemExit(f'texture {word}: its materials use both ramp directions')
        header = users[0]
        image = texture_indices(sheets, header[2], 32 << (header[0] & 7), 32 << ((header[0] >> 3) & 7))
        options = dict(options)
        kind = options.pop('kind')
        if 'periodic' in options:
            options['periodic'] = tuple(options['periodic'])
        entry = dict(overlay=design_overlay, outline=design_outline, rects=design_rects, bands=design_bands,
                     cone=design_cone, tree=design_tree, far=design_far, wall=design_wall)[kind](
            image, inverted=inverted.pop(), **options)
        entry['options'] = dict(entry.get('options', {}), **options, kind=kind)
        spec['textures'][word] = entry
        print(word, kind, 'polygons', len(entry['polygons']), 'bands', len(entry['bands']), 'fill', len(entry['fill']),
              'gutter', len(entry.get('gutter', [])), entry.get('report', ''))
    with args.output.open('x') as out:
        json.dump(spec, out)
