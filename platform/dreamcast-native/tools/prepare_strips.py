"""Append ordered cross-face strip candidates to prepared DNM4 scenery.

Keep source faces for runtime clipping/culling/binding fallback. No vertex,
material, normal or source draw-order rewriting; coplanarity is not required.
"""
import struct

from inspect_quad_strips import build_chains
from prepare_models import HEADER, MESH, VERTEX, FACE, MATERIAL, MAX_BYTES

STRIP = struct.Struct('<IHHI5I')


def prepare_strips(data, mesh_ids=None):
    """Return DNM4/5 bytes and candidate counts from validated grouped input."""
    h = list(HEADER.unpack_from(data))
    if h[0] != 0x344d4e44 or h[1] != len(data) or any(h[12:]):
        raise ValueError('strip candidates require prepared DNM4 face groups')
    vb = HEADER.size+h[2]*MESH.size
    fb = vb+h[3]*VERTEX.size
    mb = fb+h[4]*FACE.size
    eligible = {i for i in range(h[5]) if not MATERIAL.unpack_from(data, mb+i*MATERIAL.size)[0]&0x8000}
    out = bytearray(data)
    records, indices = [], []
    joined_faces = saved = 0
    for mid in range(h[2]):
        mesh = list(MESH.unpack_from(data, HEADER.size+mid*MESH.size))
        mesh[13] = len(records)
        if mesh_ids is None or mid in mesh_ids:
            vertices = [data[vb+(mesh[0]+v)*VERTEX.size:vb+(mesh[0]+v+1)*VERTEX.size]
                        for v in range(mesh[1])]
            vertex_ids = {raw: i for i, raw in enumerate(vertices)}
            faces = []
            for fid in range(mesh[2], mesh[2]+mesh[3]):
                face = FACE.unpack_from(data, fb+fid*FACE.size)
                faces.append(([vertices[i] for i in face[:face[5]]], face[4], b''))
            for ids, strip in build_chains(faces, eligible):
                if len(ids) < 2 or len(strip) > 65535:
                    continue
                assert ids == list(range(ids[0], ids[0]+len(ids)))
                records.append(STRIP.pack(mesh[2]+ids[0], len(ids), len(strip), len(indices), 0, 0, 0, 0, 0))
                indices.extend(vertex_ids[v] for v in strip)
                joined_faces += len(ids)
                saved += 2*(len(ids)-1)
        mesh[14] = len(records)-mesh[13]
        MESH.pack_into(out, HEADER.size+mid*MESH.size, *mesh)
    report = dict(strip_candidates=len(records), strip_candidate_faces=joined_faces,
                  strip_candidate_vertices_saved=saved)
    if not records:
        return data, report
    out.extend(bytes((-len(out))&31))
    out.extend(b''.join(records))
    out.extend(struct.pack(f'<{len(indices)}H', *indices))
    out.extend(bytes((-len(out))&3))
    if len(out) > MAX_BYTES:
        raise ValueError('strip metadata exceeds resident geometry ceiling; split offline')
    h[0], h[1], h[12], h[13] = 0x354d4e44, len(out), len(records), len(indices)
    HEADER.pack_into(out, 0, *h)
    return bytes(out), report
