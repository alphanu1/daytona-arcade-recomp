"""Punch-through scenery faces that show no hole, moved to the opaque list.

Used by prepare_face_order.py --pt-to-opaque (not a program of its own; the
command line below only reports).

    python3 platform/dreamcast-native/tools/pt_to_opaque.py SOURCE.dnm VIDEO_DUMP [--scenery-classes TRACK_SURFACE_C]
        classification of every punch-through face of a DNM2/4/5 pack (JSON)

Project rule (14 October 2026): a polygon on the punch-through list that
shows a whole rectangle without holes is paid for in the tile renderer's
punch-through passes for nothing; the opaque list draws it for next to
nothing (hidden-surface removal before texturing). Examples: the advertising
boards on the track walls, the banners above the garages.

What a face samples. Every 3D texture is bilinear (make NATIVE_FILTER_ALL=1,
the default): a pixel at texture point (u, v), in texels, reads the four
texels floor(u-1/2)+{0,1} x floor(v-1/2)+{0,1} (texel centres at half
integers). The texels a face can sample are therefore those whose open
square widened by half a texel on every side, (i-1/2, i+3/2) x (j-1/2, j+3/2),
meets the face's open UV polygon (per fan triangle as the PowerVR draws it:
a triangle, an affine quad whole, any other quad as triangles 0 1 2 and
0 2 3), with the texture's wrap or mirror (header word 0 bits 8..9) applied
to the texel indices. reach() computes exactly that, in integers (UVs are
stored in 1/8 texel). For a mip level k the same with texels 2^k times as
large, on the level's holes: a level texel is a hole when less than half of
the full-size texels it covers are opaque (prepare_palette_materials.py
colour_levels, --mip-coverage 0.5).

Classes per face:

  opaque   (a) no transparent texel (palette entry 15) within its reach at
           full size. The face moves to the opaque list unchanged.
  trimmed  (b) the opaque texels its polygon touches form one axis-aligned
           rectangle R of whole texels with nothing transparent inside, and
           the polygon reaches transparent texels only outside R (a frame
           or a margin). The polygon is cut to R in texture space; each new
           corner is the point of the source fan triangle with that UV (its
           own affine map, exact rational arithmetic, rounded once to
           float32), so the cut face shows the very texels at the very
           places the source showed them. Nothing opaque is lost: every
           texel the source polygon touches and that is opaque lies in R.
           At R's edges the filter blends in the transparent neighbour's RGB
           (entry 15, the hole colour), as the punch-through face did where
           its filtered alpha passed the reference (0x80); only at R's
           corners, where two transparent neighbours meet, the punch-through
           face dropped a sliver (about 0.1 texel^2 per corner) that the
           opaque face draws. Reported as corner_slivers.
           --pt-to-opaque-trim no turns this class off.
  holes    (c) anything else: stays punch-through.

Mip levels: the opaque face keeps the texture and its chain (the material
pack copies the source binding; the texture's levels are made as before).
A level texel that is a hole and lies within the reach of a converted face at
that level is drawn in the hole colour instead of being dropped: at the
distance where level k is used one level texel is about one pixel. Counted
per face as level_holes (the level-0 rule above is never relaxed); with
--pt-to-opaque-strict-levels such faces stay punch-through.

Never converted: faces whose material is not a plain textured punch-through
material (word 1 high byte 0: no composite, stipple, split or variant mark),
the reduced view's materials (track_surface.c; the renderer's stage-0 view
keys on their headers), texture words named by a level-of-detail rule or an
opaque-cores specification (lod_groups.py, opaque_cores.py: they cut those
faces themselves), undrawable faces and faces with a degenerate UV mapping.

The opaque material: the source's record under converted_header() (word 0
list bits 010 instead of 011, word 1 gains CONVERTED_MARK 0x0800, which no
other mark uses; texture, colour word, light terms unchanged), one per
source material that has converted faces, so a material shared with faces
that keep their holes keeps its punch-through record for them. It takes the
source's scenery class as it is (no NM_SCENERY_POINT: under
NATIVE_FILTER_ALL both lists are filtered). prepare_palette_materials.py
--split-mesh gives each of its (header, luma) pairs the source's binding
(texture resource, palette, vertex colour) and leaves the texture's chain,
hole colour and palette budget as the unconverted --mesh decides them.
pack['converted_materials'] maps new material -> source material.
"""
import argparse
from collections import Counter, defaultdict
from fractions import Fraction
import json
import math
from pathlib import Path
import struct

import numpy as np

import prepare_models as pm
import split_cutouts as sc

CONVERTED_MARK = 0x0800      # header word 1: free (0x8000 stipple, 0x4000 composite, 0x2000 split, 0x1000 super-sample, 0x300 variant)
MIP_COVERAGE = 0.5           # prepare_palette_materials.py --mip-coverage default


def converted_header(header):
    """Opaque-list header of a converted punch-through header."""
    return (header[0] & ~0x2000, CONVERTED_MARK | header[1], header[2], header[3])


def converted_base(header):
    """The punch-through header a converted header stands for, else None."""
    if header[1] & 0xff00 != CONVERTED_MARK or (header[0] >> 13) & 7 != 2:
        return None
    return (header[0] | 0x2000, header[1] & 0xff, header[2], header[3])


def selectable(header):
    """A plain textured punch-through header (word 0 bits 15..13 = 011, no mark)."""
    return (header[0] >> 13) & 7 == 3 and not header[1] & 0xff00


def _wrap(values, size, mirror):
    if not mirror:
        return values % size
    values = values % (2*size)
    return np.where(values >= size, 2*size-1-values, values)


def reach(poly, scale=8):
    """Texel indices (unwrapped, xs, ys arrays) a bilinear sampler reads anywhere
    inside the open convex polygon `poly` (counter-clockwise, integer points in
    1/8 texel), with texels `scale` units wide (8 << level)."""
    half = scale//2
    us, vs = [p[0] for p in poly], [p[1] for p in poly]
    i0, i1 = (min(us)-3*half)//scale-1, -(-(max(us)+half)//scale)+1
    j0, j1 = (min(vs)-3*half)//scale-1, -(-(max(vs)+half)//scale)+1
    ii, jj = np.meshgrid(np.arange(i0, i1+1, dtype=np.int64), np.arange(j0, j1+1, dtype=np.int64))
    x0, x1 = ii*scale-half, ii*scale+3*half
    y0, y1 = jj*scale-half, jj*scale+3*half
    mask = (x1 > min(us)) & (x0 < max(us)) & (y1 > min(vs)) & (y0 < max(vs))
    for p, q in zip(poly, poly[1:]+poly[:1]):
        # Interior: a*x+b*y > c (left of p->q).
        a, b = p[1]-q[1], q[0]-p[0]
        c = a*p[0]+b*p[1]
        top = max(a*r[0]+b*r[1] for r in poly)
        high = np.maximum(a*x0, a*x1)+np.maximum(b*y0, b*y1)
        low = np.minimum(a*x0, a*x1)+np.minimum(b*y0, b*y1)
        mask &= (high > c) & (low < top)
    return ii[mask], jj[mask]


def touched(poly):
    """Texels (unwrapped) whose open square meets the open polygon (no filter)."""
    us, vs = [p[0] for p in poly], [p[1] for p in poly]
    i0, i1 = min(us)//8-1, -(-max(us)//8)+1
    j0, j1 = min(vs)//8-1, -(-max(vs)//8)+1
    ii, jj = np.meshgrid(np.arange(i0, i1+1, dtype=np.int64), np.arange(j0, j1+1, dtype=np.int64))
    x0, x1, y0, y1 = ii*8, ii*8+8, jj*8, jj*8+8
    mask = (x1 > min(us)) & (x0 < max(us)) & (y1 > min(vs)) & (y0 < max(vs))
    for p, q in zip(poly, poly[1:]+poly[:1]):
        a, b = p[1]-q[1], q[0]-p[0]
        c = a*p[0]+b*p[1]
        top = max(a*r[0]+b*r[1] for r in poly)
        high = np.maximum(a*x0, a*x1)+np.maximum(b*y0, b*y1)
        low = np.minimum(a*x0, a*x1)+np.minimum(b*y0, b*y1)
        mask &= (high > c) & (low < top)
    return ii[mask], jj[mask]


def level_holes(image, coverage=MIP_COVERAGE):
    """Hole masks of every level a square texture's chain would have (level 0 first)."""
    hole = image == 15
    out = [hole]
    side = image.shape[0]
    if image.shape[0] != image.shape[1]:
        return out
    solid = (~hole).astype(float)
    size = side//2
    while size >= 1:
        step = side//size
        out.append(solid.reshape(size, step, size, step).mean((1, 3)) < coverage)
        size //= 2
    return out


def _affine(corners):
    uv = [(c[3], c[4]) for c in corners]
    return not any(uv[0][a]+uv[2][a] != uv[1][a]+uv[3][a] for a in range(2)) and \
        max(abs(corners[0][a]+corners[2][a]-corners[1][a]-corners[3][a]) for a in range(3)) <= sc.AFFINE_POSITION


def fans_of(corners):
    """Corner index lists the PowerVR draws: one polygon, or two triangles."""
    return [list(range(len(corners)))] if len(corners) == 3 or _affine(corners) else [[0, 1, 2], [0, 2, 3]]


def _ccw(corners, fan):
    uv = [(corners[k][3], corners[k][4]) for k in fan]
    turn = sc._area2(uv)
    return (fan if turn > 0 else fan[::-1]), turn


def _sampled(holes, xs, ys, flip):
    h, w = holes.shape
    return holes[_wrap(ys, h, flip & 2), _wrap(xs, w, flip & 1)]


def classify(corners, header, image, holes=None, trim=True):
    """Class of one face and what converting it means.

    corners: vertex records (x, y, z, u, v) of the face; header: material
    header; image: the texture's index image (entry 15 = transparent);
    holes: level_holes(image) or None. Returns a dict: kind 'opaque',
    'trimmed', 'holes' or 'degenerate'; for trimmed the rectangle (u0, v0,
    u1, v1) in 1/8 texel; level_holes per level for converted kinds."""
    flip = (header[0] >> 8) & 3
    holes = holes if holes is not None else level_holes(image)
    fans = []
    for fan in fans_of(corners):
        order, turn = _ccw(corners, fan)
        if not turn:
            return dict(kind='degenerate')
        fans.append([(corners[k][3], corners[k][4]) for k in order])
    reached = [reach(poly) for poly in fans]
    hit = sum(int(_sampled(holes[0], xs, ys, flip).sum()) for xs, ys in reached)
    out = dict(kind='opaque')
    rect = None
    if hit:
        if not trim:
            return dict(kind='holes', reach_holes=hit)
        # Texels the polygon itself touches (no filter): the opaque ones must form one hole-free rectangle.
        txs, tys = [], []
        for poly in fans:
            xs, ys = touched(poly)
            txs.append(xs)
            tys.append(ys)
        xs, ys = np.concatenate(txs), np.concatenate(tys)
        hole = _sampled(holes[0], xs, ys, flip)
        if hole.all():
            return dict(kind='holes', reach_holes=hit, why='no opaque texel')
        ox, oy = xs[~hole], ys[~hole]
        rect = (int(ox.min())*8, int(oy.min())*8, int(ox.max())*8+8, int(oy.max())*8+8)
        inside = (xs >= ox.min()) & (xs <= ox.max()) & (ys >= oy.min()) & (ys <= oy.max())
        if hole[inside].any():
            return dict(kind='holes', reach_holes=hit, why='transparent texel inside the opaque rectangle',
                        inside_holes=int(hole[inside].sum()))
        cut = [clip_rect(poly, rect) for poly in fans]
        if not any(cut) or any(piece and len(piece) < 3 for piece in cut):
            return dict(kind='holes', reach_holes=hit, why='empty cut')
        if any(p[0] != int(p[0]) or p[1] != int(p[1]) for piece in cut for p in piece):
            return dict(kind='holes', reach_holes=hit, why='cut corner off the 1/8 texel lattice')
        cut = [[(int(p[0]), int(p[1])) for p in piece] for piece in cut]
        out = dict(kind='trimmed', rect=list(rect), reach_holes=hit,
                   uncut=all(sorted(piece) == sorted(poly) for piece, poly in zip(cut, fans)))
        # Corners of R where both neighbours across its edges are transparent: there the
        # punch-through face's filtered alpha fell below the reference in a sliver.
        slivers = 0
        for cx, cy, dx, dy in ((rect[0], rect[1], -1, -1), (rect[2], rect[1], 1, -1),
                               (rect[0], rect[3], -1, 1), (rect[2], rect[3], 1, 1)):
            out_x, in_x = (cx//8-1, cx//8) if dx < 0 else (cx//8, cx//8-1)
            out_y, in_y = (cy//8-1, cy//8) if dy < 0 else (cy//8, cy//8-1)
            a = _sampled(holes[0], np.array([out_x]), np.array([in_y]), flip)[0]
            b = _sampled(holes[0], np.array([in_x]), np.array([out_y]), flip)[0]
            if a and b and any(_point_in(piece, (cx, cy)) for piece in cut if piece):
                slivers += 1
        out['corner_slivers'] = slivers
        fans = [piece for piece in cut if piece]
    # Mip levels: holes within the reach at each level (informative unless strict).
    per_level = []
    for k in range(1, len(holes)):
        count = 0
        for poly in fans:
            xs, ys = reach(poly, 8 << k)
            count += int(_sampled(holes[k], xs, ys, flip).sum())
        per_level.append(count)
    out['level_holes'] = per_level
    return out


def _point_in(poly, point):
    """Closed polygon (counter-clockwise) contains the point."""
    return all(sc._cross(p, q, point) >= 0 for p, q in zip(poly, poly[1:]+poly[:1]))


def clip_rect(poly, rect):
    """Convex polygon (counter-clockwise) cut to u0 <= u <= u1, v0 <= v <= v1 (exact)."""
    u0, v0, u1, v1 = rect
    for a, b, c in ((-1, 0, -u0), (1, 0, u1), (0, -1, -v0), (0, 1, v1)):
        poly = sc._clip(poly, a, b, c)
        if not poly:
            return []
    # Drop repeated points (a corner on the cut).
    out = []
    for p in poly:
        if not out or tuple(out[-1]) != tuple(p):
            out.append(p)
    if len(out) > 1 and tuple(out[0]) == tuple(out[-1]):
        out.pop()
    return out


def point_record(corners, order, point):
    """Vertex record of a texture point of the fan polygon `order` (its own affine
    map: barycentric in the fan triangle that holds it, exact, rounded once)."""
    at = next((k for k in order if (corners[k][3], corners[k][4]) == tuple(point)), None)
    if at is not None:
        return None, at
    for second in range(1, len(order)-1):
        tri = [corners[order[0]], corners[order[second]], corners[order[second+1]]]
        det = sc._cross(tri[0][3:5], tri[1][3:5], tri[2][3:5])
        w1 = Fraction(sc._cross(tri[0][3:5], point, tri[2][3:5]), det)
        w2 = Fraction(sc._cross(tri[0][3:5], tri[1][3:5], point), det)
        if w1 >= 0 and w2 >= 0 and w1+w2 <= 1:
            break
    else:
        raise AssertionError('cut point outside its fan')
    w = (1-w1-w2, w1, w2)
    position = [float(sum(w[n]*Fraction(tri[n][k]) for n in range(3))) for k in range(3)]
    if point[0] != int(point[0]) or point[1] != int(point[1]) or not (0 <= point[0] <= 65535 and 0 <= point[1] <= 65535):
        raise ValueError('a cut corner is not a stored texture coordinate')
    return pm.VERTEX.pack(*position, int(point[0]), int(point[1])), None


def run(pack, images, skip=(), skip_words=(), trim=True, strict_levels=False, coverage=MIP_COVERAGE):
    """Convert the punch-through faces of a read_pack() pack in place (module
    text). images(header) -> index image. Returns (report, log)."""
    import opaque_cores
    size = pm.MATERIAL.size
    count = pack['header'][5]
    headers = [struct.unpack_from('<4H', pack['materials'], m*size) for m in range(count)]
    if any(converted_base(h) for h in headers):
        raise ValueError('the pack already holds converted headers')
    origin = pack.setdefault('origin', list(range(len(pack['faces']))))
    records = [pm.VERTEX.unpack_from(pack['vertices'], i*pm.VERTEX.size) for i in range(pack['header'][3])]
    raw = lambda i: pack['vertices'][i*pm.VERTEX.size:(i+1)*pm.VERTEX.size]
    materials = bytearray(pack['materials'])
    new_material, holes_memo, memo = {}, {}, {}
    per_word = defaultdict(Counter)
    log, mesh_faces = [], []
    for mesh in pack['meshes']:
        members = []
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = pack['faces'][f]
            header = headers[face[4]]
            corner_records = [raw(mesh[0]+i) for i in face[:face[5]]]
            keep = ((*corner_records, *([b'']*(4-face[5])), *face[4:]), origin[f])
            if not selectable(header):
                members.append(keep)
                continue
            stats = per_word[header[2]]
            stats['faces'] += 1
            why = 'reduced view' if header in skip else 'level-of-detail or cores texture' if header[2] in skip_words else \
                'undrawn' if not face[7] & (3 << 8) else None
            if why:
                stats['kept_'+why.replace(' ', '_').replace('-', '_')] += 1
                members.append(keep)
                continue
            corners = [records[mesh[0]+i] for i in face[:face[5]]]
            key = (header[0] & 0x33f, header[2], tuple((c[3], c[4]) for c in corners), len(fans_of(corners)))
            if key not in memo:
                texture = (header[2], header[0] & 0x3f)
                if texture not in holes_memo:
                    holes_memo[texture] = level_holes(images(header), coverage)
                memo[key] = classify(corners, header, None, holes_memo[texture], trim)
            result = memo[key]
            kind = result['kind']
            if kind in ('opaque', 'trimmed') and strict_levels and any(result['level_holes']):
                kind = 'level_holes'
            stats[kind] += 1
            entry = dict(face=origin[f], header=list(header), kind=kind,
                         uv=[[c[3], c[4]] for c in corners], centre=[sum(c[a] for c in corners)/len(corners) for a in range(3)])
            for name in ('rect', 'reach_holes', 'level_holes', 'corner_slivers', 'why', 'uncut', 'inside_holes'):
                if name in result:
                    entry[name] = result[name]
            if kind not in ('opaque', 'trimmed'):
                log.append(entry)
                members.append(keep)
                continue
            if face[4] not in new_material:
                record = bytearray(materials[face[4]*size:(face[4]+1)*size])
                header_new = converted_header(header)
                if header_new in headers:
                    raise ValueError('converted header collides with a material of the pack')
                struct.pack_into('<4H', record, 0, *header_new)
                new_material[face[4]] = len(materials)//size
                materials += record
            material = new_material[face[4]]
            if kind == 'opaque' or result.get('uncut'):
                members.append(((*corner_records, *([b'']*(4-face[5])), material, *face[5:]), origin[f]))
                stats['faces_converted'] += 1
                stats['triangles_converted'] += face[5]-2
                if kind == 'trimmed':
                    stats['corner_slivers'] += result['corner_slivers']
                log.append(entry)
                continue
            # Cut to the rectangle: per fan, the clipped polygon as quads and a last triangle from its first corner.
            pieces = []
            for fan in fans_of(corners):
                order, _ = _ccw(corners, fan)
                poly = [(corners[k][3], corners[k][4]) for k in order]
                piece = clip_rect(poly, result['rect'])
                if not piece:
                    continue
                points = []
                for point in piece:
                    record, at = point_record(corners, order, point)
                    points.append(corner_records[at] if record is None else record)
                # Winding as the source: reverse back if the UV order was reversed.
                if order != fan:
                    points = points[::-1]
                at = 1
                while at+1 < len(points):
                    members_ = [points[0]]+points[at:at+3]
                    pieces.append(members_)
                    at += 2
            for corner_list in pieces:
                n = len(corner_list)
                members.append(((*corner_list, *([b'']*(4-n)), material, n, face[6], (face[7] & ~3) | (1 if n == 4 else 2),
                                 *face[8:]), origin[f]))
            stats['faces_converted'] += 1
            stats['faces_cut'] += 1
            stats['pieces'] += len(pieces)
            stats['triangles_converted'] += sum(len(p)-2 for p in pieces)
            stats['triangles_before_cut'] += face[5]-2
            stats['corner_slivers'] += result['corner_slivers']
            entry['pieces'] = len(pieces)
            log.append(entry)
        mesh_faces.append(members)
    pack['materials'] = bytes(materials)
    pack['header'][5] = len(materials)//size
    opaque_cores._rewrite(pack, mesh_faces)
    pack['converted_materials'] = {new: old for old, new in new_material.items()}
    total = Counter()
    for stats in per_word.values():
        total.update(stats)
    words = {str(w): dict(s) for w, s in sorted(per_word.items())}
    return dict(total=dict(total), converted_materials=len(new_material), words=words,
                options=dict(trim=trim, strict_levels=strict_levels, coverage=coverage)), log


def main():
    import prepare_face_order as pfo
    from prepare_palette_materials import texture_indices
    from prepare_palette_probe import video
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('source', type=Path)
    parser.add_argument('video', type=Path)
    parser.add_argument('--scenery-classes', type=Path)
    parser.add_argument('--no-trim', action='store_true')
    parser.add_argument('--log', type=Path)
    args = parser.parse_args()
    pack = pfo.read_pack(args.source.read_bytes())
    sheets = video(args.video)[2]
    images = lambda h: texture_indices(sheets, h[2], 32 << (h[0] & 7), 32 << ((h[0] >> 3) & 7))
    skip = set(pfo.reduced_headers(args.scenery_classes.read_text())) if args.scenery_classes else ()
    report, log = run(pack, images, skip, trim=not args.no_trim)
    if args.log:
        args.log.write_text(json.dumps(log))
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
