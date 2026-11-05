"""Road decals kept on or above the road (prepare_face_order.py --drape-road-decals).

Course 0's rubber streaks (texture 6680, punch-through, 501 faces) are a mesh
of their own that does not follow the coarse road mesh. Their corners float
0 to 0.106 units above the road triangles the PowerVR draws (most of them
0.05; up is +y, the supplied road normals point down), but a streak quad is
long and on the crests and banked curves spans creases of the road's warped
quads: there its middle dips up to 0.18 units under the road (68 faces). The
arcade sorts polygons and shows the streaks on top anyway. Without this pass
prepare_face_order.py lifts every road decal by ROAD_DECAL_LAYER = 240 layers
(written depth times 1.234) so that it wins over those dips too; then its
written depth is no longer its own, the streaks show through crests up to 23%
nearer, and the cars' shadow prisms (code/render/scene/practice.c, one unit up
and down from the posed footprint, at the road-surface depth scale) miss them:
in the simulator 16% of the shadow lands on streaks within 12 units, nothing
beyond.

This pass removes the dips, so that one layer above the road surface
(prepare_face_order.py: 1 + the largest layer of the road-surface faces) wins
everywhere and the written depth is the true one:

1. Under the road. For every fan triangle of a road-decal face the road
   triangles under it (fan from corner 0 of each road-surface face that is no
   decal, outside the world-object meshes, within LAYER_REACH of its height)
   are clipped to it in plan; the depth of the decal under the road is
   largest at a corner of such an overlap, where it is measured.
2. Raise (uncut="raise", the default). The corners of a face deeper than
   TOLERANCE rise by that depth (a corner of several faces by the largest), so
   the face lies on or above the road everywhere; x, z and the texture
   coordinates stay, and every other corner stays where the arcade has it.

Measurement alternatives (not used by the recipes): corner_mode="snap" first
moves every decal corner vertically onto the road triangle under it (the one
nearest in height within ROAD_REACH among those whose plan holds the corner,
else the nearest in plan within PLAN_REACH); cut=DEPTH cuts a face deeper than
DEPTH, in plan, along the edges of the road triangles under it, every piece in
its road triangle's plane with the texture coordinates of its plan position in
the decal triangle (rounded to the 1/8 texel of the vertex format; pieces of
more than four corners become a quad and the rest; a triangle the road does not
cover once within COVER is left whole); uncut="lift" gives an uncut face that
dips extra layers instead (prepare_face_order.py), enough for a camera
lift_height above the road. Snapped and cut streaks follow the road exactly
but cost the SH4 about 0.1 ms a frame (twice the faces); lifted ones lose the
shadow again where they are lifted.

Faces keep material, supplied normal, attribute bits but the corner count,
face group and their place in the source order (pack['origin']); vertex
records are shared by position and coordinates per mesh, the position
references rebuilt, and mesh and face-group bounds and the groups' facing
spheres grown to hold the moved corners. Report: corners moved and by how
far, faces raised (or cut, lifted), the largest depth under the road left.
"""
import math
import struct
from collections import defaultdict, Counter

import prepare_models as pm

TOLERANCE = 1e-4     # largest depth under the road left, model units
LIFT_HEIGHT = 1.0    # camera height above the road an uncut face's extra layers are made for (race eye: 1.85)
PLAN_REACH = 0.25    # a corner beside every road triangle drapes onto the nearest within this, in plan
ROAD_REACH = 2.0     # prepare_face_order.ROAD_DECAL_REACH
LAYER_REACH = 0.5    # road triangles this far above or below a draped face's centre are the ones it is cut along
SLIVER = 1e-5        # pieces of less area (square model units) are dropped: overlaps of road faces, invisible slivers
COVER = 1e-3         # the road triangles must cover a decal triangle once within this share of its area
CELL = 8.0           # plan grid of the road triangle index
EDGE = 1e-6          # plan tolerance of the containment tests


def _f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def _area2(poly):
    return sum(a[0]*b[1]-b[0]*a[1] for a, b in zip(poly, poly[1:]+poly[:1]))


class Road:
    """The rasterised road triangles in plan (x, z) with their planes, on a grid."""

    def __init__(self, triangles):
        self.tris = []
        self.grid = defaultdict(list)
        for owner, p in triangles:
            e1 = [p[1][k]-p[0][k] for k in range(3)]
            e2 = [p[2][k]-p[0][k] for k in range(3)]
            n = (e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0])
            plan = [(q[0], q[2]) for q in p]
            if abs(n[1]) < 1e-12 or abs(_area2(plan)) < 1e-12:
                continue
            index = len(self.tris)
            self.tris.append((owner, plan, n, n[0]*p[0][0]+n[1]*p[0][1]+n[2]*p[0][2]))
            for gx in range(math.floor(min(q[0] for q in plan)/CELL), math.floor(max(q[0] for q in plan)/CELL)+1):
                for gz in range(math.floor(min(q[1] for q in plan)/CELL), math.floor(max(q[1] for q in plan)/CELL)+1):
                    self.grid[gx, gz].append(index)

    def near(self, x0, x1, z0, z1):
        found = set()
        for gx in range(math.floor(x0/CELL), math.floor(x1/CELL)+1):
            for gz in range(math.floor(z0/CELL), math.floor(z1/CELL)+1):
                found.update(self.grid.get((gx, gz), ()))
        return sorted(found)

    def height(self, i, x, z):
        _, _, n, offset = self.tris[i]
        return (offset-n[0]*x-n[2]*z)/n[1]

    def plan_distance(self, i, x, z):
        plan = self.tris[i][1]
        sign = 1 if _area2(plan) > 0 else -1
        outside = 0.0
        for a, b in zip(plan, plan[1:]+plan[:1]):
            ex, ez = b[0]-a[0], b[1]-a[1]
            side = sign*(ex*(z-a[1])-ez*(x-a[0]))/math.hypot(ex, ez)
            outside = max(outside, -side)
        if outside <= EDGE:
            return 0.0
        best = math.inf
        for a, b in zip(plan, plan[1:]+plan[:1]):
            ex, ez = b[0]-a[0], b[1]-a[1]
            t = max(0.0, min(1.0, ((x-a[0])*ex+(z-a[1])*ez)/(ex*ex+ez*ez)))
            best = min(best, math.hypot(x-a[0]-t*ex, z-a[1]-t*ez))
        return best

    def under(self, x, y, z):
        """(triangle, height) the corner drapes onto, or None. Up is +y (the supplied road normals point down)."""
        best = None
        for i in self.near(x, x, z, z):
            if self.plan_distance(i, x, z) > 0:
                continue
            h = self.height(i, x, z)
            if abs(h-y) <= ROAD_REACH and (best is None or abs(h-y) < abs(best[1]-y)):
                best = (i, h)
        if best is not None:
            return best
        nearest = None
        for i in self.near(x-PLAN_REACH, x+PLAN_REACH, z-PLAN_REACH, z+PLAN_REACH):
            d = self.plan_distance(i, x, z)
            h = self.height(i, x, z)
            if d <= PLAN_REACH and abs(h-y) <= ROAD_REACH and (nearest is None or (d, abs(h-y)) < nearest[0]):
                nearest = ((d, abs(h-y)), i, h)
        return None if nearest is None else nearest[1:]


def _clip(subject, plan):
    """Convex polygon subject [(x, z, ...)] clipped to the triangle plan; keeps the subject's winding."""
    sign = 1 if _area2(plan) > 0 else -1
    out = list(subject)
    for a, b in zip(plan, plan[1:]+plan[:1]):
        if not out:
            break
        ex, ez = b[0]-a[0], b[1]-a[1]
        inside = [sign*(ex*(p[1]-a[1])-ez*(p[0]-a[0])) for p in out]
        result = []
        for k, p in enumerate(out):
            q, sp, sq = out[(k+1) % len(out)], inside[k], inside[(k+1) % len(out)]
            if sp >= 0:
                result.append(p)
            if (sp >= 0) != (sq >= 0):
                t = sp/(sp-sq)
                result.append((p[0]+(q[0]-p[0])*t, p[1]+(q[1]-p[1])*t))
        out = result
    # Drop repeated points (a corner on an edge comes out twice).
    clean = []
    for p in out:
        if not clean or math.hypot(p[0]-clean[-1][0], p[1]-clean[-1][1]) > 1e-6:
            clean.append(p)
    while len(clean) > 1 and math.hypot(clean[0][0]-clean[-1][0], clean[0][1]-clean[-1][1]) <= 1e-6:
        clean.pop()
    return clean


def drape(pack, decal_materials, surface, skip_meshes=(), cut=math.inf, uncut='raise', lift_height=LIFT_HEIGHT,
          corner_mode='keep'):
    """In place (module text). decal_materials: the road-decal materials, surface: the other road-surface
    materials (prepare_face_order.scenery_classes); skip_meshes: meshes that are not scenery cells (world
    objects); cut: a face whose plane lies deeper than this under a road triangle is cut (its triangles then to
    TOLERANCE); lift_height: the camera height above the road the per-face lift of an uncut face is made for.
    Returns (report, the source faces of the decals, extra layers per source face of an uncut face)."""
    faces, meshes = pack['faces'], pack['meshes']
    origin = pack.setdefault('origin', list(range(len(faces))))
    records = [list(pm.VERTEX.unpack_from(pack['vertices'], i*pm.VERTEX.size)) for i in range(pack['header'][3])]
    triangles = []
    for m, mesh in enumerate(meshes):
        if m in skip_meshes:
            continue
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = faces[f]
            if face[4] in surface:
                p = [records[mesh[0]+i][:3] for i in face[:face[5]]]
                triangles += [(f, (p[0], p[t], p[t+1])) for t in range(1, face[5]-1)]
    road = Road(triangles)
    report = Counter()
    shifts, left_under, before_under = [], [], []
    # 1. Corners.
    moved = {}
    for m, mesh in enumerate(meshes):
        if m in skip_meshes:
            continue
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = faces[f]
            if face[4] not in decal_materials:
                continue
            for i in face[:face[5]]:
                v = mesh[0]+i
                if v in moved:
                    continue
                x, y, z = records[v][:3]
                found = road.under(x, y, z)
                if found is None:
                    moved[v] = None
                    report['corners_left'] += 1
                    continue
                h = _f32(found[1]) if corner_mode == 'snap' else y
                moved[v] = h
                shifts.append(h-y)   # up is +y: positive = the corner rises
    users = defaultdict(set)
    for m, mesh in enumerate(meshes):
        for f in range(mesh[2], mesh[2]+mesh[3]):
            for i in faces[f][:faces[f][5]]:
                users[mesh[0]+i].add(faces[f][4] in decal_materials)
    if any(len(users[v]) > 1 for v in moved):
        raise ValueError('a road-decal vertex record is shared with another face')
    for v, h in moved.items():
        if h is not None:
            records[v][1] = h
    report['corners_draped'] = sum(1 for h in moved.values() if h is not None)
    # 2. Creases: what lies under the road after step 1, per fan triangle of every decal face.
    plans = {}
    for m, mesh in enumerate(meshes):
        if m in skip_meshes:
            continue
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = faces[f]
            if face[4] not in decal_materials:
                continue
            corners = [records[mesh[0]+i] for i in face[:face[5]]]
            plan_box = (min(c[0] for c in corners), max(c[0] for c in corners), min(c[2] for c in corners),
                        max(c[2] for c in corners))
            candidates = [t for t in road.near(*plan_box)
                          if abs(road.height(t, *[sum(c[k] for c in corners)/len(corners) for k in (0, 2)]) -
                                 sum(c[1] for c in corners)/len(corners)) <= LAYER_REACH]
            fans = []
            for t in range(1, face[5]-1):
                tri = (corners[0], corners[t], corners[t+1])
                idx = (face[0], face[t], face[t+1])
                plan = [(c[0], c[2]) for c in tri]
                area = _area2(plan)
                pieces, deepest = [], 0.0
                if abs(area) > 1e-9:
                    # Decal plane over the plan: y = a*x + b*z + c.
                    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = (c[:3] for c in tri)
                    det = (x1-x0)*(z2-z0)-(x2-x0)*(z1-z0)
                    a = ((y1-y0)*(z2-z0)-(y2-y0)*(z1-z0))/det
                    b = ((x1-x0)*(y2-y0)-(x2-x0)*(y1-y0))/det
                    covered = 0.0
                    for r in candidates:
                        piece = _clip(plan, road.tris[r][1])
                        if len(piece) < 3 or abs(_area2(piece)) < 2*SLIVER:
                            continue
                        covered += abs(_area2(piece))
                        for px, pz in piece:
                            under = road.height(r, px, pz)-(y0+a*(px-x0)+b*(pz-z0))   # > 0: decal below the road (up is +y)
                            deepest = max(deepest, under)
                        pieces.append((r, piece))
                    # Road faces meet with float gaps and overlaps of a few 1e-4 units at cell borders.
                    exact = abs(covered-abs(area)) <= COVER*abs(area)
                else:
                    exact = False
                before_under.append(deepest)
                fans.append((tri, idx, plan, pieces, deepest, exact))
            whole = all(d <= cut for *_, d, _ in fans) or any(d > TOLERANCE and not ex for *_, d, ex in fans)
            plans[f] = (fans, whole)
    # An uncut face still under the road: its corners rise (uncut="raise") by its depth, so that it lies on or above
    # the road everywhere; a corner of several faces by the largest. Or the face gets extra layers (uncut="lift").
    rise = {}
    for f, (fans, whole) in plans.items():
        deepest = max(d for *_, d, _ in fans)
        if whole and deepest > TOLERANCE and uncut == 'raise':
            mesh = meshes[next(m for m, mm in enumerate(meshes) if mm[2] <= f < mm[2]+mm[3])]
            for i in faces[f][:faces[f][5]]:
                rise[mesh[0]+i] = max(rise.get(mesh[0]+i, 0.0), deepest)
    for v, d in rise.items():
        records[v][1] = _f32(records[v][1]+d)
    report['corners_raised'] = len(rise)
    raised = sorted(rise.values())
    # 3. Faces: whole (draped, raised) or cut into pieces on the road triangles.
    new_faces, new_origin, new_meshes, draped, lift = [], [], [], set(), {}
    vertices_out, refs_out = bytearray(), []
    groups = bytearray(pack['groups'])
    grow = defaultdict(list)    # group -> points
    for m, mesh in enumerate(meshes):
        mesh = list(mesh)
        raw = [pm.VERTEX.pack(*records[mesh[0]+i]) for i in range(mesh[1])]
        by_raw = {}
        for i, record in enumerate(raw):
            by_raw.setdefault(record, i)
        first_face = len(new_faces)
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = faces[f]
            if m in skip_meshes or face[4] not in decal_materials:
                new_faces.append(face)
                new_origin.append(origin[f])
                continue
            draped.add(origin[f])
            corners = [records[mesh[0]+i] for i in face[:face[5]]]
            fans, whole = plans[f]
            if whole:
                # Whole: on the road within the cut depth, or not exactly covered by road triangles.
                if any(d > cut for *_, d, _ in fans):
                    report['faces_left_uncovered'] += 1
                deepest = max(d for *_, d, _ in fans)
                left_under.append(deepest if uncut == 'lift' else 0.0)
                if deepest > TOLERANCE and uncut == 'lift':
                    # Lifted further, so that it still wins over the road seen from lift_height above it.
                    lift[origin[f]] = math.ceil(1024*deepest/lift_height)
                    report['faces_lifted'] += 1
                elif deepest > TOLERANCE:
                    report['faces_raised'] += 1
                new_faces.append(face)
                new_origin.append(origin[f])
                for c in corners:
                    grow[face[11]].append(c[:3])
                continue
            report['faces_cut'] += 1
            for tri, idx, plan, pieces, deepest, exact in fans:
                if deepest <= TOLERANCE:
                    new_faces.append((*idx, 0, face[4], 3, face[6], (face[7] & ~3) | 2, *face[8:]))
                    new_origin.append(origin[f])
                    report['triangles_kept'] += 1
                    for c in tri:
                        grow[face[11]].append(c[:3])
                    continue
                report['triangles_cut'] += 1
                (x0, _, z0), (x1, _, z1), (x2, _, z2) = (c[:3] for c in tri)
                det = (x1-x0)*(z2-z0)-(x2-x0)*(z1-z0)
                for r, piece in pieces:
                    index = []
                    for px, pz in piece:
                        w1 = ((px-x0)*(z2-z0)-(x2-x0)*(pz-z0))/det
                        w2 = ((x1-x0)*(pz-z0)-(px-x0)*(z1-z0))/det
                        w = (1-w1-w2, w1, w2)
                        uv = [round(sum(w[n]*tri[n][3+k] for n in range(3))) for k in range(2)]
                        if not all(0 <= c <= 65535 for c in uv):
                            raise ValueError('draped vertex UV out of range')
                        record = pm.VERTEX.pack(px, road.height(r, px, pz), pz, *uv)
                        if record not in by_raw:
                            by_raw[record] = len(raw)
                            raw.append(record)
                        if not index or index[-1] != by_raw[record]:
                            index.append(by_raw[record])
                    while len(index) > 1 and index[0] == index[-1]:
                        index.pop()
                    if len(index) < 3:
                        report['pieces_dropped'] += 1
                        continue
                    left_under.append(0.0)
                    runs = [index[:4]]
                    rest = index[3:]
                    while len(rest) > 1:   # fan of quads from the first corner
                        runs.append([index[0]]+rest[:3])
                        rest = rest[2:]
                    for members in runs:
                        new_faces.append((*members, *([0]*(4-len(members))), face[4], len(members), face[6],
                                          (face[7] & ~3) | (1 if len(members) == 4 else 2), *face[8:]))
                        new_origin.append(origin[f])
                        report['pieces'] += 1
                        for v in members:
                            grow[face[11]].append(struct.unpack('<3f', raw[v][:12]))
        if len(raw) > 65536:
            raise ValueError('a mesh exceeds 65536 vertices after draping')
        refs, first = [], {}
        for i, record in enumerate(raw):
            refs.append(first.setdefault(record[:12], i))
        points = [struct.unpack('<3f', record[:12]) for record in raw]
        if points and new_faces[first_face:]:
            used = [points[i] for face in new_faces[first_face:] for i in face[:face[5]]]
            bounds = [min(p[k] for p in used) for k in range(3)]+[max(p[k] for p in used) for k in range(3)]
            mesh[4:10] = [min(mesh[4+k], bounds[k]) for k in range(3)]+[max(mesh[7+k], bounds[3+k]) for k in range(3)]
        mesh[0], mesh[1] = len(refs_out), len(raw)
        mesh[2], mesh[3] = first_face, len(new_faces)-first_face
        vertices_out += b''.join(raw)
        refs_out += refs
        new_meshes.append(mesh)
    if groups and pm.GROUP.size == 64:
        for group, points in grow.items():
            values = list(pm.GROUP.unpack_from(groups, group*pm.GROUP.size))
            changed = False
            for k in range(3):
                low, high = min(p[k] for p in points), max(p[k] for p in points)
                if low < values[k]:
                    values[k], changed = low, True
                if high > values[3+k]:
                    values[3+k], changed = high, True
            if changed:
                report['group_bounds_grown'] += 1
            pm.GROUP.pack_into(groups, group*pm.GROUP.size, *values)
        # Facing spheres: corner 2 of every face of the group.
        facing = defaultdict(list)
        for m, mesh in enumerate(new_meshes):
            for f in range(mesh[2], mesh[2]+mesh[3]):
                face = new_faces[f]
                if face[4] in decal_materials:
                    facing[face[11]].append(struct.unpack_from('<3f', vertices_out, (mesh[0]+face[2])*pm.VERTEX.size))
        for group, points in facing.items():
            values = list(pm.GROUP.unpack_from(groups, group*pm.GROUP.size))
            radius = max(math.dist(p, values[10:13]) for p in points)
            if radius > values[13]:
                values[13] = pm.outward_float(radius)
                pm.GROUP.pack_into(groups, group*pm.GROUP.size, *values)
                report['facing_spheres_grown'] += 1
    report['faces_before'] = sum(1 for f in faces if f[4] in decal_materials)
    report['faces_after'] = sum(1 for f in new_faces if f[4] in decal_materials)
    pack['faces'], pack['origin'], pack['meshes'] = new_faces, new_origin, new_meshes
    pack['vertices'], pack['refs'] = bytes(vertices_out), struct.pack(f'<{len(refs_out)}H', *refs_out)
    pack['groups'] = bytes(groups)
    pack['header'][3], pack['header'][4] = len(refs_out), len(new_faces)
    shifts.sort()
    return dict(sorted(report.items()), rise_min=round(shifts[0], 4) if shifts else 0.0,
                rise_max=round(shifts[-1], 4) if shifts else 0.0,
                rise_median=round(shifts[len(shifts)//2], 4) if shifts else 0.0,
                deepest_under_road_before_cuts=round(max(before_under, default=0.0), 4),
                deepest_under_road_left=round(max(left_under, default=0.0), 6),
                tolerance=TOLERANCE, cut=cut, lift_height=lift_height,
                extra_layers_max=max(lift.values(), default=0), uncut=uncut, corners=corner_mode,
                raise_max=round(raised[-1], 4) if raised else 0.0,
                raise_median=round(raised[len(raised)//2], 4) if raised else 0.0), draped, lift
