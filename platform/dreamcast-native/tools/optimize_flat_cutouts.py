"""Collapse exact adjacent flat cutout quads before DNM position/group packing."""
from fractions import Fraction
import math


def _orient(a, b, c):
    return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])


def _merged_corners(indices, vertices, schema):
    raw = [vertices[i] for i in indices]
    values = [schema.unpack(v) for v in raw]
    points = [tuple(Fraction(x) for x in v[:3]) for v in values]
    # Numeric equality must never weld different serialized signed zeros.
    positions = {}
    for p, v in zip(points, raw):
        if p in positions and positions[p] != v[:12]:
            return None
        positions[p] = v[:12]
    a, b = [tuple(points[i][k]-points[0][k] for k in range(3)) for i in (1, 2)]
    normal = (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
    if not any(normal) or any(sum(n*(p[k]-points[0][k]) for k, n in enumerate(normal)) for p in points):
        return None
    drop = max(range(3), key=lambda k: abs(normal[k]))
    xy = [tuple(p[k] for k in range(3) if k != drop) for p in points]
    area = _orient(*xy[:3])
    # Both source fans must describe convex quads with the same winding.
    if any(_orient(q[i-1], q[i], q[(i+1)%4])*area <= 0
           for q in (xy[:4], xy[4:]) for i in range(4)):
        return None
    weights = [(_orient(p, xy[1], xy[2]), _orient(xy[0], p, xy[2]),
                _orient(xy[0], xy[1], p)) for p in xy]
    if any(v[k+3]*area != sum(values[i][k+3]*w[i] for i in range(3))
           for v, w in zip(values, weights) for k in (0, 1)):
        return None
    edges = {}
    for start in (0, 4):
        for i in range(4):
            u, v = points[start+i], points[start+(i+1)%4]
            edges.setdefault(tuple(sorted((u, v))), []).append((u, v))
    perimeter = []
    shared = 0
    for rows in edges.values():
        if len(rows) == 1:
            perimeter.append(rows[0])
        elif len(rows) == 2 and rows[0] == rows[1][::-1]:
            shared += 1
        else:
            return None
    # Crossed panels may share this edge globally; only this planar pair matters.
    if shared != 1:
        return None
    outgoing = dict(perimeter)
    if len(outgoing) != len(perimeter) or len(set(outgoing.values())) != len(perimeter):
        return None
    ring, current = [], perimeter[0][0]
    while current not in ring:
        ring.append(current)
        if current not in outgoing:
            return None
        current = outgoing[current]
    if current != ring[0] or len(ring) != len(perimeter):
        return None
    projected = [tuple(p[k] for k in range(3) if k != drop) for p in ring]
    corners = [p for i, p in enumerate(ring)
               if _orient(projected[i-1], projected[i], projected[(i+1)%len(ring)])]
    if len(corners) != 4:
        return None
    cxy = [tuple(p[k] for k in range(3) if k != drop) for p in corners]
    if any(_orient(cxy[i-1], cxy[i], cxy[(i+1)%4])*area <= 0 for i in range(4)):
        return None
    old_area = sum(_orient(xy[s], xy[s+1], xy[s+2]) +
                   _orient(xy[s], xy[s+2], xy[s+3]) for s in (0, 4))
    if _orient(*cxy[:3])+_orient(cxy[0], cxy[2], cxy[3]) != old_area:
        return None
    return [indices[points.index(p)] for p in corners]


def collapse_flat_cutouts(data, mesh_ids=None):
    """Return (DNM1 bytes, merged pair count) for validated prepare() output.

    mesh_ids optionally limits the pass to selected mesh indices.
    Unsupported pairs stay unchanged. Only consecutive disjoint pairs are
    considered; grouping and position references must be rebuilt afterwards.
    """
    # Runtime import lets prepare_models call this without a schema import cycle.
    from prepare_models import HEADER, MESH, VERTEX, FACE, MATERIAL, INSTANCE, MAX_BYTES
    if len(data) < HEADER.size:
        raise ValueError("flat cutouts require nonempty prepared DNM1")
    h = list(HEADER.unpack_from(data))
    vb = HEADER.size+h[2]*MESH.size
    fb = vb+h[3]*VERTEX.size
    mb = fb+h[4]*FACE.size
    ib = mb+h[5]*MATERIAL.size
    if (h[0] != 0x314d4e44 or h[1] != len(data) or len(data) > MAX_BYTES
            or not all(h[2:7]) or h[7] or any(h[12:])
            or ib+h[6]*INSTANCE.size != len(data)):
        raise ValueError("flat cutouts require prepared DNM1 tables")
    materials = [MATERIAL.unpack_from(data, mb+i*MATERIAL.size) for i in range(h[5])]
    meshes, vertices, faces = [], [], []
    merged = vertex_cursor = face_cursor = 0
    for mid in range(h[2]):
        mesh = list(MESH.unpack_from(data, HEADER.size+mid*MESH.size))
        if (mesh[0] != vertex_cursor or mesh[2] != face_cursor or not 0 < mesh[1] <= 65536
                or not mesh[3] or mesh[0]+mesh[1] > h[3] or mesh[2]+mesh[3] > h[4]
                or mesh[10] > 3 or any(mesh[11:])):
            raise ValueError("invalid prepared mesh ranges")
        vs = [data[vb+(mesh[0]+i)*VERTEX.size:vb+(mesh[0]+i+1)*VERTEX.size] for i in range(mesh[1])]
        fs = [data[fb+(mesh[2]+i)*FACE.size:fb+(mesh[2]+i+1)*FACE.size] for i in range(mesh[3])]
        if any(not all(map(math.isfinite, VERTEX.unpack(v)[:3])) for v in vs):
            raise ValueError("non-finite prepared vertex")
        decoded = [list(FACE.unpack(f)) for f in fs]
        for f in decoded:
            if (f[5] not in (3, 4) or f[5] != (4 if f[7]&1 else 3)
                    or not f[7]&3 or not (f[7]>>8)&3 or f[6] or f[11]
                    or f[4] >= h[5] or any(i >= mesh[1] for i in f[:f[5]])
                    or not all(map(math.isfinite, f[8:11]))):
                raise ValueError("invalid prepared face")
        # shortcut: one disjoint pair pass; rerun explicitly to merge larger patches.
        kept, i = [], 0
        while i < len(fs):
            f = decoded[i]
            material = materials[f[4]]
            if ((mesh_ids is None or mid in mesh_ids)
                    and i+1 < len(fs) and mesh[10] <= 1 and f[5] == 4
                    and f[7]&(1<<17) and material[5] == 255
                    and (material[0]>>13)&3 == 3 and not material[0]&0x8000
                    and fs[i][8:] == fs[i+1][8:]):
                corners = _merged_corners(f[:4]+decoded[i+1][:4], vs, VERTEX)
                if corners is not None:
                    f[:4] = corners
                    kept.append(f)
                    merged += 1
                    i += 2
                    continue
            kept.append(f)
            i += 1
        vertex_cursor += mesh[1]
        face_cursor += mesh[3]
        used = {v for f in kept for v in f[:f[5]]}
        remap = {v: j for j, v in enumerate(sorted(used))}
        mesh[0], mesh[1], mesh[2], mesh[3] = len(vertices), len(used), len(faces), len(kept)
        meshes.append(MESH.pack(*mesh))
        vertices.extend(v for j, v in enumerate(vs) if j in used)
        for f in kept:
            f[:f[5]] = [remap[v] for v in f[:f[5]]]
            faces.append(FACE.pack(*f))
    if (vertex_cursor, face_cursor) != (h[3], h[4]):
        raise ValueError("incomplete prepared mesh ranges")
    if not merged:
        return data, 0
    payload = b"".join(meshes+vertices+faces)+data[mb:]
    h[1], h[3], h[4] = HEADER.size+len(payload), len(vertices), len(faces)
    return HEADER.pack(*h)+payload, merged
