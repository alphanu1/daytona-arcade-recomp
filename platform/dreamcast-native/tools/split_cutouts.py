"""Punch-through scenery faces cut into opaque, punch-through and empty parts.

Used by prepare_face_order.py --split-cutouts (not a program of its own).

On the PowerVR an opaque-list pixel is resolved by hidden-surface removal
before it is textured, so opaque overdraw is nearly free; every punch-through
layer is paid for, also where its texel is transparent. A cutout such as a
tree billboard is therefore cut up offline, in the plane of its polygon:

  opaque parts       every texel a pixel of the part can sample is opaque:
                     drawn on the opaque list with a material of its own
                     (same texture, palette bank, light terms and baked luma;
                     header split_header(), class NM_SCENERY_POINT so it is
                     point sampled like the punch-through list)
  mixed parts        stay punch-through faces of the source material
  transparent parts  are not drawn at all

Two ways to choose the parts (mode):

  outline   convex pieces whose edges follow the silhouette. Per face, on the
            texels its UV footprint covers: (1) hull cuts drop what lies
            outside the opaque texels, (2) the largest convex polygon found
            inside the safely opaque texels becomes the opaque part, the
            slivers between it and the hull stay punch-through, (3) a sliver
            is cut once more where that drops enough empty area (a trunk).
  grid      square cells of `cell` texels on the texel grid, classified
            opaque / mixed / transparent, equal neighbours merged greedily
            into rectangles.

  tight     (no opaque parts) a few convex punch-through pieces hugging the
            opaque texels, see tight_pieces(): the footprint is cut along
            texel rows or columns where that drops the most empty area (a
            tree's crown and its trunk; the two sides of a hole) and every
            part is trimmed with hull cuts. Everything drawn stays on the
            punch-through list with the source material: fewer transparent
            texels are rasterised, nothing else changes.

Measurement aids (wrong pictures, for isolating costs on hardware): whole
materials or single faces removed (drop, drop_faces), whole materials moved
to the opaque list with their alpha ignored (opaque).

What makes the result look the same:
- The source face is an affine image of its UV footprint (triangles, and
  quads whose UVs and corners form a parallelogram; other faces are left
  alone and counted). Every piece vertex is a point of the source polygon:
  its position comes from the same affine map, its UV is the footprint
  point, so each piece samples exactly the texels the source face sampled.
- "Safely opaque" is the opaque mask eroded by one texel (8 neighbours),
  with the texture's wrap: an opaque piece can be off by a whole texel at
  its border and still not reach the transparent entry, which on the opaque
  list would show as a speck of that entry's colour.
- A dropped region contains no opaque texel, and stays HULL_GUARD (1/4
  texel) away from every one.
- Pieces of one face share vertex records along common edges: where a
  vertex of one piece lies on an edge of another it is inserted into that
  edge before triangulation, so there is no T-junction inside a face.
  (A piece vertex on the OUTER edge of the source face faces whatever
  neighbour that edge had; billboards have none.)
- Each plan is checked on the final, lattice-rounded vertices before it is
  used: opaque triangles touch safely opaque texels only, every opaque texel
  of the footprint is covered by a drawn triangle, all triangles keep their
  orientation. A plan that fails is not used.

Coordinates here are the pack's UV units, 1/8 texel, integers; texel k spans
[8k, 8k+8). Geometry is exact (fractions) until vertices are written.
"""
from collections import Counter, defaultdict
from fractions import Fraction
import math
import struct

import numpy as np

import prepare_models as pm

SPLIT_MARK = 0x2000      # header[1] high byte of an opaque split material
POINT_CLASS = 0x200      # meshpack.h NM_SCENERY_POINT
HULL_GUARD = 2           # 1/8 texels kept around opaque texels when dropping
AFFINE_POSITION = 1e-3   # model units: quad corner 3 against the parallelogram


def split_header(header):
    """Opaque-list header of a punch-through header's opaque pieces."""
    return (header[0] & ~0x2000, SPLIT_MARK | header[1], header[2], header[3])


def source_header(header):
    """The punch-through header a split header came from (else the header)."""
    if (header[0] >> 13) & 7 == 2 and header[1] & 0xff00 == SPLIT_MARK:
        return (header[0] | 0x2000, header[1] & 0xff, header[2], header[3])
    return tuple(header)


def _directions(limit):
    out = []
    for a in range(-limit, limit+1):
        for b in range(-limit, limit+1):
            if (a or b) and math.gcd(abs(a), abs(b)) == 1:
                out.append((a, b))
    return out


DIRECTIONS = _directions(4)
AXES = [(1, 0), (-1, 0), (0, 1), (0, -1)]


def _area2(poly):
    return sum(p[0]*q[1]-q[0]*p[1] for p, q in zip(poly, poly[1:]+poly[:1]))


def _clip(poly, a, b, c):
    """Part of a convex polygon with a*x+b*y <= c (exact)."""
    out = []
    for p, q in zip(poly, poly[1:]+poly[:1]):
        dp, dq = a*p[0]+b*p[1]-c, a*q[0]+b*q[1]-c
        if dp <= 0:
            out.append(p)
        if (dp < 0 < dq) or (dq < 0 < dp):
            t = Fraction(dp, dp-dq)
            out.append((p[0]+(q[0]-p[0])*t, p[1]+(q[1]-p[1])*t))
    clean = []
    for p in out:
        if not clean or clean[-1] != p:
            clean.append(p)
    if len(clean) > 1 and clean[0] == clean[-1]:
        clean.pop()
    return clean if len(clean) >= 3 and _area2(clean) > 0 else []


def _plane(p, q):
    """Integer (a, b, c): the left side of p->q is a*x+b*y <= c."""
    a, b = q[1]-p[1], p[0]-q[0]
    c = a*p[0]+b*p[1]
    scale = 1
    for v in (a, b, c):
        scale = scale*Fraction(v).denominator//math.gcd(scale, Fraction(v).denominator)
    a, b, c = int(a*scale), int(b*scale), int(c*scale)
    g = math.gcd(math.gcd(abs(a), abs(b)), abs(c)) or 1
    return a//g, b//g, c//g


class Texels:
    """Opaque masks over the texels a footprint can reach, wrap applied."""

    def __init__(self, image, flip, low, high):
        # Texel range with a margin: one for the guard, one for the erosion.
        self.x0, self.y0 = low[0]//8-2, low[1]//8-2
        self.x1, self.y1 = -(-high[0]//8)+2, -(-high[1]//8)+2
        height, width = image.shape

        def wrap(values, size, mirror):
            if not mirror:
                return values % size
            values = values % (2*size)
            return np.where(values >= size, 2*size-1-values, values)
        xs = wrap(np.arange(self.x0-1, self.x1+1), width, flip & 1)
        ys = wrap(np.arange(self.y0-1, self.y1+1), height, flip & 2)
        wide = image[np.ix_(ys, xs)] != 15
        safe = np.ones_like(wide[1:-1, 1:-1])
        for dy in range(3):
            for dx in range(3):
                safe &= wide[dy:dy+safe.shape[0], dx:dx+safe.shape[1]]
        self.opaque, self.safe = wide[1:-1, 1:-1], safe
        # Clear: no opaque texel among the texel and its 8 neighbours.
        clear = np.ones_like(safe)
        for dy in range(3):
            for dx in range(3):
                clear &= ~wide[dy:dy+safe.shape[0], dx:dx+safe.shape[1]]
        self.clear = clear
        self.cx, self.cy = np.meshgrid(8*np.arange(self.x0, self.x1, dtype=np.int64),
                                       8*np.arange(self.y0, self.y1, dtype=np.int64))
        self._cache = {}

    def low(self, a, b):
        """Smallest a*x+b*y over each texel square."""
        key = (a, b)
        if key not in self._cache:
            if max(abs(a), abs(b))*(1 << 20) >= 1 << 62:
                raise OverflowError('edge coefficients too large')
            base = a*self.cx+b*self.cy
            self._cache[key] = (base+min(0, 8*a)+min(0, 8*b), base+max(0, 8*a)+max(0, 8*b))
        return self._cache[key][0]

    def high(self, a, b):
        self.low(a, b)
        return self._cache[a, b][1]

    def touched(self, planes):
        """Texels some open part of which satisfies every plane (superset of
        the texels a convex polygon bounded by these planes touches)."""
        mask = np.ones(self.opaque.shape, bool)
        for a, b, c in planes:
            mask &= self.low(a, b) < c
        return mask

    def touched_by(self, poly):
        """Exactly the texels whose open square meets the open convex polygon
        (separating axes: the polygon's edges and the texel grid's)."""
        mask = self.touched([_plane(p, q) for p, q in zip(poly, poly[1:]+poly[:1])])
        xs, ys = [p[0] for p in poly], [p[1] for p in poly]
        return mask & (self.cx+8 > min(xs)) & (self.cx < max(xs)) & (self.cy+8 > min(ys)) & (self.cy < max(ys))


def _margin(a, b):
    """Offset that keeps a cut valid when its vertices are rounded to the
    lattice (half a unit each way); axis-aligned cuts are on the lattice."""
    return 0 if not a or not b else (abs(a)+abs(b)+1)//2


def _tighten(tex, poly, mask, limit, least):
    """Hull cuts: drop what lies beyond the opaque texels. None if empty."""
    opaque = mask & tex.opaque
    if not opaque.any():
        return None
    for _ in range(limit):
        size, best = _area2(poly), None
        for a, b in DIRECTIONS:
            c = int(tex.high(a, b)[opaque].max())+HULL_GUARD*(abs(a)+abs(b))+_margin(a, b)
            kept = _clip(poly, a, b, c)
            gain = size-(_area2(kept) if kept else 0)
            if kept and (best is None or gain > best[0]):
                best = (gain, a, b, c, kept)
        if best is None or best[0] < least:
            break
        _, a, b, c, poly = best
        mask = mask & (tex.low(a, b) < c)
    return poly, mask


def _inner(tex, poly, mask, vertices, wanted=None):
    """The largest of a few greedy tries, see _inner_try. wanted: the texels
    the polygon may touch (default: the safely opaque ones)."""
    best = None
    wanted = tex.safe if wanted is None else wanted
    for patience in (2.0, 8.0, 32.0, 128.0):
        core = _inner_try(tex, poly, mask, vertices, patience, wanted)
        if core and (best is None or _area2(core) > _area2(best)):
            best = core
    return best


def _inner_try(tex, poly, mask, vertices, patience, wanted):
    """A convex polygon inside `poly` that touches wanted texels only.

    Greedy: while a texel it touches is not safe, cut with the half-plane
    that removes the most unsafe texels per safe texel given up (plus
    `patience`: small values shave thin layers first, large ones cut deep)."""
    for _ in range(600):
        unsafe = mask & ~wanted
        if not unsafe.any():
            break
        good = mask & wanted
        if not good.any():
            return None
        best = None
        for a, b in DIRECTIONS:
            margin = _margin(a, b)
            values = np.sort(tex.low(a, b)[unsafe])
            cuts = np.unique(values)
            removed = len(values)-np.searchsorted(values, cuts, 'left')
            kept = np.sort(tex.high(a, b)[good])
            lost = len(kept)-np.searchsorted(kept, cuts-margin, 'right')
            score = removed/(lost+patience)
            k = int(np.argmax(score))
            if best is None or score[k] > best[0]:
                best = (float(score[k]), a, b, int(cuts[k])-margin)
        _, a, b, c = best
        poly = _clip(poly, a, b, c)
        if not poly:
            return None
        mask = mask & (tex.low(a, b) < c)
    else:
        return None
    # Fewer corners: drop the corner whose loss is smallest (stays inside).
    while len(poly) > vertices:
        loss = [abs(_area2([poly[i-1], poly[i], poly[(i+1) % len(poly)]])) for i in range(len(poly))]
        i = loss.index(min(loss))
        poly = poly[:i]+poly[i+1:]
    return poly


def _crossing(p1, p2, p3, p4):
    """Segments cross at a point inside both."""
    d1, d2 = _cross(p3, p4, p1), _cross(p3, p4, p2)
    d3, d4 = _cross(p1, p2, p3), _cross(p1, p2, p4)
    return ((d1 > 0 > d2) or (d2 > 0 > d1)) and ((d3 > 0 > d4) or (d4 > 0 > d3))


def _strictly_inside(poly, point):
    return all(_cross(p, q, point) > 0 for p, q in zip(poly, poly[1:]+poly[:1]))


def _tile(hull, cores):
    """Triangles tiling a convex polygon minus convex polygons inside it,
    with no corner other than theirs (so nothing can end up inside an edge):
    the greedy triangulation, shortest edges first, of the corner set with
    the outlines as given edges. Rings must already carry each other's
    corners where they touch. None if the result does not add up."""
    rings = [hull]+cores
    points = []
    for ring in rings:
        for p in ring:
            if p not in points:
                points.append(p)
    edges = set()
    for ring in rings:
        for p, q in zip(ring, ring[1:]+ring[:1]):
            edges.add(frozenset((points.index(p), points.index(q))))
    count = len(points)
    candidates = []
    for i in range(count):
        for j in range(i+1, count):
            if frozenset((i, j)) in edges:
                continue
            p, q = points[i], points[j]
            if any(_between(p, q, r) for r in points):
                continue
            middle = ((p[0]+q[0])/2, (p[1]+q[1])/2)
            if any(_strictly_inside(core, middle) for core in cores) or not _strictly_inside(hull, middle):
                continue
            candidates.append(((q[0]-p[0])**2+(q[1]-p[1])**2, i, j))
    for _, i, j in sorted(candidates):
        if not any(_crossing(points[i], points[j], *(points[k] for k in edge)) for edge in edges):
            edges.add(frozenset((i, j)))
    triangles, total = [], 0
    for i in range(count):
        for j in range(i+1, count):
            if frozenset((i, j)) not in edges:
                continue
            for k in range(j+1, count):
                if frozenset((i, k)) not in edges or frozenset((j, k)) not in edges:
                    continue
                t = [points[i], points[j], points[k]]
                size = _cross(*t)
                if not size:
                    continue
                if size < 0:
                    t, size = [t[0], t[2], t[1]], -size
                centre = ((t[0][0]+t[1][0]+t[2][0])/3, (t[0][1]+t[1][1]+t[2][1])/3)
                if any(_strictly_inside(core, centre) for core in cores) or \
                        any(_strictly_inside(t, r) for r in points):
                    continue
                triangles.append(t)
                total += size
    if total != _area2(hull)-sum(_area2(core) for core in cores):
        return None
    return triangles


def _quads(triangles):
    """Pair triangles into strictly convex quads where they share an edge."""
    out, used = [], set()
    for i, t in enumerate(triangles):
        if i in used:
            continue
        for j in range(i+1, len(triangles)):
            if j in used:
                continue
            u = triangles[j]
            shared = [p for p in t if p in u]
            if len(shared) != 2:
                continue
            k = next(n for n in range(3) if t[n] not in shared)
            other = next(p for p in u if p not in shared)
            quad = [t[k], t[(k+1) % 3], other, t[(k+2) % 3]]
            if all(_cross(quad[n-1], quad[n], quad[(n+1) % 4]) > 0 for n in range(4)):
                out.append(quad)
                used.update((i, j))
                break
        else:
            out.append(list(t))
            used.add(i)
    return out


def outline_pieces(tex, footprint, hull_cuts=4, inner_vertices=4, cores=1, least=0.03, least_inner=0.05,
                   voids=0, void_vertices=3):
    """[(class, polygon)] for a CCW footprint; class 'O' opaque, 'M' mixed.

    The hull: the footprint after `hull_cuts` cuts that drop what lies beyond
    the opaque texels. cores: most opaque polygons. After a core is found,
    what lies beyond each of its edges is searched for the next one (a tree
    with a hole in the middle gets one on either side). voids: most convex
    polygons without any opaque texel that are then taken out of what is left
    between hull and cores (the corners beside a trunk); they are not drawn."""
    total = _area2(footprint)
    planes = [_plane(p, q) for p, q in zip(footprint, footprint[1:]+footprint[:1])]

    def empty(poly):
        """No opaque texel within HULL_GUARD of the polygon."""
        mask = np.ones(tex.opaque.shape, bool)
        for p, q in zip(poly, poly[1:]+poly[:1]):
            a, b, c = _plane(p, q)
            mask &= tex.low(a, b) < c+HULL_GUARD*(abs(a)+abs(b))
        return not (mask & tex.opaque).any()

    tight = _tighten(tex, list(footprint), tex.touched(planes), hull_cuts, least*total)
    if tight is None:
        return []
    hull, mask = tight
    if not (mask & ~tex.safe).any():
        return [('O', hull)]
    found, regions, spent = [], [(hull, mask)], []
    while regions and len(found) < cores:
        regions.sort(key=lambda region: -int((region[1] & tex.safe).sum()))
        poly, mask = regions.pop(0)
        if (mask & tex.safe).sum()*128 < least_inner*total:
            spent.append((poly, mask))
            break
        core = _inner(tex, poly, mask, inner_vertices)
        if not core or _area2(core) < least_inner*total:
            spent.append((poly, mask))
            continue
        found.append(core)
        for p, q in zip(core, core[1:]+core[:1]):
            a, b, c = _plane(p, q)
            beyond = _clip(poly, -a, -b, -c)
            if beyond:
                regions.append((beyond, mask & (tex.low(-a, -b) < -c)))
            poly = _clip(poly, a, b, c)
            mask = mask & (tex.low(a, b) < c)
    if not found:
        return [('M', hull)]
    holes = []
    regions += spent
    while regions and len(holes) < voids:
        regions.sort(key=lambda region: -int((region[1] & tex.clear).sum()))
        poly, mask = regions.pop(0)
        if (mask & tex.clear).sum()*128 < least*total:
            break
        hole = _inner(tex, poly, mask, void_vertices, tex.clear)
        if not hole or _area2(hole) < least*total:
            continue
        holes.append(hole)
        for p, q in zip(hole, hole[1:]+hole[:1]):
            a, b, c = _plane(p, q)
            beyond = _clip(poly, -a, -b, -c)
            if beyond:
                regions.append((beyond, mask & (tex.low(-a, -b) < -c)))
            poly = _clip(poly, a, b, c)
    rings = [ring for _, ring in conform([('M', hull)]+[('O', core) for core in found]+[('V', hole) for hole in holes])]
    triangles = _tile(rings[0], rings[1:])
    if triangles is None:
        return [('M', hull)]
    return [('O', core) for core in rings[1:1+len(found)]] + \
        [('M', part) for part in _quads([t for t in triangles if not empty(t)])]


def _box_area(opaque):
    rows, columns = np.flatnonzero(opaque.any(1)), np.flatnonzero(opaque.any(0))
    return int((rows[-1]-rows[0]+1)*(columns[-1]-columns[0]+1)) if len(rows) else 0


def tight_pieces(tex, footprint, pieces=3, hull_cuts=4, least=0.03, tries=3, apart=6):
    """[('M', polygon)] for a CCW footprint: at most `pieces` convex polygons
    that together cover every opaque texel of the footprint (with HULL_GUARD
    around it) and as little else as they can.

    Start: the footprint after `hull_cuts` hull cuts. Then, while a piece is
    left to spend: every piece is tried cut in two along a texel column or
    row (the `tries` lines per axis that leave the smallest bounding boxes,
    at least `apart` texels from each other), both halves trimmed again with
    hull cuts, and the cut that drops the most area is kept if it drops at
    least `least` of the footprint. A half without opaque texels is dropped.
    Cuts lie on texel boundaries, so the pieces meet on lattice lines and a
    corner of one piece on the edge of its neighbour stays on that edge."""
    total = _area2(footprint)
    planes = [_plane(p, q) for p, q in zip(footprint, footprint[1:]+footprint[:1])]
    first = _tighten(tex, list(footprint), tex.touched(planes), hull_cuts, least*total)
    if first is None:
        return []
    regions = [first]
    while len(regions) < pieces:
        best = None
        for n, (poly, mask) in enumerate(regions):
            opaque = mask & tex.opaque
            size = _area2(poly)
            for axis in (0, 1):
                used = np.flatnonzero(opaque.any(axis=axis))
                if len(used) < 2:
                    continue
                ranked = []
                for k in range(int(used[0])+1, int(used[-1])+1):
                    low, high = (opaque[:, :k], opaque[:, k:]) if axis == 0 else (opaque[:k], opaque[k:])
                    ranked.append((_box_area(low)+_box_area(high), k))
                ranked.sort()
                chosen = []
                for _, k in ranked:
                    if all(abs(k-other) >= apart for other in chosen):
                        chosen.append(k)
                        if len(chosen) == tries:
                            break
                for k in chosen:
                    c = 8*((tex.x0 if axis == 0 else tex.y0)+k)
                    a, b = (1, 0) if axis == 0 else (0, 1)
                    parts = []
                    for sign in (1, -1):
                        part = _clip(poly, sign*a, sign*b, sign*c)
                        if not part:
                            continue
                        tight = _tighten(tex, part, mask & (tex.low(sign*a, sign*b) < sign*c), hull_cuts, least*total)
                        if tight is not None:
                            parts.append(tight)
                    gain = size-sum(_area2(part) for part, _ in parts)
                    if best is None or gain > best[0]:
                        best = (gain, n, parts)
        if best is None or best[0] < least*total:
            break
        regions[best[1]:best[1]+1] = best[2]
    return [('M', poly) for poly, _ in regions]


def grid_pieces(tex, footprint, cell):
    """Cells of `cell` texels, equal classes merged into rectangles."""
    step = 8*cell
    xs, ys = [p[0] for p in footprint], [p[1] for p in footprint]
    i0, i1 = min(xs)//step, -(-max(xs)//step)
    j0, j1 = min(ys)//step, -(-max(ys)//step)

    def rectangle(i, j, w, h):
        poly = list(footprint)
        for a, b, c in ((1, 0, (i+w)*step), (-1, 0, -i*step), (0, 1, (j+h)*step), (0, -1, -j*step)):
            poly = _clip(poly, a, b, c)
            if not poly:
                return []
        return poly
    classes = {}
    for j in range(j0, j1):
        for i in range(i0, i1):
            if not rectangle(i, j, 1, 1):
                continue
            # The cell's texels the footprint can reach (the masks end two
            # texels beyond it).
            sx = slice(max(i*cell-tex.x0, 0), (i+1)*cell-tex.x0)
            sy = slice(max(j*cell-tex.y0, 0), (j+1)*cell-tex.y0)
            if not tex.opaque[sy, sx].any():
                continue
            classes[i, j] = 'O' if tex.safe[sy, sx].all() else 'M'
    pieces = []
    for j in range(j0, j1):
        for i in range(i0, i1):
            kind = classes.get((i, j))
            if kind is None:
                continue
            w = 1
            while classes.get((i+w, j)) == kind:
                w += 1
            h = 1
            while all(classes.get((i+k, j+h)) == kind for k in range(w)):
                h += 1
            for dj in range(h):
                for di in range(w):
                    del classes[i+di, j+dj]
            pieces.append((kind, rectangle(i, j, w, h)))
    return pieces


def _cross(a, b, c):
    return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])


def _between(a, b, r):
    """r lies strictly inside segment a-b."""
    if _cross(a, b, r) != 0:
        return False
    d = (r[0]-a[0])*(b[0]-a[0])+(r[1]-a[1])*(b[1]-a[1])
    return 0 < d < (b[0]-a[0])**2+(b[1]-a[1])**2


def conform(pieces):
    """Insert into every edge the vertices of other pieces that lie on it."""
    points = {p for _, poly in pieces for p in poly}
    out = []
    for kind, poly in pieces:
        ring = []
        for a, b in zip(poly, poly[1:]+poly[:1]):
            on = [r for r in points if _between(a, b, r)]
            on.sort(key=lambda r: (r[0]-a[0])*(b[0]-a[0])+(r[1]-a[1])*(b[1]-a[1]))
            ring += [a]+on
        out.append((kind, ring))
    return out


def triangulate(ring):
    """Triangles of a convex ring that may carry collinear vertices; every
    vertex is used and none ends up inside an edge of a triangle."""
    ring, triangles = list(ring), []
    while len(ring) > 3:
        for i in range(len(ring)):
            a, b, c = ring[i-1], ring[i], ring[(i+1) % len(ring)]
            if _cross(a, b, c) > 0 and not any(_between(a, c, r) for r in ring):
                triangles.append((a, b, c))
                del ring[i]
                break
        else:
            raise ValueError('ring cannot be triangulated')
    if _cross(*ring) <= 0:
        raise ValueError('degenerate ring')
    triangles.append(tuple(ring))
    return triangles


def _faces(ring):
    """Quads (fan diagonal 0-2) and triangles of one conformed piece."""
    if len(ring) == 4 and all(_cross(ring[i-1], ring[i], ring[(i+1) % 4]) > 0 for i in range(4)):
        return [tuple(ring)]
    triangles, faces, used = triangulate(ring), [], set()
    for i, t in enumerate(triangles):
        if i in used:
            continue
        for j in range(i+1, len(triangles)):
            if j in used:
                continue
            shared = [p for p in t if p in triangles[j]]
            if len(shared) != 2:
                continue
            k = next(n for n in range(3) if t[n] not in shared)
            x, y, z = t[(k+1) % 3], t[(k+2) % 3], t[k]          # t = (x, y, z) with shared edge x-y
            w = next(p for p in triangles[j] if p not in shared)
            quad = (x, w, y, z)                                  # fan: (x, w, y), (x, y, z)
            if all(_cross(quad[n-1], quad[n], quad[(n+1) % 4]) > 0 for n in range(4)):
                faces.append(quad)
                used.update((i, j))
                break
        else:
            faces.append(t)
            used.add(i)
    return faces


def _on_boundary(footprint, point):
    for e, (a, b) in enumerate(zip(footprint, footprint[1:]+footprint[:1])):
        if point == a:
            return e, Fraction(0)
        if _between(a, b, point):
            length = (b[0]-a[0])**2+(b[1]-a[1])**2
            return e, Fraction((point[0]-a[0])*(b[0]-a[0])+(point[1]-a[1])*(b[1]-a[1]), length)
    return None


FAILURES = Counter()


def plan_face(image, flip, footprint, mode, options):
    """Split plan of one CCW UV footprint, or None when none passes the
    checks. vertices: (exact point, lattice UV, footprint edge and parameter
    or None); faces: (class, vertex numbers)."""
    low = (min(p[0] for p in footprint), min(p[1] for p in footprint))
    high = (max(p[0] for p in footprint), max(p[1] for p in footprint))
    tex = Texels(image, flip, low, high)
    exact = [(Fraction(p[0]), Fraction(p[1])) for p in footprint]
    if mode == 'grid':
        pieces = grid_pieces(tex, exact, options['cell'])
    elif mode == 'tight':
        pieces = tight_pieces(tex, exact, **options)
    else:
        pieces = outline_pieces(tex, exact, **options)
    total = _area2(exact)
    number, vertices, faces = {}, [], []

    def vertex(point):
        if point not in number:
            # Every vertex goes to the UV lattice, where its UV and its
            # position agree exactly. The one exception is a point of the
            # source outline whose lattice point is off the outline (a
            # slanted footprint edge): it keeps its place on the outline, so
            # the face's shape is unchanged, and its UV is rounded (at most
            # 1/16 texel).
            snapped = (int(round(point[0])), int(round(point[1])))
            edge = _on_boundary(exact, point)
            if edge:
                moved = _on_boundary(exact, (Fraction(snapped[0]), Fraction(snapped[1])))
                if moved:
                    point_used, edge = (Fraction(snapped[0]), Fraction(snapped[1])), moved
                else:
                    point_used = point
            else:
                point_used = (Fraction(snapped[0]), Fraction(snapped[1]))
            number[point] = len(vertices)
            vertices.append((point_used, snapped, edge))
        return number[point]
    try:
        for kind, ring in conform(pieces):
            for face in _faces(ring):
                faces.append((kind, tuple(vertex(p) for p in face)))
    except ValueError:
        FAILURES['triangulation'] += 1
        return None
    lattice = [v[1] for v in vertices]
    triangles = []
    for kind, face in faces:
        for i in range(1, len(face)-1):
            triangles.append((kind, lattice[face[0]], lattice[face[i]], lattice[face[i+1]]))
    areas = Counter()
    for kind, a, b, c in triangles:
        size = _cross(a, b, c)
        if size <= 0:
            FAILURES['triangle turned or vanished on the lattice'] += 1
            return None
        areas[kind] += size
        if kind == 'O':
            if (tex.touched_by([a, b, c]) & ~tex.safe).any():
                FAILURES['opaque piece touches an unsafe texel'] += 1
                return None
    # Every opaque texel of the footprint is drawn, as far as it lies in the
    # footprint: exact areas, on the pieces as they lie on the face (a vertex
    # on a slanted outline edge keeps its exact place there).
    planes = [_plane(p, q) for p, q in zip(exact, exact[1:]+exact[:1])]
    opaque = tex.touched(planes) & tex.opaque
    placed = [v[0] for v in vertices]
    shapes = []
    for kind, face in faces:
        for i in range(1, len(face)-1):
            shapes.append([placed[face[0]], placed[face[i]], placed[face[i+1]]])
    whole = np.zeros(opaque.shape, bool)
    reach = []
    for shape in shapes:
        cuts = [_plane(p, q) for p, q in zip(shape, shape[1:]+shape[:1])]
        inside = np.ones(opaque.shape, bool)
        for a, b, c in cuts:
            inside &= tex.high(a, b) <= c
        whole |= inside
        reach.append((cuts, tex.touched_by(shape)))
    for row, column in zip(*np.nonzero(opaque & ~whole)):
        x, y = int(tex.cx[row, column]), int(tex.cy[row, column])
        square = [(Fraction(x), Fraction(y)), (Fraction(x+8), Fraction(y)), (Fraction(x+8), Fraction(y+8)),
                  (Fraction(x), Fraction(y+8))]
        for cut in planes:
            square = _clip(square, *cut)
            if not square:
                break
        if not square:
            continue
        drawn = 0
        for cuts, mask in reach:
            if mask[row, column]:
                part = square
                for cut in cuts:
                    part = _clip(part, *cut)
                    if not part:
                        break
                if part:
                    drawn += _area2(part)
        if drawn != _area2(square):
            FAILURES['opaque texel not covered'] += 1
            return None
    return dict(vertices=vertices, faces=faces, punch=areas['M']/total, opaque=areas['O']/total,
                triangles=len(triangles), pieces=[(kind, [(float(x), float(y)) for x, y in poly]) for kind, poly in pieces])


PLAN_VERSION = 4


def _cached_plan(image, flip, footprint, mode, options):
    """plan_face, kept across runs in the directory SPLIT_CUTOUTS_CACHE names
    (optional; keyed by PLAN_VERSION and every input: raise the version with
    any change to how plans are made, or empty the directory)."""
    import hashlib, os, pickle
    folder = os.environ.get('SPLIT_CUTOUTS_CACHE')
    if not folder:
        return plan_face(image, flip, footprint, mode, options)
    key = hashlib.sha256(image.tobytes()+repr((PLAN_VERSION, image.shape, flip, footprint, mode,
                                               sorted(options.items()))).encode()).hexdigest()
    path = os.path.join(folder, key)
    if os.path.exists(path):
        with open(path, 'rb') as stored:
            return pickle.load(stored)
    plan = plan_face(image, flip, footprint, mode, options)
    with open(path, 'wb') as stored:
        pickle.dump(plan, stored)
    return plan


PRESETS = {
    # hull: one punch-through polygon hugging the silhouette, nothing opaque
    'hull': [dict(hull_cuts=2, cores=0)],
    'lean': [dict(hull_cuts=2, inner_vertices=3, cores=1)],
    'outline': [dict(hull_cuts=3, inner_vertices=4, cores=2, voids=2)],
    'rich': [dict(hull_cuts=5, inner_vertices=5, cores=4, least=0.02, least_inner=0.03, voids=4)],
}
# tight: every plan below is made for a face; split_cutouts() takes the one
# that drops the most area for its triangles.
TIGHT = [dict(pieces=pieces, hull_cuts=cuts) for pieces in (1, 2, 3, 4) for cuts in (2, 4, 6)]
# auto: every plan below is made for a face and the one that pays best taken.
AUTO = [dict(hull_cuts=cuts, cores=0) for cuts in (1, 2, 3, 4)] + [
    dict(hull_cuts=2, inner_vertices=3, cores=1), dict(hull_cuts=2, inner_vertices=4, cores=1),
    dict(hull_cuts=3, inner_vertices=4, cores=1, voids=1), dict(hull_cuts=3, inner_vertices=3, cores=2),
    dict(hull_cuts=3, inner_vertices=4, cores=2, voids=2), dict(hull_cuts=4, inner_vertices=5, cores=3, voids=3)]


def split_cutouts(pack, images, mode='outline', cell=16, least_gain=0.25, face_limit=24, budget=None,
                  only=None, skip=(), weights=None, pixel_price=100.0, tight_triangles=8, tight_price=0.02,
                  drop=(), opaque=(), drop_faces=()):
    """Cut the punch-through faces of a pack (prepare_face_order.read_pack
    tables) in place. images(header) returns the index image of a material
    header. Returns the report; pack['split_materials'] maps each new opaque
    material to its source, pack['origin'] the faces to source faces.

    least_gain: least share of a face's punch-through area that must go.
    face_limit: most faces one source face may become.
    budget: most triangles added over the whole pack; the faces that remove
    the most punch-through area per added triangle are taken first.
    only: restrict to these texture words (header[2]).
    skip: material headers left alone (the reduced view's cutouts).

    mode 'auto' with weights {source face: (pixel samples drawn, frames
    seen)} from recorded runs (the simulator's fill file): each face takes,
    among the AUTO plans, the one with the largest
        samples * (share of the face leaving the punch-through list)
        - pixel_price * (faces added) * (frames seen)
    and stays whole when none is positive: an added face is paid for on the
    SH4 in every frame the face is drawn, the punch-through pixels saved are
    the gain, and pixel_price says how many of them one added face must save
    per frame. A face the runs never show stays whole.

    mode 'tight': punch-through pieces only (tight_pieces). Each face takes,
    among the TIGHT plans of at most tight_triangles triangles, the one with
    the largest
        (share of the face no longer drawn) - tight_price * (triangles added)
    if that share reaches least_gain.

    mode 'none': no face is cut; only the measurement aids below apply.
    drop, opaque: texture words (header[2]) of punch-through materials whose
    faces are removed, or drawn whole on the opaque list (alpha ignored: a
    wrong picture, for measuring what the punch-through list costs).
    drop_faces: source face indices removed. All three ignore `only`.
    """
    size = pm.MATERIAL.size
    count = pack['header'][5]
    headers = [struct.unpack_from('<4H', pack['materials'], m*size) for m in range(count)]
    faces = pack['faces']
    origin = pack.setdefault('origin', list(range(len(faces))))
    records = [pm.VERTEX.unpack_from(pack['vertices'], i*pm.VERTEX.size) for i in range(pack['header'][3])]
    if mode == 'auto' and weights is None:
        raise ValueError('mode auto needs weights')
    option_sets = [dict(cell=cell)] if mode == 'grid' else AUTO if mode == 'auto' else TIGHT if mode == 'tight' \
        else [] if mode == 'none' else PRESETS[mode]
    drop, opaque, drop_faces = set(drop), set(opaque), set(drop_faces)
    report, plans, memo = Counter(), {}, {}
    per_material = defaultdict(Counter)
    for mesh in pack['meshes']:
        stacked = Counter()
        for f in range(mesh[2], mesh[2]+mesh[3]):
            stacked[frozenset(records[mesh[0]+i][:3] for i in faces[f][:faces[f][5]])] += 1
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = faces[f]
            header = headers[face[4]]
            if (header[0] >> 13) & 7 != 3 or header[1] & 0xff00 or not face[7] & (3 << 8):
                continue
            if header not in skip and (header[2] in drop or origin[f] in drop_faces):
                plans[f] = (dict(whole=None), True)
                report['faces_dropped'] += 1
                continue
            if header not in skip and header[2] in opaque:
                plans[f] = (dict(whole='O'), True)
                report['faces_made_opaque'] += 1
                continue
            if mode == 'none' or (only is not None and header[2] not in only):
                continue
            report['punch_through_faces'] += 1
            if header in skip:
                report['left_reduced_view'] += 1
                continue
            corners = [records[mesh[0]+i] for i in face[:face[5]]]
            uv = [(c[3], c[4]) for c in corners]
            if stacked[frozenset(c[:3] for c in corners)] > 1:
                report['left_shares_its_polygon_with_another_face'] += 1
                continue
            if face[5] == 4:
                if any(uv[0][a]+uv[2][a] != uv[1][a]+uv[3][a] for a in range(2)) or \
                        max(abs(corners[0][a]+corners[2][a]-corners[1][a]-corners[3][a]) for a in range(3)) > AFFINE_POSITION:
                    report['left_not_affine'] += 1
                    continue
            turn = _area2(uv)
            if not turn:
                report['left_degenerate_uv'] += 1
                continue
            footprint = uv if turn > 0 else uv[::-1]
            key = (header[0] & 0x33f, header[2], tuple(footprint))
            if mode == 'auto':
                samples, frames = weights.get(origin[f], (0, 0))
                if not frames:
                    report['left_never_seen'] += 1
                    continue
                if samples < pixel_price*frames:      # Not even one added face can pay.
                    report['left_does_not_pay'] += 1
                    continue
            if key not in memo:
                memo[key] = []
                for options in option_sets:
                    try:
                        plan = _cached_plan(images(header), (header[0] >> 8) & 3, footprint,
                                            mode if mode in ('grid', 'tight') else 'outline', options)
                    except OverflowError:
                        plan = None
                    if plan is not None and len(plan['faces']) <= face_limit:
                        memo[key].append(plan)
            if not memo[key]:
                report['left_no_valid_plan'] += 1
            elif mode == 'auto':
                gain = [samples*float(1-plan['punch'])-pixel_price*(len(plan['faces'])-1)*frames for plan in memo[key]]
                best = max(range(len(gain)), key=gain.__getitem__)
                if gain[best] > 0:
                    plans[f] = (memo[key][best], turn > 0)
                else:
                    report['left_does_not_pay'] += 1
            elif mode == 'tight':
                fit = [plan for plan in memo[key] if plan['triangles'] <= tight_triangles and
                       1-plan['punch'] >= least_gain and not plan['opaque']]
                if fit:
                    plans[f] = (max(fit, key=lambda plan: float(1-plan['punch']) -
                                    tight_price*(plan['triangles']-(face[5]-2))), turn > 0)
                else:
                    report['left_gain_too_small'] += 1
            else:
                plan = next((plan for plan in memo[key] if 1-plan['punch'] >= least_gain), None)
                if plan is None:
                    report['left_gain_too_small'] += 1
                else:
                    plans[f] = (plan, turn > 0)
    # Budget: world area that leaves the punch-through list per added triangle.
    def world_area(f):
        mesh = next(m for m in pack['meshes'] if m[2] <= f < m[2]+m[3])
        p = [records[mesh[0]+i][:3] for i in faces[f][:faces[f][5]]]
        total = 0.0
        for i in range(1, len(p)-1):
            a, b = [p[i][k]-p[0][k] for k in range(3)], [p[i+1][k]-p[0][k] for k in range(3)]
            total += 0.5*math.sqrt((a[1]*b[2]-a[2]*b[1])**2+(a[2]*b[0]-a[0]*b[2])**2+(a[0]*b[1]-a[1]*b[0])**2)
        return total
    if budget is not None:
        ranked = sorted(plans, key=lambda f: (-world_area(f)*(1-plans[f][0]['punch']) /
                                              max(plans[f][0]['triangles']-(faces[f][5]-2), 0.5), f))
        spent, chosen = 0, {}
        for f in ranked:
            added = plans[f][0]['triangles']-(faces[f][5]-2)
            if spent+max(added, 0) > budget:
                report['left_over_budget'] += 1
                continue
            spent += max(added, 0)
            chosen[f] = plans[f]
        plans = chosen
    materials = bytearray(pack['materials'])
    split_material = {}
    new_faces, new_origin, new_meshes = [], [], []
    vertices_out, refs_out = bytearray(), []
    refs = struct.unpack(f'<{pack["header"][3]}H', pack['refs'])
    group_points = defaultdict(list)
    plan_log = []
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
            if f not in plans:
                new_faces.append(face)
                new_origin.append(origin[f])
                continue
            plan, ccw = plans[f]
            if 'whole' in plan:
                # Measurement aids: the face removed, or unchanged on the opaque list.
                if plan['whole'] == 'O':
                    material = face[4]
                    if material not in split_material:
                        header = split_header(headers[material])
                        if header in headers:
                            raise ValueError('split header collides with a material of the pack')
                        record = bytearray(materials[material*size:(material+1)*size])
                        struct.pack_into('<4H', record, 0, *header)
                        split_material[material] = len(materials)//size
                        materials += record
                    new_faces.append((*face[:4], split_material[material], *face[5:]))
                    new_origin.append(origin[f])
                continue
            source = face[:face[5]] if ccw else face[:face[5]][::-1]   # footprint order
            corner = [records[mesh[0]+i] for i in source]
            box = [(min(c[a] for c in corner), max(c[a] for c in corner)) for a in range(3)]
            index = []
            for point, lattice, edge in plan['vertices']:
                if edge is not None and edge[1] == 0:
                    index.append(source[edge[0]])
                    continue
                if edge is not None:
                    a, b = corner[edge[0]], corner[(edge[0]+1) % len(corner)]
                    t = float(edge[1])
                    position = [a[k]+(b[k]-a[k])*t for k in range(3)]
                else:
                    # Barycentric in the fan triangle that holds the point.
                    for second in range(1, len(corner)-1):
                        tri = (corner[0], corner[second], corner[second+1])
                        det = _cross(tri[0][3:5], tri[1][3:5], tri[2][3:5])
                        w1 = Fraction(_cross(tri[0][3:5], point, tri[2][3:5]), det)
                        w2 = Fraction(_cross(tri[0][3:5], tri[1][3:5], point), det)
                        if w1 >= 0 and w2 >= 0 and w1+w2 <= 1:
                            break
                    w = (float(1-w1-w2), float(w1), float(w2))
                    position = [sum(w[n]*tri[n][k] for n in range(3)) for k in range(3)]
                position = [min(max(position[k], box[k][0]), box[k][1]) for k in range(3)]
                if not 0 <= lattice[0] <= 65535 or not 0 <= lattice[1] <= 65535:
                    raise ValueError('split vertex UV out of range')
                record = pm.VERTEX.pack(*position, *lattice)
                if record not in by_raw:
                    by_raw[record] = len(raw)
                    raw.append(record)
                    local_refs.append(by_position.setdefault(record[:12], by_raw[record]))
                index.append(by_raw[record])
            material = face[4]
            if any(kind == 'O' for kind, _ in plan['faces']) and material not in split_material:
                header = split_header(headers[material])
                if header in headers:
                    raise ValueError('split header collides with a material of the pack')
                record = bytearray(materials[material*size:(material+1)*size])
                struct.pack_into('<4H', record, 0, *header)
                split_material[material] = len(materials)//size
                materials += record
            for kind, members in plan['faces']:
                members = [index[v] for v in (members if ccw else members[::-1])]
                if not ccw and len(members) == 4:
                    # Reversed quad: keep the fan diagonal (corners 0 and 2).
                    members = members[1:]+members[:1]
                attribute = (face[7] & ~3) | (1 if len(members) == 4 else 2)
                new_faces.append((*members, *([0]*(4-len(members))), split_material[material] if kind == 'O' else material,
                                  len(members), face[6], attribute, *face[8:]))
                new_origin.append(origin[f])
                group_points[face[11]].append(struct.unpack('<3f', raw[members[2]][:12]))
            added = len(plan['faces'])
            stats = per_material[headers[material]]
            stats['faces'] += 1
            stats['faces_after'] += added
            stats['triangles_before'] += face[5]-2
            stats['triangles_after'] += plan['triangles']
            stats['opaque_faces'] += sum(1 for kind, _ in plan['faces'] if kind == 'O')
            area = world_area(f)
            stats['area'] += area
            stats['area_punch_through'] += area*float(plan['punch'])
            stats['area_opaque'] += area*float(plan['opaque'])
            report['faces_split'] += 1
            report['faces_added'] += added-1
            report['triangles_added'] += plan['triangles']-(face[5]-2)
            plan_log.append(dict(face=origin[f], header=list(headers[material]), ccw=ccw,
                                 footprint=[list(p) for p in (uv_of(records, mesh, face))],
                                 pieces=plan['pieces'],
                                 faces=[[kind, [list(plan['vertices'][v][1]) for v in members]] for kind, members in plan['faces']]))
        if len(raw) > 65536:
            raise ValueError('a mesh exceeds 65536 vertices after splitting')
        mesh[0], mesh[1] = len(refs_out), len(raw)
        mesh[2], mesh[3] = first_face, len(new_faces)-first_face
        vertices_out += b''.join(raw)
        refs_out += local_refs
        new_meshes.append(mesh)
    # Facing spheres of the groups: enclose the new faces' corner 2.
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
    table = []
    for header, stats in sorted(per_material.items(), key=lambda item: -item[1]['area']):
        table.append(dict(header=list(header), faces=stats['faces'], faces_after=stats['faces_after'],
                          opaque_faces=stats['opaque_faces'],
                          triangles_before=stats['triangles_before'], triangles_after=stats['triangles_after'],
                          area=round(stats['area'], 1),
                          punch_through_share=round(stats['area_punch_through']/stats['area'], 4),
                          opaque_share=round(stats['area_opaque']/stats['area'], 4)))
    if mode == 'tight':
        report['tight_triangles'], report['tight_price'] = tight_triangles, tight_price
    return dict(report, plan_failures=dict(FAILURES), pixel_price=pixel_price if mode == 'auto' else None, mode=mode, cell=cell if mode == 'grid' else None, least_gain=least_gain,
                face_limit=face_limit, budget=budget, split_materials=len(split_material),
                distinct_footprints=len(memo), vertices=len(refs_out), materials=table), plan_log


def uv_of(records, mesh, face):
    return [records[mesh[0]+i][3:5] for i in face[:face[5]]]
