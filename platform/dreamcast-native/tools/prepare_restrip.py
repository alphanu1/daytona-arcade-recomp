"""Offline strips for a finished DNM6 pack: reorder faces inside their draw runs and chain them.

python3 platform/dreamcast-native/tools/prepare_restrip.py IN.dnm IN.dnt NEW_OUT.dnm
    [--rules exact|diagonal|unwrap|unwrap-diagonal] [--strict-winding] [--scope header|normal]
    [--keep-sources ADDRESS...] [--car-levels INTRO_DRIVE_DATA_H] [--order-map NEW.json]
    [--pixel-faces NEW.txt] [--report NEW.json]

The pass runs after every other packer step (prepare_face_order.py, prepare_palette_materials.py)
on the pack the console loads, with the material pack it is drawn with: the DNT decides which faces
share a PVR header (texture, palette bank, list, filter) and a UV mapping, and only such faces may
form one TA strip (a lit pack, the cars: one material). Vertices, materials, instances, position
references, face groups, the level-of-detail table and every face record's bytes stay as they are
(rule sets diagonal and unwrap change some faces, see below); only the order of faces inside a draw
run and the strip table change. A draw run is a maximal stretch of a mesh's faces with one material
and one layer inside one level-of-detail range (or outside every range): the order there was already
the packer's free choice (prepare_face_order.py sorts it by normal and strip length), so the picture
does not depend on it, and a strip stays inside one material, one layer and one range (meshpack.h;
track_surface.c keeps or drops a strip whole with its material). Screen-checker faces (material
header bit 15) and the meshes of --keep-sources instances (the car pack's shadow models, whose
outline nm_shadow_outline finds in face order) keep their order and strips.

Rule sets (what may share an edge in a strip):
  exact     (default) faces whose shared edge is the same two vertices, position and u,v byte for
            byte, with the same front header and UV mapping (and the same mapping for a two-luma
            face's rear): the triangles are the faces' own fan triangles (0,1,2),(0,2,3), in face
            and fan order. Colour is free: flat shading takes each triangle's colour from its last
            vertex, and the renderer writes each face's colour on the vertices that complete its
            triangles (model_submission.h, NM_FLAT_PROVOKING_LAST). Winding is free (every header
            has culling off, so a triangle is its three vertices; meshpack.h) unless
            --strict-winding. The frame's triangles are exactly the source pack's (simulator "set"
            digest, every frame).
  diagonal  as exact, and a quad whose four corners lie in one plane (PLANE_TOLERANCE) and whose
            texture mapping is affine over it (AFFINE_TOLERANCE) may be split on its other
            diagonal: the face's corner list is rotated by one, so its triangles change.
  unwrap    as exact, and the shared corners may differ by one whole-period shift of u, v on an
            axis where the binding's texture is not cropped (the PVR repeats it there; a mirrored
            axis repeats every two extents): the face takes the shift, through added vertex
            records (same position and position reference, u, v shifted; 0..65535).
  unwrap-diagonal  both.
  In exact arithmetic diagonal and unwrap draw the same pixels; in the simulator's single-precision
  model of the PVR's set-up (sim_main.c SIM_PIXEL_FACES, fed by --pixel-faces) they do not (depth
  bits, texels at texel boundaries): they are measurement options, not the default.
--scope normal chains only faces with one supplied normal (a lit pack's faces then keep one binding
through a strip at any light). Measurement only (not loadable or not exact): --measure position
(u, v ignored), --cross-material.

Greedy: a range is split by header and mapping; inside, a strip starts at one of the TRIES unused
faces with the fewest unused edge neighbours (the longest walk wins) and grows forward, each step
to the candidate with the fewest unused neighbours (ties: same face group, then pack order). Chains
are written longest first inside each header, headers in their order of first appearance, so the
list keeps its header changes. Report: per class (list and scenery stage, or car detail level)
faces, triangles, strips, and TA vertices if every face were drawn, before and after.
"""
import argparse
import hashlib
import itertools
import json
import math
from collections import defaultdict
from pathlib import Path
import struct

import prepare_models as pm

LAYERED = 0x364d4e44
FACE6 = struct.Struct('<5HBBI3fHBB')
STRIP = struct.Struct('<IHHI5I')
LOD = struct.Struct('<I3HBB3f2f')
BINDING = struct.Struct('<6HI')
PLANE_TOLERANCE = 1e-5     # corner 3 from the plane of corners 0,1,2, relative to the longest edge
AFFINE_TOLERANCE = 1/16.0  # u,v of corner 3 against the affine map of corners 0,1,2, in 1/8 texels
LOD_GROUPS = 4
TRIES = 64          # start faces tried per strip (the greedy's breadth)


def read_dnm6(data):
    h = list(pm.HEADER.unpack_from(data))
    magic, aligned = h[0] & ~pm.ALIGNED, bool(h[0] & pm.ALIGNED)
    if magic != LAYERED or h[1] != len(data):
        raise ValueError('prepare_restrip.py needs a DNM6 pack')
    offset = pm.HEADER.size
    tables = []

    def take(count, stride, legacy_alignment=1):
        nonlocal offset
        offset = (offset+(31 if aligned else legacy_alignment-1)) & -(32 if aligned else legacy_alignment)
        tables.append(data[offset:offset+count*stride])
        if len(tables[-1]) != count*stride:
            raise ValueError('truncated DNM table')
        offset += count*stride
    for count, record in zip(h[2:7], (pm.MESH, pm.VERTEX, FACE6, pm.MATERIAL, pm.INSTANCE)):
        take(count, record.size)
    take(h[3], 2)
    groups = b''
    if h[7]:
        take(h[7], pm.GROUP.size, 4)
        groups = tables.pop()
    strips, indices = [], []
    if h[12]:
        take(h[12], STRIP.size, 32)
        strips = [STRIP.unpack_from(tables[-1], i*STRIP.size)[:4] for i in range(h[12])]
        tables.pop()
        take(h[13], 2)
        indices = list(struct.unpack(f'<{h[13]}H', tables.pop()))
    lod = b''
    if h[15] & LOD_GROUPS:
        take(h[15] >> 16, LOD.size, 4)
        lod = tables.pop()
    meshes, vertices, faces, materials, instances, refs = tables
    return dict(header=h, aligned=aligned,
                meshes=[list(pm.MESH.unpack_from(meshes, i*pm.MESH.size)) for i in range(h[2])],
                vertices=vertices, materials=materials, instances=instances, refs=refs, groups=groups,
                faces=[list(FACE6.unpack_from(faces, i*FACE6.size)) for i in range(h[4])],
                strips=strips, indices=indices, lod=lod)


def write_dnm6(pack, faces, strips, indices, mesh_strips, added_vertices=None):
    """added_vertices: per mesh (vertex records, position references), the existing ones first."""
    h = list(pack['header'])
    meshes, vertices, refs, at = [], pack['vertices'], pack['refs'], 0
    if added_vertices:
        vertices = b''.join(b''.join(r) for r, _ in added_vertices)
        refs = b''.join(struct.pack(f'<{len(p)}H', *p) for _, p in added_vertices)
        h[3] = len(vertices)//pm.VERTEX.size
    for index, (mesh, (first, count)) in enumerate(zip(pack['meshes'], mesh_strips)):
        mesh = list(mesh)
        mesh[13], mesh[14] = (first, count) if strips else (0, 0)
        if added_vertices:
            mesh[0], mesh[1] = at, len(added_vertices[index][0])
            if mesh[1] > 8192:
                raise ValueError(f'mesh {index}: {mesh[1]} vertices after unwrapping (the renderer addresses 8192)')
            at += mesh[1]
        meshes.append(pm.MESH.pack(*mesh))
    out = bytearray(pm.HEADER.size)
    out += b''.join(meshes)+vertices+b''.join(FACE6.pack(*f) for f in faces)+pack['materials']
    out += pack['instances']+refs
    out += bytes((-len(out)) & 3)
    out += pack['groups']
    if strips:
        out += bytes((-len(out)) & 31)
        out += b''.join(STRIP.pack(first, count, vertices, at, 0, 0, 0, 0, 0) for first, count, vertices, at in strips)
        out += struct.pack(f'<{len(indices)}H', *indices)
    out += bytes((-len(out)) & 3)
    out += pack['lod']
    out += bytes((-len(out)) & 3)
    h[0], h[1] = LAYERED, len(out)
    h[12], h[13] = len(strips), len(indices)
    pm.HEADER.pack_into(out, 0, *h)
    out = bytes(out)
    return pm.align_tables(out) if pack['aligned'] else out


def read_dnt(data):
    """Bindings by (header words, luma): (index, resource, color, palette index) of a DNT2/4/6 pack."""
    magic, size, resources, count = struct.unpack_from('<4s3I', data)
    if magic not in (b'DNT2', b'DNT4', b'DNT6') or size != len(data):
        raise ValueError('strips need the uncompressed DNT2/4/6 material pack the mesh pack is drawn with')
    base = 32+16*resources
    palette = None
    if magic == b'DNT6':
        at = base+16*count+4*count
        palette = data[at:at+count]
    out = {}
    for i in range(count):
        b = BINDING.unpack_from(data, base+16*i)
        out[b[:4], b[4]] = (i, b[5], b[6], palette[i] if palette is not None else 0)
    return out


def header_word(pack, material):
    return struct.unpack_from('<4H', pack['materials'], material*pm.MATERIAL.size)


def material_class(pack, material):
    return struct.unpack_from('<I', pack['materials'], material*pm.MATERIAL.size+24)[0]


def lod_segments(pack, mesh):
    """Per mesh-local face: None outside every level-of-detail range, else (group, level)."""
    seg = [None]*mesh[3]
    lod = pack['lod']
    if not lod:
        return seg
    first, count = mesh[15] & 0xffff, mesh[15] >> 16
    for g in range(first, first+count):
        row = LOD.unpack_from(lod, g*LOD.size)
        at = row[0]-mesh[2]
        for level in range(3):
            for f in range(at, at+row[1+level]):
                seg[f] = (g, level)
            at += row[1+level]
    return seg


def _sets(seq):
    return [frozenset(seq[i:i+3]) for i in range(len(seq)-2)]


def _oriented(seq, parity):
    tris = []
    for i in range(len(seq)-2):
        a, b, c = (seq[i+1], seq[i], seq[i+2]) if (i+parity) & 1 else (seq[i], seq[i+1], seq[i+2])
        tris.append(min((a, b, c), (b, c, a), (c, a, b)))
    return tris


def face_orders(corners, strict):
    """Strip vertex orders of one face that give its fan triangles in fan order.

    Winding free: {parity: orders} with both parities alike; strict: per start parity."""
    if len(set(corners)) != len(corners):
        return {0: [], 1: []}
    fan = [(corners[0], corners[i], corners[i+1]) for i in range(1, len(corners)-1)]
    if not strict:
        want = [frozenset(t) for t in fan]
        orders = [p for p in itertools.permutations(corners) if _sets(p) == want]
        return {0: orders, 1: orders}
    want = [min((a, b, c), (b, c, a), (c, a, b)) for a, b, c in fan]
    return {parity: [p for p in itertools.permutations(corners) if _oriented(p, parity) == want] for parity in (0, 1)}


def quad_flat_affine(vertices, corners):
    """Planar quad (PLANE_TOLERANCE) with an affine u,v mapping (AFFINE_TOLERANCE)."""
    p = [vertices[c][0] for c in corners]
    t = [vertices[c][1] for c in corners]
    def sub(a, b): return [a[i]-b[i] for i in range(3)]
    def dot(a, b): return sum(a[i]*b[i] for i in range(3))
    def cross(a, b): return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
    e1, e2, e3 = sub(p[1], p[0]), sub(p[2], p[0]), sub(p[3], p[0])
    n = cross(e1, e2)
    nn = math.sqrt(dot(n, n))
    longest = max(math.sqrt(dot(sub(p[i], p[j]), sub(p[i], p[j]))) for i in range(4) for j in range(i))
    if nn == 0 or longest == 0:
        return False
    if abs(dot(e3, n))/nn > PLANE_TOLERANCE*longest:
        return False
    # Corner 3 in the barycentric frame of 0,1,2 (least squares in the plane), then its u,v.
    a11, a12, a22 = dot(e1, e1), dot(e1, e2), dot(e2, e2)
    b1, b2 = dot(e3, e1), dot(e3, e2)
    det = a11*a22-a12*a12
    if det == 0:
        return False
    s = (b1*a22-b2*a12)/det
    r = (a11*b2-a12*b1)/det
    for k in range(2):
        want = t[0][k]+s*(t[1][k]-t[0][k])+r*(t[2][k]-t[0][k])
        if abs(want-t[3][k]) > AFFINE_TOLERANCE:
            return False
    return True


def chain(members, options, neighbours, group_of, strict, uv=None):
    """Greedy strips over `members` (pack order). options[f]: [(rotation, {parity: orders})].
    Returns [(faces, rotations, strip vertex ids, shifts)].

    uv (rule set unwrap): uv[f][id] = (u, v) of the face's corner with that vertex id, where an
    id stands for a position and u, v modulo the texture's periods; a face continues a strip
    when its two shared corners are the strip's last two up to ONE whole-period shift, which the
    face then takes (shifts[i] = (du, dv) of the i-th face, u and v staying inside 0..65535)."""
    starts = defaultdict(list)
    for f in members:
        for rotation, by_parity in options[f]:
            for parity, orders in by_parity.items():
                for o in orders:
                    starts[parity, o[0], o[1]].append((f, rotation, o))
    unused = set(members)
    position = {f: i for i, f in enumerate(members)}
    free = {f: sum(1 for g in neighbours[f] if g in unused) for f in members}

    def take(f):
        unused.discard(f)
        for g in neighbours[f]:
            free[g] -= 1

    chains = []
    while unused:
        # Starts: the TRIES unused faces with the fewest unused neighbours; the longest walk wins.
        firsts = sorted(unused, key=lambda f: (free[f], position[f]))[:TRIES]
        best = None
        for first, (rotation, by_parity) in ((f, o) for f in firsts for o in options[f] or [(0, {0: []})]):
            for o in by_parity[0] or [None]:
                ids, rotations, shifts = [first], [rotation], [(0, 0)]
                strip = list(o) if o else None
                at = [uv[first][i] for i in o] if uv is not None and o else None   # absolute u, v of each strip vertex
                seen = {first}
                while strip and len(strip) < 65000:
                    parity = (len(strip)-2) & 1 if strict else 0
                    cands = []
                    for g, r, s in starts[parity, strip[-2], strip[-1]]:
                        if g not in unused or g in seen:
                            continue
                        shift = (0, 0)
                        if uv is not None:
                            a, b = uv[g][s[0]], uv[g][s[1]]
                            shift = (at[-2][0]-a[0], at[-2][1]-a[1])
                            if shift != (at[-1][0]-b[0], at[-1][1]-b[1]) or \
                                    any(not (0 <= u+shift[0] <= 65535 and 0 <= v+shift[1] <= 65535) for u, v in uv[g].values()):
                                continue
                        cands.append((g, r, s, shift))
                    if not cands:
                        break
                    g, r, s, shift = min(cands, key=lambda c: (free[c[0]]-1, group_of[c[0]] != group_of[ids[-1]], position[c[0]]))
                    ids.append(g); rotations.append(r); shifts.append(shift); seen.add(g); strip += s[2:]
                    if uv is not None:
                        at += [(uv[g][i][0]+shift[0], uv[g][i][1]+shift[1]) for i in s[2:]]
                walk = (ids, rotations, strip, shifts, at)
                if best is None or len(walk[0]) > len(best[0]):
                    best = walk
        for f in best[0]:
            take(f)
        chains.append(best)
    return chains


def strip_vertices(pack, faces, mesh, corners_of):
    return sum(len(corners_of(f))-2 for f in faces)+2 if len(faces) > 1 else len(corners_of(faces[0]))


def restrip(pack, dnt, rules='exact', strict=False, scope='header', levels=None, measure=None, extents=None, cross=False,
            keep_sources=()):
    """New face records, strips, per-mesh strip ranges, new->old face map, rotated faces, report."""
    vertices = [(pm.VERTEX.unpack_from(pack['vertices'], i*pm.VERTEX.size)[:3],
                 pm.VERTEX.unpack_from(pack['vertices'], i*pm.VERTEX.size)[3:]) for i in range(pack['header'][3])]
    raw = [pack['vertices'][i*pm.VERTEX.size:(i+1)*pm.VERTEX.size] for i in range(pack['header'][3])]
    baked = bool(pack['header'][15] & 1)
    faces = pack['faces']
    new_faces, order, strips, indices, mesh_strips, rotated, shifted, added_vertices = [], [], [], [], [], [], [], []
    changed = {}   # new face index -> (rotated, du, dv): what the simulator's pixel check (SIM_PIXEL_FACES) compares
    stats = defaultdict(lambda: defaultdict(int))
    old_strip_of = {}
    for first, count, vcount, at in pack['strips']:
        for f in range(first, first+count):
            old_strip_of[f] = (first, count)

    def klass(mesh_index, f):
        face = faces[f]
        th = header_word(pack, face[4])[0]
        lst = 'pt' if ((th >> 13) & 3) == 3 else 'op'
        if levels is not None:
            return f'car-{lst}-{levels.get(mesh_index, "other")}'
        stage = material_class(pack, face[4]) & 0xff
        return f'world-{lst}-stage{stage}'

    refs = struct.unpack(f'<{pack["header"][3]}H', pack['refs'])
    kept_meshes = {pm.INSTANCE.unpack_from(pack['instances'], i*pm.INSTANCE.size)[0] for i in range(pack['header'][6])
                   if pm.INSTANCE.unpack_from(pack['instances'], i*pm.INSTANCE.size)[1] in keep_sources}
    for mesh_index, mesh in enumerate(pack['meshes']):
        vf, vc, ff, fc = mesh[:4]
        canonical = {}
        vid = [canonical.setdefault(raw[vf+v], v) for v in range(vc)]
        by_bytes = {raw[vf+v]: v for v in reversed(range(vc))}
        mesh_raw, mesh_refs = [raw[vf+v] for v in range(vc)], list(refs[vf:vf+vc])
        added_vertices.append((mesh_raw, mesh_refs))

        def local(cid, uv_at, vf=vf, by_bytes=by_bytes, mesh_raw=mesh_raw, mesh_refs=mesh_refs):
            """Mesh-local index of the vertex at id cid's position with u, v uv_at (added if new)."""
            data = raw[vf+cid][:12]+struct.pack('<2H', *uv_at)
            if data not in by_bytes:
                by_bytes[data] = len(mesh_raw)
                mesh_raw.append(data); mesh_refs.append(refs[vf+cid])
            return by_bytes[data]
        if measure == 'position':   # measurement only: u,v ignored (not a valid pack)
            vid = [canonical.setdefault(('p', raw[vf+v][:12]), v) for v in range(vc)]
        seg = lod_segments(pack, mesh)
        first_strip = len(strips)
        # Old TA vertices if every face were drawn, per class.
        stats_mesh_before = sum(stats[k]['vertices_before'] for k in stats)
        f = ff
        while f < ff+fc:
            k = klass(mesh_index, f)
            if f in old_strip_of and old_strip_of[f][0] == f:
                s0, n0 = old_strip_of[f]
                stats[k]['vertices_before'] += sum(faces[g][5]-2 for g in range(s0, s0+n0))+2
                stats[k]['strips_before'] += 1; stats[k]['strip_faces_before'] += n0
                for g in range(s0, s0+n0):
                    stats[klass(mesh_index, g)]['faces'] += 1
                    stats[klass(mesh_index, g)]['triangles'] += faces[g][5]-2
                f += n0
                continue
            stats[k]['vertices_before'] += faces[f][5]; stats[k]['faces'] += 1; stats[k]['triangles'] += faces[f][5]-2
            f += 1
        stats_mesh_before = sum(stats[k]['vertices_before'] for k in stats)-stats_mesh_before
        if mesh_index in kept_meshes:
            # Face order kept (the shadow models: nm_shadow_outline walks their faces in order), strips as they were.
            assert len(order) == ff
            for first, count, vcount, at in pack['strips'][mesh[13]:mesh[13]+mesh[14]]:
                strips.append((first, count, vcount, len(indices)))
                indices += pack['indices'][at:at+vcount]
            for f in range(ff, ff+fc):
                order.append(f); new_faces.append(list(faces[f]))
            if fc:
                stats[klass(mesh_index, ff)]['vertices_after'] += stats_mesh_before
                stats[klass(mesh_index, ff)]['kept_meshes'] += 1
            mesh_strips.append((first_strip, len(strips)-first_strip))
            continue
        # Draw runs: one material, one layer, one level-of-detail range.
        runs = []
        for f in range(ff, ff+fc):
            face = faces[f]
            th = header_word(pack, face[4])[0]
            key = ((((th >> 13) & 3) == 3, bool(th & 0x8000)) if cross else face[4], face[12], seg[f-ff])
            if runs and runs[-1][0] == key and not (th & 0x8000):
                runs[-1][1].append(f)
            else:
                runs.append((key, [f]))
        for key, members in runs:
            if header_word(pack, faces[members[0]][4])[0] & 0x8000:   # screen checker: never drawn, kept as it is
                for f in members:
                    order.append(f); new_faces.append(list(faces[f]))
                continue
            # Header and mapping of each face's sides.
            def side_key(f):
                face = faces[f]
                material, words = face[4], header_word(pack, face[4])
                if not baked:
                    return (material,)+((tuple(face[8:11]),) if scope == 'normal' else ())
                b = dnt.get((words, face[6]))
                if b is None:
                    raise ValueError(f'face {f}: no binding for material {material} luma {face[6]}')
                k = (b[1], b[3], b[2])+((words[0] & 0x300, material_class(pack, material) & 0x300) if cross else (material,))
                if face[7] & (1 << 17):
                    back = dnt.get((words, face[13]))
                    if back is None:
                        raise ValueError(f'face {f}: no binding for its rear luma {face[13]}')
                    if (back[1], back[2]) != (b[1], b[2]):
                        k += ('rear', back[1], back[2])
                return k + ((tuple(face[8:11]),) if scope == 'normal' else ())
            by_key = defaultdict(list)
            for f in members:
                by_key[side_key(f)].append(f)
            for k in by_key:
                part = by_key[k]
                uv = None
                extent_of = (1, 1)
                if baked:
                    b = dnt[header_word(pack, faces[part[0]][4]), faces[part[0]][6]]
                    extent_of = extents[b[1]] if b[1] != 65535 else (1, 1)
                if rules in ('unwrap', 'unwrap-diagonal') and baked:
                    # u, v modulo the period of each axis on which the texture repeats whole: the
                    # binding's texture is not cropped there (prepare_palette_materials.py), so the
                    # PVR wraps it (u_clamp off) exactly where the arcade does; a mirrored axis
                    # (header bit 8/9, uv_flip) repeats every two extents.
                    words = header_word(pack, faces[part[0]][4])
                    b = dnt[words, faces[part[0]][6]]
                    periods = []
                    for axis in range(2):
                        original = 32 << ((words[0] >> (3*axis)) & 7)
                        extent = extents[b[1]][axis] if b[1] != 65535 else None
                        whole = extent == original and not (b[2] >> (16*axis)) & 0xffff
                        periods.append(extent*8*(2 if words[0] & (0x100 << axis) else 1) if whole else None)
                    def mid(i):
                        x = pm.VERTEX.unpack_from(raw[vf+i])
                        key = tuple(x[3+a] % periods[a] if periods[a] else x[3+a] for a in range(2))
                        return canonical.setdefault(('m', raw[vf+i][:12])+key, i)
                    corners = {f: tuple(mid(i) for i in faces[f][:faces[f][5]]) for f in part}
                    uv = {f: {mid(i): tuple(pm.VERTEX.unpack_from(raw[vf+i])[3:]) for i in faces[f][:faces[f][5]]} for f in part}
                    stats[klass(mesh_index, part[0])]['axes_unwrappable'] += sum(1 for p in periods if p)
                else:
                    corners = {f: tuple(vid[i] for i in faces[f][:faces[f][5]]) for f in part}
                options = {}
                for f in part:
                    c = corners[f]
                    opts = [(0, face_orders(c, strict))]
                    if rules in ('diagonal', 'unwrap-diagonal') and len(c) == 4 and len(set(c)) == 4 and \
                            quad_flat_affine(vertices, [vf+faces[f][i] for i in range(4)]):
                        opts.append((1, face_orders(c[1:]+c[:1], strict)))
                    options[f] = opts
                edges = defaultdict(set)
                for f in part:
                    c = corners[f]
                    for i in range(len(c)):
                        edges[frozenset((c[i], c[(i+1) % len(c)]))].add(f)
                neighbours = {f: set() for f in part}
                for shared in edges.values():
                    for f in shared:
                        neighbours[f] |= shared-{f}
                group_of = {f: faces[f][11] for f in part}
                chains = chain(part, options, neighbours, group_of, strict, uv)
                chains.sort(key=lambda c: (-len(c[0]), part.index(c[0][0])))
                for ids, rots, strip, shifts, at in chains:
                    if uv is not None and len(ids) > 1:
                        # The strip's vertices with their unwrapped u, v: existing records or new ones.
                        strip = [local(c, at[i]) for i, c in enumerate(strip)]
                    if len(ids) > 1:
                        strips.append((len(order), len(ids), len(strip), len(indices)))
                        indices += strip
                    for f, r, shift in zip(ids, rots, shifts):
                        face = list(faces[f])
                        if shift != (0, 0) or r:
                            # u, v in texture units: whole periods (the PVR's u = (u - crop)/(8*extent))
                            changed[len(order)] = (1 if r else 0, *(shift[a]//(8*extent_of[a]) for a in range(2)))
                            assert all(shift[a] % (8*extent_of[a]) == 0 for a in range(2))
                        if shift != (0, 0):
                            n = face[5]
                            face[:n] = [local(mid(i), tuple(pm.VERTEX.unpack_from(raw[vf+i])[3+a]+shift[a] for a in range(2))) for i in face[:n]]
                            shifted.append(len(order))
                        if r:
                            n = face[5]
                            face[:n] = face[r:n]+face[:r]
                            rotated.append(len(order))
                        order.append(f); new_faces.append(face)
                    kk = klass(mesh_index, ids[0])
                    stats[kk]['vertices_after'] += strip_vertices(pack, ids, mesh, lambda g: corners[g])
                    if len(ids) > 1:
                        stats[kk]['strips_after'] += 1; stats[kk]['strip_faces_after'] += len(ids)
                    stats[kk]['rotated'] += sum(1 for r in rots if r)
        mesh_strips.append((first_strip, len(strips)-first_strip))
    if sorted(order) != list(range(len(faces))):
        raise AssertionError('face order is not a permutation')
    report = {k: dict(v) for k, v in sorted(stats.items())}
    report['shifted_faces'] = len(shifted)
    report['changed'] = changed
    report['added_vertices'] = sum(len(r)-m[1] for (r, _), m in zip(added_vertices, pack['meshes']))
    return new_faces, order, strips, indices, mesh_strips, rotated, report, added_vertices


def car_levels(header_text):
    """Car mesh -> detail level from the generated intro_drive_data.h body table: L0 nearest."""
    import re
    body = re.search(r'nm_intro_body_models\[\]\[8\]\s*=\s*\{(.*?)\};', header_text, re.S)
    rows = [[int(v) for v in row.split(',')] for row in re.findall(r'\{([^{}]*)\}', body.group(1))]
    out = {}
    for row in rows[1:]:
        for at, name in ((0, 'L0'), (3, 'L1'), (4, 'L2'), (5, 'L3')):
            out.setdefault(row[at], name)
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('source', type=Path, help='DNM6 pack as the console loads it')
    parser.add_argument('materials', type=Path, help='the DNT2/4/6 pack it is drawn with')
    parser.add_argument('output', type=Path, help='NEW DNM6 pack')
    parser.add_argument('--rules', choices=('exact', 'diagonal', 'unwrap', 'unwrap-diagonal'), default='exact')
    parser.add_argument('--strict-winding', action='store_true', help='keep each triangle\'s winding (the strips of prepare_face_order.py)')
    parser.add_argument('--scope', choices=('header', 'normal'), default='header',
                        help='chain faces of one header (default) or also of one supplied normal')
    parser.add_argument('--measure', choices=('position',),
                        help='measurement only, not a valid pack: chain by position alone (u, v ignored)')
    parser.add_argument('--cross-material', action='store_true',
                        help='measurement only (a strip is one material: meshpack.h, track_surface.c): baked packs, chain faces '
                             'of different materials (same list and layer) that share header, mapping and depth class')
    parser.add_argument('--keep-sources', nargs='+', default=[], metavar='ADDRESS',
                        help='meshes of instances with these source addresses keep face order and strips (the car pack: the '
                             'shadow models 0x8d9e42 0x8d7ab7, whose outline nm_shadow_outline finds in face order)')
    parser.add_argument('--car-levels', type=Path, metavar='INTRO_DRIVE_DATA_H', help='report car classes by detail level')
    parser.add_argument('--order-map', type=Path, help='NEW JSON: new->old face indices and the rotated faces (new indices)')
    parser.add_argument('--report', type=Path, help='NEW JSON report (also printed)')
    parser.add_argument('--pixel-faces', type=Path,
                        help='NEW text file for the simulator\'s pixel check (SIM_PIXEL_FACES): faces whose triangles changed')
    args = parser.parse_args()
    pack = read_dnm6(args.source.read_bytes())
    dnt_bytes = args.materials.read_bytes()
    dnt = read_dnt(dnt_bytes)
    extents = [struct.unpack_from('<IIHH', dnt_bytes, 32+16*i)[2:] for i in range(struct.unpack_from('<I', dnt_bytes, 8)[0])]
    levels = car_levels(args.car_levels.read_text()) if args.car_levels else None
    new_faces, order, strips, indices, mesh_strips, rotated, report, added = restrip(
        pack, dnt, args.rules, args.strict_winding, args.scope, levels, args.measure, extents, args.cross_material,
        {int(v, 0) for v in args.keep_sources})
    changed = report.pop('changed')
    out = write_dnm6(pack, new_faces, strips, indices, mesh_strips, added if args.rules.startswith('unwrap') else None)
    total = defaultdict(int)
    for v in report.values():
        for k, n in (v.items() if isinstance(v, dict) else ()):
            total[k] += n
    result = dict(rules=args.rules, strict_winding=args.strict_winding, scope=args.scope, classes=report, total=dict(total),
                  strips=len(strips), strip_vertices=len(indices), rotated=len(rotated), bytes=len(out),
                  sha256=hashlib.sha256(out).hexdigest())
    with args.output.open('xb') as f:
        f.write(out)
    if args.pixel_faces:
        with args.pixel_faces.open('x') as f:
            f.write(''.join(f'{face} {r} {du} {dv}\n' for face, (r, du, dv) in sorted(changed.items())))
    if args.order_map:
        with args.order_map.open('x') as f:
            json.dump(dict(new_to_old=order, rotated=rotated), f)
    text = json.dumps(result, indent=1)
    if args.report:
        with args.report.open('x') as f:
            f.write(text)
    print(text)
