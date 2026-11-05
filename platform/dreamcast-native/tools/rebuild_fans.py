"""Triangle fans of flat scenery rebuilt without their hub.

Used by prepare_face_order.py --rebuild-fans (rebuild_fans()); run on its own
it only surveys a pack (any DNM2..6, also the car pack) and changes nothing:

python3 platform/dreamcast-native/tools/rebuild_fans.py PACK.dnm [--min-triangles N]
    [--hub-angle DEG] [--plane TOL] [--uv TOL] [--prefer angle|span]

The arcade models a flat region with a curved edge (an infield, a run-off) as
a fan: many long thin triangles from one hub vertex to a rim. A tile renderer
pays for each of them in every tile its bounding box touches, the hub's tile
gets all of them, and they do not strip. Where the fan is ONE surface (one
material, one baked light, one side rule, one face group, one plane, one
affine texture mapping) its triangles are an accident of modelling and any
triangulation of its outline draws the same picture.

Detection, per mesh and hub position:
  hub triangle  a triangle of a face's fan split (0,k,k+1) with the hub as a
                corner and at most --fan-hub-angle (30 degrees) there: the
                corner of an ordinary quad mesh is not a fan
  fan           faces with the same material, supplied normal (hence baked
                luma), two-sided bit and face group that are joined through
                edges at the hub, with at least --fan-min-triangles (3) hub
                triangles between them
A fan is left alone, and counted in the report under the reason, if
  not_coplanar    a corner lies more than --fan-plane (1e-3 model units) off
                  the fan's plane (area-weighted geometric normal)
  layered         a member takes part in a coplanar layer pair (find_layers):
                  the arcade order among such faces is kept, not rebuilt
  outline         the union of the members is not one simple polygon
  uv_seam         neighbours disagree at a shared corner by more than whole
                  texture periods (twice the size on a mirrored axis)
  uv_not_affine   the texture coordinates, unwrapped over the periods, leave
                  more than --fan-uv (1.0 = 1/8 texel, the vertex format's
                  unit; they were rounded to it) around one affine mapping
  uv_range        the unwrapped coordinates do not fit 16 bits
  triangulation   no valid triangulation of the outline was found
  unchanged       the rebuilt outline is no better (below)

Rebuilding. The outline keeps every one of its vertices, also collinear
ones, with their exact position bytes: neighbouring faces meet the same
vertices as before, so there is neither a crack nor a T-junction. The hub
stays only where it is a vertex of the outline.
  convex outline   a zigzag strip v0, v1, vN-1, v2, vN-2, ...: no common
                   vertex. Of the N possible starts the one that is best
                   under --fan-prefer is taken.
  otherwise, or with Steiner points: ear clipping, then edge flips to the
                   constrained Delaunay triangulation (it maximises the
                   minimum angle), exact predicates.
--fan-prefer angle (default): the largest minimum angle; span: the smallest
sum of the triangles' longest edges (a proxy for bounding-box tile entries).
The fan is rebuilt if fewer triangles meet the hub or this measure improves.
Triangles sharing an edge are paired into strictly convex quads split on that
edge (a pack quad is the fan (0,1,2),(0,2,3)), so the pack's strip pass can
chain them. Winding follows the members; material, attribute, normal, group
and baked light are the first member's (they are equal by detection); the
new faces take the first member's place in the source order and its source
index in pack['origin']. Group bounds enclose the new faces by construction
(they lie inside the union of members of that group); the groups' facing
spheres are enlarged as split_cutouts.py does.

Steiner points (both off by default; they add vertices and triangles):
--fan-steiner PITCH  interior points of a rebuilt outline on a grid of PITCH
                     model units, at least 0.4 PITCH from the outline. The
                     point lies in the fan's plane and takes the affine
                     mapping's texture coordinate rounded to 1/8 texel.
--fan-spokes N       N points on every hub edge that is shared only by faces
                     of rebuilt fans of that hub (any material, group or
                     mesh): both sides get the identical points, so fans of
                     different materials around one hub can be cut into
                     shorter pieces. Fans with a single hub triangle take
                     part then, around hubs where at least --fan-spoke-site
                     (8) hub triangles of any material lie in one plane.
                     An edge that any other face uses stays whole.
"""
import argparse
from collections import Counter, defaultdict
from fractions import Fraction
import json
import math
from pathlib import Path
import struct

import prepare_models as pm

FACE6 = struct.Struct('<5HBBI3fHBB')


def _f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def _sub(a, b):
    return (a[0]-b[0], a[1]-b[1], a[2]-b[2])


def _dot(a, b):
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]


def _cross3(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def _orient(a, b, c):
    return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])


def _between(a, b, r):
    """r lies strictly inside segment a-b (exact coordinates)."""
    if _orient(a, b, r) != 0:
        return False
    d = (r[0]-a[0])*(b[0]-a[0])+(r[1]-a[1])*(b[1]-a[1])
    return 0 < d < (b[0]-a[0])**2+(b[1]-a[1])**2


def _angles(a, b, c):
    """Corner angles in degrees of a triangle given in metric 2D or 3D."""
    out = []
    for p, q, r in ((a, b, c), (b, c, a), (c, a, b)):
        u, v = [q[k]-p[k] for k in range(len(p))], [r[k]-p[k] for k in range(len(p))]
        lu, lv = math.sqrt(sum(x*x for x in u)), math.sqrt(sum(x*x for x in v))
        if lu == 0 or lv == 0:
            return [0.0, 0.0, 180.0]
        out.append(math.degrees(math.acos(max(-1.0, min(1.0, sum(x*y for x, y in zip(u, v))/(lu*lv))))))
    return out


def aspect(a, b, c):
    """Longest edge over the height on it: 1.15 equilateral, large for slivers."""
    edges = [math.dist(a, b), math.dist(b, c), math.dist(c, a)]
    s = sum(edges)/2
    area = math.sqrt(max(s*(s-edges[0])*(s-edges[1])*(s-edges[2]), 0.0))
    return max(edges)**2/(2*area) if area > 1e-12 else float('inf')


def _fan_triangles(corners):
    return [(corners[0], corners[k], corners[k+1]) for k in range(1, len(corners)-1)]


class _Shape:
    """Points of one fan: exact 2D (predicates), metric 2D (angles), 3D, UV."""

    def __init__(self):
        self.exact, self.metric, self.position, self.uv, self.raw = [], [], [], [], []

    def add(self, exact, metric, position, uv, raw):
        self.exact.append(exact); self.metric.append(metric); self.position.append(position)
        self.uv.append(uv); self.raw.append(raw)
        return len(self.exact)-1

    def orient(self, a, b, c):
        return _orient(self.exact[a], self.exact[b], self.exact[c])

    def min_angle(self, t):
        return min(_angles(*(self.metric[i] for i in t)))

    def longest(self, t):
        m = [self.metric[i] for i in t]
        return max(math.dist(m[0], m[1]), math.dist(m[1], m[2]), math.dist(m[2], m[0]))


def _valid(shape, triangles, points):
    """Positive triangles and no point inside an edge of a triangle."""
    for t in triangles:
        if shape.orient(*t) <= 0:
            return False
        for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            if any(r not in t and _between(shape.exact[a], shape.exact[b], shape.exact[r]) for r in points):
                return False
    return True


def _score(shape, triangles, prefer):
    if prefer == 'span':
        return -sum(shape.longest(t) for t in triangles)
    return min(shape.min_angle(t) for t in triangles)


def _zigzag(shape, loop, prefer):
    """Best valid zigzag strip of a convex loop, or None."""
    n, best = len(loop), None
    for start in range(n):
        ring = loop[start:]+loop[:start]
        order, a, b = [ring[0]], 1, n-1
        while a <= b:
            order.append(ring[a]); a += 1
            if a <= b:
                order.append(ring[b]); b -= 1
        triangles = [(order[i+1], order[i], order[i+2]) if i & 1 else (order[i], order[i+1], order[i+2])
                     for i in range(n-2)]
        if not _valid(shape, triangles, loop):
            continue
        score = _score(shape, triangles, prefer)
        if best is None or score > best[0]+1e-9:
            best = (score, triangles)
    return best[1] if best else None


def _ear_clip(shape, loop):
    ring, triangles = list(loop), []
    while len(ring) > 3:
        best = None
        for i in range(len(ring)):
            a, b, c = ring[i-1], ring[i], ring[(i+1) % len(ring)]
            if shape.orient(a, b, c) <= 0:
                continue
            if any(r not in (a, b, c) and shape.orient(a, b, r) >= 0 and shape.orient(b, c, r) >= 0 and
                   shape.orient(c, a, r) >= 0 for r in ring):
                continue
            angle = shape.min_angle((a, b, c))
            if best is None or angle > best[0]:
                best = (angle, i)
        if best is None:
            return None
        i = best[1]
        triangles.append((ring[i-1], ring[i], ring[(i+1) % len(ring)]))
        del ring[i]
    if shape.orient(*ring) <= 0:
        return None
    triangles.append(tuple(ring))
    return triangles


def _insert(shape, triangles, p):
    """Add interior point p: split the triangle, or the two at an edge, that holds it."""
    for index, t in enumerate(triangles):
        signs = [shape.orient(t[k], t[(k+1) % 3], p) for k in range(3)]
        if min(signs) < 0:
            continue
        zero = [k for k in range(3) if signs[k] == 0]
        if not zero:
            triangles[index:index+1] = [(t[k], t[(k+1) % 3], p) for k in range(3)]
            return True
        if len(zero) > 1:
            return False
        a, b, c = t[zero[0]], t[(zero[0]+1) % 3], t[(zero[0]+2) % 3]
        other = next((j for j, u in enumerate(triangles) if j != index and a in u and b in u), None)
        if other is None:
            return False
        d = next(v for v in triangles[other] if v not in (a, b))
        for j in sorted((index, other), reverse=True):
            del triangles[j]
        triangles += [(a, p, c), (p, b, c), (b, p, d), (p, a, d)]
        return True
    return False


def _delaunay(shape, triangles):
    """Edge flips while the smaller angle of a convex pair grows."""
    for _ in range(20*len(triangles)*len(triangles)+100):
        edges = {}
        for index, t in enumerate(triangles):
            for k in range(3):
                edges[t[k], t[(k+1) % 3]] = (index, t[(k+2) % 3])
        for (a, b), (i, c) in edges.items():
            if (b, a) not in edges or a > b:
                continue
            j, d = edges[b, a]
            # Triangles (a, b, c) and (b, a, d): the other diagonal is c-d.
            if shape.orient(c, a, d) <= 0 or shape.orient(d, b, c) <= 0:
                continue
            old = min(shape.min_angle((a, b, c)), shape.min_angle((b, a, d)))
            new = min(shape.min_angle((c, a, d)), shape.min_angle((d, b, c)))
            if new > old+1e-7:
                triangles[i], triangles[j] = (c, a, d), (d, b, c)
                break
        else:
            return triangles
    raise ValueError('edge flips do not settle')


def _pair(shape, triangles):
    """Quads (x, w, y, z): the fan (x,w,y),(x,y,z) is the two triangles."""
    faces, used = [], set()
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
            x, y, z = t[(k+1) % 3], t[(k+2) % 3], t[k]
            w = next(p for p in triangles[j] if p not in shared)
            quad = (x, w, y, z)
            if all(shape.orient(quad[n-1], quad[n], quad[(n+1) % 4]) > 0 for n in range(4)):
                faces.append(quad)
                used.update((i, j))
                break
        else:
            faces.append(t)
            used.add(i)
    return faces


def _mesh_tables(pack, mesh):
    records = [pm.VERTEX.unpack_from(pack['vertices'], (mesh[0]+i)*pm.VERTEX.size) for i in range(mesh[1])]
    raw = [pack['vertices'][(mesh[0]+i)*pm.VERTEX.size:(mesh[0]+i+1)*pm.VERTEX.size] for i in range(mesh[1])]
    return records, raw


def find_fans(pack, min_triangles=3, hub_angle=30.0):
    """Candidate fans: dicts with mesh, hub, key, members (pack face indices
    in source order) and hub_triangles. A face belongs to one fan at most;
    larger fans choose first."""
    size = pm.MATERIAL.size
    header0 = [struct.unpack_from('<H', pack['materials'], m*size)[0] for m in range(pack['header'][5])]
    found = []
    for mesh_index, mesh in enumerate(pack['meshes']):
        records, _ = _mesh_tables(pack, mesh)
        at_hub = defaultdict(list)
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = pack['faces'][f]
            if header0[face[4]] & 0x8000 or not face[7] & (3 << 8):
                continue
            corners = [records[i][:3] for i in face[:face[5]]]
            if len(set(corners)) != len(corners):
                continue
            hubs = defaultdict(list)
            for t in _fan_triangles(corners):
                for k in range(3):
                    hubs[t[k]].append(_angles(t[k], t[(k+1) % 3], t[(k+2) % 3])[0])
            key = (face[4], struct.pack('<3f', *face[8:11]), face[7] & (1 << 17), face[11], face[6])
            for hub, angles in hubs.items():
                if max(angles) <= hub_angle:
                    at = corners.index(hub)
                    at_hub[hub, key].append((f, len(angles), corners[at-1], corners[(at+1) % len(corners)]))
        candidates = []
        for (hub, key), members in at_hub.items():
            # Joined through edges at the hub.
            parent = list(range(len(members)))

            def root(i):
                while parent[i] != i:
                    parent[i] = parent[parent[i]]
                    i = parent[i]
                return i
            spoke = {}
            for i, (_, _, a, b) in enumerate(members):
                for other in (a, b):
                    if other in spoke:
                        parent[root(i)] = root(spoke[other])
                    spoke[other] = i
            parts = defaultdict(list)
            for i, member in enumerate(members):
                parts[root(i)].append(member)
            for part in parts.values():
                triangles = sum(m[1] for m in part)
                if triangles >= min_triangles:
                    candidates.append((triangles, hub, key, sorted(m[0] for m in part)))
        taken = set()
        for triangles, hub, key, members in sorted(candidates, key=lambda c: (-c[0], c[3])):
            if taken.isdisjoint(members):
                taken.update(members)
                found.append(dict(mesh=mesh_index, hub=hub, key=key, members=members, hub_triangles=triangles))
    found.sort(key=lambda fan: fan['members'][0])
    return found


def _outline(polygons):
    """The one simple boundary loop of faces given as position rings, or None."""
    edges = Counter()
    for p in polygons:
        for a, b in zip(p, p[1:]+p[:1]):
            edges[a, b] += 1
    if any(count > 1 for count in edges.values()):
        return None
    step = {}
    for a, b in edges:
        if (b, a) not in edges:
            if a in step:
                return None
            step[a] = b
    if len(step) < 3:
        return None
    start = next(iter(step))
    loop, at = [start], step[start]
    while at != start:
        if at not in step or len(loop) > len(step):
            return None
        loop.append(at)
        at = step[at]
    return loop if len(loop) == len(step) else None


def measure(pack, fan, plane=1e-3, uv_tolerance=1.0):
    """Check one candidate and prepare its outline. Returns (reason, None) or
    (None, data): data has the _Shape with the outline points, the loop
    (counter-clockwise in the shape's 2D; `flipped` if the members wind the
    other way), the plane and the affine texture mapping."""
    mesh = pack['meshes'][fan['mesh']]
    records, raw = _mesh_tables(pack, mesh)
    faces = [pack['faces'][f] for f in fan['members']]
    polygons = [[records[i][:3] for i in face[:face[5]]] for face in faces]
    normal = [0.0, 0.0, 0.0]
    for p in polygons:
        for a, b in zip(p, p[1:]+p[:1]):
            normal[0] += a[1]*b[2]-a[2]*b[1]
            normal[1] += a[2]*b[0]-a[0]*b[2]
            normal[2] += a[0]*b[1]-a[1]*b[0]
    length = math.sqrt(_dot(normal, normal))
    if length < 1e-12:
        return 'not_coplanar', None
    normal = tuple(v/length for v in normal)
    heights = [_dot(normal, q) for p in polygons for q in p]
    offset = 0.5*(min(heights)+max(heights))
    fan['plane_gap'] = max(heights)-offset
    if fan['plane_gap'] > plane:
        return 'not_coplanar', None
    loop = _outline(polygons)
    if loop is None:
        return 'outline', None
    # Texture coordinates unwrapped over whole periods, from the first member.
    header = struct.unpack_from('<4H', pack['materials'], faces[0][4]*pm.MATERIAL.size)
    textured = (header[0] >> 13) & 7 in (2, 3)
    period = (8*(32 << (header[0] & 7))*(2 if header[0] & 0x100 else 1),
              8*(32 << ((header[0] >> 3) & 7))*(2 if header[0] & 0x200 else 1))
    uvs = [[records[i][3:5] for i in face[:face[5]]] for face in faces]
    unwrapped, shift, queue = {}, {0: (0, 0)}, [0]
    if not textured:   # the coordinates mean nothing: first record of each position
        for p, uv in zip(polygons, uvs):
            for q, value in zip(p, uv):
                unwrapped.setdefault(q, value)
        shift, queue = {i: (0, 0) for i in range(len(polygons))}, []
    while queue:
        i = queue.pop()
        for q, uv in zip(polygons[i], uvs[i]):
            value = (uv[0]+shift[i][0], uv[1]+shift[i][1])
            if unwrapped.setdefault(q, value) != value:
                return 'uv_seam', None
        for j in range(len(polygons)):
            if j in shift:
                continue
            common = [q for q in polygons[j] if q in polygons[i]]
            if len(common) < 2:
                continue
            q = common[0]
            delta = tuple(unwrapped[q][k]-uvs[j][polygons[j].index(q)][k] for k in range(2))
            if delta[0] % period[0] or delta[1] % period[1]:
                return 'uv_seam', None
            shift[j] = delta
            queue.append(j)
    if len(shift) != len(polygons):
        return 'outline', None
    # Metric frame in the plane; exact 2D drops the normal's dominant axis.
    axis = max(range(3), key=lambda k: abs(normal[k]))
    kept = [k for k in range(3) if k != axis]
    e1 = _cross3(normal, tuple(1.0 if k == kept[0] else 0.0 for k in range(3)))
    scale = math.sqrt(_dot(e1, e1))
    e1 = tuple(v/scale for v in e1)
    e2 = _cross3(normal, e1)
    origin = loop[0]
    # (e1, e2, normal) is right-handed; the kept axes are mirrored against it
    # when this is negative, and then the first exact coordinate is negated.
    mirror = -1 if (axis == 1) != (normal[axis] < 0) else 1

    def exact(q):
        return (mirror*Fraction(q[kept[0]]), Fraction(q[kept[1]]))

    def metric(q):
        d = _sub(q, origin)
        return (_dot(d, e1), _dot(d, e2))
    # Affine fit of the unwrapped coordinates over the metric frame.
    points = list(unwrapped)
    rows = [(*metric(q), 1.0) for q in points]
    fit, residual = [], 0.0
    for channel in range(2):
        ata = [[sum(r[i]*r[j] for r in rows) for j in range(3)] for i in range(3)]
        atb = [sum(r[i]*unwrapped[q][channel] for r, q in zip(rows, points)) for i in range(3)]
        det = _dot(ata[0], _cross3(ata[1], ata[2]))
        if abs(det) < 1e-9:
            return 'not_coplanar', None
        solution = []
        for k in range(3):
            m = [row[:] for row in ata]
            for i in range(3):
                m[i][k] = atb[i]
            solution.append(_dot(m[0], _cross3(m[1], m[2]))/det)
        fit.append(solution)
        residual = max(residual, max(abs(_dot(solution, r)-unwrapped[q][channel]) for r, q in zip(rows, points)))
    fan['uv_residual'] = residual if textured else 0.0
    if textured and residual > uv_tolerance:
        return 'uv_not_affine', None
    move = [-(min(unwrapped[q][k] for q in points)//period[k])*period[k]
            if min(unwrapped[q][k] for q in points) < 0 else 0 for k in range(2)]
    if any(max(unwrapped[q][k] for q in points)+move[k] > 65535 for k in range(2)):
        return 'uv_range', None
    position_raw = {}
    for face in faces:
        for i in face[:face[5]]:
            position_raw.setdefault(records[i][:3], raw[i][:12])
    shape = _Shape()
    for q in loop:
        shape.add(exact(q), metric(q), q,
                  (unwrapped[q][0]+move[0], unwrapped[q][1]+move[1]), position_raw[q])
    ring = list(range(len(loop)))
    area = sum(shape.exact[a][0]*shape.exact[b][1]-shape.exact[b][0]*shape.exact[a][1]
               for a, b in zip(ring, ring[1:]+ring[:1]))
    if area == 0:
        return 'outline', None
    flipped = area < 0
    if flipped:
        ring.reverse()
    old = [t for p in polygons for t in _fan_triangles(p)]
    return None, dict(shape=shape, loop=ring, flipped=flipped, normal=normal, offset=offset, axis=axis, kept=kept,
                      metric=metric, exact=exact, fit=fit, move=move, old=old, textured=textured)


def _steiner(data, pitch):
    """Grid points inside the outline, at least 0.4 pitch from it."""
    shape, loop = data['shape'], data['loop']
    kept, axis, normal = data['kept'], data['axis'], data['normal']
    low = [min(shape.position[i][k] for i in loop) for k in kept]
    high = [max(shape.position[i][k] for i in loop) for k in kept]
    added = []
    for ix in range(math.ceil(low[0]/pitch), math.floor(high[0]/pitch)+1):
        for iy in range(math.ceil(low[1]/pitch), math.floor(high[1]/pitch)+1):
            p = [0.0, 0.0, 0.0]
            p[kept[0]], p[kept[1]] = _f32(ix*pitch), _f32(iy*pitch)
            p[axis] = _f32((data['offset']-normal[kept[0]]*p[kept[0]]-normal[kept[1]]*p[kept[1]])/normal[axis])
            exact = data['exact'](p)
            inside = False
            for a, b in zip(loop, loop[1:]+loop[:1]):
                a, b = shape.exact[a], shape.exact[b]
                if (a[1] > exact[1]) != (b[1] > exact[1]) and \
                        exact[0] < a[0]+(exact[1]-a[1])*(b[0]-a[0])/(b[1]-a[1]):
                    inside = not inside
            if not inside:
                continue
            m = data['metric'](p)
            near = float('inf')
            for a, b in zip(loop, loop[1:]+loop[:1]):
                a, b = shape.metric[a], shape.metric[b]
                d = (b[0]-a[0], b[1]-a[1])
                t = max(0.0, min(1.0, ((m[0]-a[0])*d[0]+(m[1]-a[1])*d[1])/(d[0]*d[0]+d[1]*d[1])))
                near = min(near, math.hypot(m[0]-a[0]-t*d[0], m[1]-a[1]-t*d[1]))
            if near < 0.4*pitch:
                continue
            uv = tuple(int(round(_dot(data['fit'][k], (*m, 1.0))))+data['move'][k] for k in range(2))
            if not data['textured']:
                uv = shape.uv[loop[0]]
            if not (0 <= uv[0] <= 65535 and 0 <= uv[1] <= 65535):
                continue
            added.append(shape.add(exact, m, tuple(p), uv, struct.pack('<3f', *p)))
    return added


def triangulate(data, prefer='angle', steiner=0.0):
    """Triangles (shape point indices, counter-clockwise) or None."""
    shape, loop = data['shape'], data['loop']
    inner = _steiner(data, steiner) if steiner > 0 else []
    convex = all(shape.orient(loop[i-1], loop[i], loop[(i+1) % len(loop)]) >= 0 for i in range(len(loop)))
    triangles = _zigzag(shape, loop, prefer) if convex and not inner else None
    if triangles is None:
        triangles = _ear_clip(shape, loop)
        if triangles is None:
            return None
        for p in inner:
            _insert(shape, triangles, p)
        triangles = _delaunay(shape, triangles)
        used = {i for t in triangles for i in t}
        if not _valid(shape, triangles, [i for i in loop+inner if i in used]) or not set(loop) <= used:
            return None
    # The stored float32 positions must give the same triangles.
    for t in triangles:
        m = [shape.metric[i] for i in t]
        if _orient(*m) <= 0:
            return None
    return triangles


def _add_spokes(pack, accepted, count):
    """Insert `count` points into every hub edge that only rebuilt fans use."""
    hubs = {fan['hub'] for fan, _ in accepted}
    census = Counter()
    for mesh in pack['meshes']:
        records, _ = _mesh_tables(pack, mesh)
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = pack['faces'][f]
            p = [records[i][:3] for i in face[:face[5]]]
            for a, b in zip(p, p[1:]+p[:1]):
                if a in hubs:
                    census[a, b] += 1
                if b in hubs:
                    census[b, a] += 1
    ours = Counter()
    for fan, data in accepted:
        shape, loop = data['shape'], data['loop']
        for a, b in zip(loop, loop[1:]+loop[:1]):
            for hub, other in ((a, b), (b, a)):
                if shape.position[hub] == fan['hub']:
                    ours[fan['hub'], shape.position[other]] += 1
    split = 0
    for fan, data in accepted:
        shape, loop, hub = data['shape'], data['loop'], fan['hub']
        ring = []
        for a, b in zip(loop, loop[1:]+loop[:1]):
            ring.append(a)
            other = b if shape.position[a] == hub else a if shape.position[b] == hub else None
            if other is None or census[hub, shape.position[other]] != ours[hub, shape.position[other]]:
                continue
            at = a if other == b else b
            points = []
            for n in range(1, count+1):
                t = Fraction(n, count+1)
                p = tuple(_f32(hub[k]+(shape.position[other][k]-hub[k])*float(t)) for k in range(3))
                exact = tuple(shape.exact[at][k]+(shape.exact[other][k]-shape.exact[at][k])*t for k in range(2))
                uv = tuple(int(round(shape.uv[at][k]+(shape.uv[other][k]-shape.uv[at][k])*float(t))) for k in range(2))
                points.append(shape.add(exact, data['metric'](p), p, uv, struct.pack('<3f', *p)))
            ring += points if other == b else points[::-1]
            split += 1
        data['loop'] = ring
        data['spokes'] = len(ring)-len(loop)
    return split


def rebuild_fans(pack, pairs=(), min_triangles=3, hub_angle=30.0, plane=1e-3, uv_tolerance=1.0, prefer='angle',
                 steiner=0.0, spokes=0, spoke_site=8, apply=True):
    """Rebuild the fans of a pack (prepare_face_order.read_pack tables) in
    place: see the module text. pairs: find_layers' evidence for this pack
    (current face indices); faces named there are left alone. apply=False
    only reports. Returns the report; pack['origin'] maps faces to source
    faces."""
    faces = pack['faces']
    origin = pack.setdefault('origin', list(range(len(faces))))
    layered = {f for pair in pairs for f in pair[:2]}
    report = Counter()
    fans = find_fans(pack, 1 if spokes else min_triangles, hub_angle)
    if spokes:
        # Single hub triangles only around a hub that is a fan whatever the material.
        hubs = {tuple(site['hub']) for site in sites(pack, spoke_site, hub_angle, plane) if site['coplanar']}
        fans = [fan for fan in fans if fan['hub_triangles'] >= min_triangles or fan['hub'] in hubs]
    report['candidates'] = len(fans)
    accepted, log = [], []
    for fan in fans:
        reason = 'layered' if not layered.isdisjoint(fan['members']) else None
        data = None
        if reason is None:
            reason, data = measure(pack, fan, plane, uv_tolerance)
        fan['reason'] = reason
        if reason is None:
            accepted.append((fan, data))
        else:
            report['left_'+reason] += 1
    if spokes:
        report['spoke_edges_split'] = _add_spokes(pack, accepted, spokes)
    plans = {}
    for fan, data in accepted:
        shape = data['shape']
        hub = fan['hub']
        forced = data.get('spokes', 0) > 0
        triangles = triangulate(data, prefer, steiner)
        if triangles is None:
            if forced:
                raise ValueError(f'fan at face {fan["members"][0]}: split spokes but no triangulation')
            fan['reason'] = 'triangulation'
            report['left_triangulation'] += 1
            continue
        old = data['old']
        old_m = [[data['metric'](q) for q in t] for t in old]
        new_m = [[shape.metric[i] for i in t] for t in triangles]
        hub_before = sum(1 for t in old if hub in t)
        hub_after = sum(1 for t in triangles if any(shape.position[i] == hub for i in t))
        if prefer == 'span':
            better = sum(max(math.dist(t[0], t[1]), math.dist(t[1], t[2]), math.dist(t[2], t[0])) for t in new_m) < \
                sum(max(math.dist(t[0], t[1]), math.dist(t[1], t[2]), math.dist(t[2], t[0])) for t in old_m)-1e-6
        else:
            better = min(min(_angles(*t)) for t in new_m) > min(min(_angles(*t)) for t in old_m)+1e-6
        if not forced and fan['hub_triangles'] < min_triangles:
            fan['reason'] = 'small'
            continue
        if not forced and not better and hub_after >= hub_before:
            fan['reason'] = 'unchanged'
            report['left_unchanged'] += 1
            continue
        fan.update(triangles_before=len(old), triangles_after=len(triangles), hub_before=hub_before, hub_after=hub_after,
                   aspect_before=[aspect(*t) for t in old_m], aspect_after=[aspect(*t) for t in new_m],
                   min_angle_before=min(min(_angles(*t)) for t in old_m),
                   min_angle_after=min(min(_angles(*t)) for t in new_m),
                   outline_vertices=len(data['loop']), steiner_points=len(shape.exact)-len(data['loop']),
                   spoke_points=data.get('spokes', 0))
        plans[fan['members'][0]] = (fan, data, _pair(shape, triangles))
        report['fans_rebuilt'] += 1
    for fan in fans:
        entry = dict(face=origin[fan['members'][0]], members=[origin[f] for f in fan['members']], mesh=fan['mesh'],
                     hub=list(fan['hub']), material=fan['key'][0],
                     header=list(struct.unpack_from('<4H', pack['materials'], fan['key'][0]*pm.MATERIAL.size)),
                     hub_triangles=fan['hub_triangles'], result=fan['reason'] or 'rebuilt')
        for name in ('plane_gap', 'uv_residual', 'triangles_before', 'triangles_after', 'hub_before', 'hub_after',
                     'min_angle_before', 'min_angle_after', 'outline_vertices', 'steiner_points', 'spoke_points'):
            if name in fan:
                entry[name] = round(fan[name], 6) if isinstance(fan[name], float) else fan[name]
        for name in ('aspect_before', 'aspect_after'):
            if name in fan:
                entry[name+'_mean'] = round(sum(fan[name])/len(fan[name]), 3)
                entry[name+'_worst'] = round(max(fan[name]), 3)
        log.append(entry)
    settings = dict(min_triangles=min_triangles, hub_angle=hub_angle, plane=plane, uv_tolerance=uv_tolerance,
                    prefer=prefer, steiner=steiner, spokes=spokes, spoke_site=spoke_site)
    if not plans or not apply:
        return dict(report, settings=settings, fans=log)
    dropped = {f for fan, _, _ in plans.values() for f in fan['members'][1:]}
    new_faces, new_origin, new_meshes = [], [], []
    vertices_out, refs_out = bytearray(), []
    refs = struct.unpack(f'<{pack["header"][3]}H', pack['refs'])
    group_points, group_corners = defaultdict(list), defaultdict(list)
    for mesh in pack['meshes']:
        mesh = list(mesh)
        _, raw = _mesh_tables(pack, mesh)
        local_refs = list(refs[mesh[0]:mesh[0]+mesh[1]])
        by_raw, by_position = {}, {}
        for i, record in enumerate(raw):
            by_raw.setdefault(record, i)
            by_position.setdefault(record[:12], local_refs[i])
        first_face = len(new_faces)
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = faces[f]
            if f in dropped:
                continue
            if f not in plans:
                new_faces.append(face)
                new_origin.append(origin[f])
                continue
            fan, data, pieces = plans[f]
            shape = data['shape']
            index = {}
            for i in {i for piece in pieces for i in piece}:
                record = shape.raw[i]+struct.pack('<2H', *shape.uv[i])
                if record not in by_raw:
                    by_raw[record] = len(raw)
                    raw.append(record)
                    local_refs.append(by_position.setdefault(record[:12], by_raw[record]))
                    report['vertices_added'] += 1
                index[i] = by_raw[record]
            for piece in pieces:
                members = [index[i] for i in piece]
                if data['flipped']:
                    members.reverse()
                    if len(members) == 4:   # keep the fan diagonal on corners 0 and 2
                        members = members[1:]+members[:1]
                attribute = (face[7] & ~3) | (1 if len(members) == 4 else 2)
                new_faces.append((*members, *([0]*(4-len(members))), face[4], len(members), face[6], attribute, *face[8:]))
                new_origin.append(origin[f])
                group_points[face[11]].append(struct.unpack('<3f', raw[members[2]][:12]))
                group_corners[face[11]] += [struct.unpack('<3f', raw[i][:12]) for i in members]
            report['faces_before'] += len(fan['members'])
            report['faces_after'] += len(pieces)
            report['triangles_before'] += fan['triangles_before']
            report['triangles_after'] += fan['triangles_after']
        if len(raw) > 65536:
            raise ValueError('a mesh exceeds 65536 vertices after rebuilding fans')
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
            for p in group_corners[group]:
                if any(p[k] < values[k] or p[k] > values[3+k] for k in range(3)):
                    raise AssertionError('a rebuilt face leaves its group bounds')
    pack['faces'], pack['origin'], pack['meshes'] = new_faces, new_origin, new_meshes
    pack['vertices'], pack['refs'], pack['groups'] = bytes(vertices_out), struct.pack(f'<{len(refs_out)}H', *refs_out), bytes(groups)
    pack['header'][3], pack['header'][4] = len(refs_out), len(new_faces)
    return dict(report, settings=settings, fans=log)


def read_any(data):
    """Tables of a DNM2..6 pack, for the survey (DNM6: its own face record)."""
    h = list(pm.HEADER.unpack_from(data))
    bases = pm.table_offsets(h)
    six = (h[0] & ~pm.ALIGNED) == 0x364d4e44
    faces = []
    for i in range(h[4]):
        if six:
            face = FACE6.unpack_from(data, bases[2]+i*32)
            faces.append((*face[:6], 0, *face[7:11], face[11]))
        else:
            faces.append(pm.FACE.unpack_from(data, bases[2]+i*32))
    return dict(header=h, faces=faces, groups=b'',
                meshes=[list(pm.MESH.unpack_from(data, bases[0]+i*pm.MESH.size)) for i in range(h[2])],
                vertices=data[bases[1]:bases[1]+h[3]*pm.VERTEX.size],
                materials=data[bases[3]:bases[3]+h[5]*pm.MATERIAL.size],
                refs=struct.pack(f'<{h[3]}H', *([0]*h[3])))


def sites(pack, min_triangles=3, hub_angle=30.0, plane=1e-3):
    """Hubs whatever the material, over all meshes: per hub position and
    supplied normal the hub triangles (see the module text), if coplanar."""
    size = pm.MATERIAL.size
    header0 = [struct.unpack_from('<H', pack['materials'], m*size)[0] for m in range(pack['header'][5])]
    at_hub = defaultdict(list)
    for mesh_index, mesh in enumerate(pack['meshes']):
        records, _ = _mesh_tables(pack, mesh)
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = pack['faces'][f]
            if header0[face[4]] & 0x8000 or not face[7] & (3 << 8):
                continue
            corners = [records[i][:3] for i in face[:face[5]]]
            for t in _fan_triangles(corners):
                if len(set(t)) < 3:
                    continue
                for k in range(3):
                    if _angles(t[k], t[(k+1) % 3], t[(k+2) % 3])[0] <= hub_angle:
                        at_hub[mesh_index if pack.get('per_mesh') else 0, t[k], struct.pack('<3f', *face[8:11])].append(
                            (mesh_index, f, face[4], t))
    out = []
    for (_, hub, _), members in at_hub.items():
        if len(members) < min_triangles:
            continue
        normal = [0.0, 0.0, 0.0]
        for _, _, _, t in members:
            n = _cross3(_sub(t[1], t[0]), _sub(t[2], t[0]))
            normal = [normal[k]+n[k] for k in range(3)]
        length = math.sqrt(_dot(normal, normal))
        if length < 1e-12:
            continue
        normal = [v/length for v in normal]
        heights = [_dot(normal, q) for _, _, _, t in members for q in t]
        out.append(dict(hub=list(hub), triangles=len(members), faces=len({(m[0], m[1]) for m in members}),
                        meshes=sorted({m[0] for m in members}), materials=dict(Counter(m[2] for m in members)),
                        coplanar=(max(heights)-min(heights))/2 <= plane, plane_gap=round((max(heights)-min(heights))/2, 6),
                        aspect_mean=round(sum(aspect(*m[3]) for m in members)/len(members), 3),
                        aspect_worst=round(max(aspect(*m[3]) for m in members), 3)))
    return sorted(out, key=lambda site: -site['triangles'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Survey the triangle fans of a mesh pack; nothing is written')
    parser.add_argument('pack', type=Path)
    parser.add_argument('--min-triangles', type=int, default=3)
    parser.add_argument('--hub-angle', type=float, default=30.0)
    parser.add_argument('--plane', type=float, default=1e-3)
    parser.add_argument('--uv', type=float, default=1.0)
    parser.add_argument('--prefer', choices=('angle', 'span'), default='angle')
    parser.add_argument('--per-mesh', action='store_true',
                        help='sites: do not join meshes (models with their own matrices, e.g. cars)')
    args = parser.parse_args()
    tables = read_any(args.pack.read_bytes())
    tables['per_mesh'] = args.per_mesh
    result = rebuild_fans(tables, (), args.min_triangles, args.hub_angle, args.plane, args.uv, args.prefer, apply=False)
    result['sites_any_material'] = sites(tables, args.min_triangles, args.hub_angle, args.plane)
    print(json.dumps(result, indent=1))
