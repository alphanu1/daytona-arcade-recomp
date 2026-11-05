"""The race player's damaged bodies as position/normal variants of the clean body, in a NEW car-damage.dnb.

python3 platform/dreamcast-native/tools/prepare_damage_bodies.py RACE_CARS_EXPORT ROM_IMAGES CAR.dnm NEW.dnb [--hoods]

--hoods: the hoods of the hood view instead (a NEW car-hoods.dnb): the automatic car's hood list 231eb0[0][i]
(7328..7350, include/player_parts.h nm_player_hood): the clean hood 94fff9 and the three damaged ones, which have
its topology as the damaged bodies have the body's. Four records in hood order, index i = 231eb0[0][i], the clean
hood first (index 0). The manual car's hoods 231eb0[1] have the same positions with other texture headers: its
clean hood takes these variants in its own face order, as the manual body takes car-damage.dnb's.

The body the player's draw takes (7358: +14) is crash_model[v][d] (word 235b5c+4v+8d) once a corner is damaged or the
car crashed (spec-damage.md section 1; include/player_parts.h nm_player_damage_body). The 16 bodies of the automatic
car have the clean body's topology face for face and vertex for vertex: the same 584 vertices with the same texture
coordinates, the same 325 faces with the same corners and texture headers (only the arcade's texture distance
coefficient, attribute bits 27-31, differs, which the port does not use). They differ in positions and supplied
normals only. Sixteen more meshes in car.dnm would cost 1.15 MB of heap (the renderer's per-face and per-vertex
tables); a variant here is 11 KB: the renderer writes it into the clean body's mesh when the drawn body changes
(code/render/scene/practice.c, nm_render_car_mesh_refresh).

Each variant is given in the vertex and face order of the clean body's mesh of CAR.dnm, after face ordering and
restripping: a final vertex is the source vertex with its position and u, v (unique), a final face the source face
with its corners and supplied normal. Checked: every final vertex and face is found, a variant's value does not
depend on which of several equal source faces is taken, vertices that share a position representative in CAR.dnm
(the renderer transforms representatives only) share it in every variant, and the clean variant reproduces the
mesh's positions, normals and bounds bit for bit.

Format (little endian; ROM-derived, keep under build/): header 32 bytes: "DNB1", file size, body model address
(0x95ff9d), vertex count V, face count F, variant count (17), bytes per variant record, the most light classes
of one variant (distinct material and supplied normal pairs: the renderer's light class cache must hold them). Then 17 records, each
32-byte aligned: model address, index (crash_model[0][index] for 0..15; 16 the clean body), mesh bounds (6 float32,
the min and max corner as nm_mesh_t holds them), V positions (3 float32), F normals (3 float32), zero padding.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import prepare_models as pm
from inspect_course import MAIN_BASE, words

CLEAN = 0x95ff9d
HEADER = struct.Struct('<4s7I')
RECORD = struct.Struct('<2I6f')


HOOD_LISTS = 0x231eb0


def hood_models(program, main):
    """231eb0[0][0..3], the automatic car's hoods: the list's single model; the first is the clean hood."""
    out = []
    for i in range(4):
        pointer = words(program, 0, HOOD_LISTS-0x200000+4*i, 1)[0]
        record = words(main, MAIN_BASE, pointer, 5)
        if record[4] or not 0x800000 <= record[0] < 0xc00000:
            raise ValueError('hood list is not one ROM model')
        out.append(record[0])
    if len(set(out)) != 4:
        raise ValueError('hood table changed')
    return out


def crash_models(program, main):
    """crash_model[0][d], the automatic car (235b5c+8d): the list's single model."""
    out = []
    for d in range(16):
        pointer = words(program, 0, 0x35b5c+8*d, 1)[0]
        record = words(main, MAIN_BASE, pointer, 5)
        if record[4] or not 0x800000 <= record[0] < 0xc00000:
            raise ValueError('crash_model list is not one ROM model')
        out.append(record[0])
    if len(set(out)) != 16 or CLEAN in out:
        raise ValueError('crash_model table changed')
    return out


def tables(data):
    h = pm.HEADER.unpack_from(data)
    off = pm.table_offsets(h)
    end = off[4]+h[6]*pm.INSTANCE.size
    refs = (end+31) & ~31 if h[0] & pm.ALIGNED else end
    return h, off, refs


def mesh_of(data, address):
    h, off, refs = tables(data)
    owners = {pm.INSTANCE.unpack_from(data, off[4]+i*pm.INSTANCE.size)[:2] for i in range(h[6])}
    meshes = {m for m, a in owners if a == address}
    if len(meshes) != 1 or sum(1 for m, a in owners if m in meshes) != 1:
        raise ValueError(f'model {address:#x} is not exactly one mesh of its own')
    index = meshes.pop()
    mesh = pm.MESH.unpack_from(data, off[0]+index*pm.MESH.size)
    vertices = [pm.VERTEX.unpack_from(data, off[1]+(mesh[0]+k)*pm.VERTEX.size) for k in range(mesh[1])]
    faces = [pm.FACE.unpack_from(data, off[2]+(mesh[2]+k)*pm.FACE.size) for k in range(mesh[3])]
    shared = struct.unpack_from(f'<{mesh[1]}H', data, refs+2*mesh[0]) if h[0] & 0x7fffffff != 0x314d4e44 else None
    return mesh, vertices, faces, shared


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def prepare(export, images, car, hoods=False):
    program, main = pm.car_images(images)
    if hoods:
        clean_model, *damaged = hood_models(program, main)
        order = [0, 1, 2, 3]                 # 231eb0[0][0..3]: the clean hood first
    else:
        clean_model, damaged = CLEAN, crash_models(program, main)
        order = list(range(1, 17))+[0]       # crash_model[0][0..15], then the clean body
    count = len(order)
    source = pm.local_models(pm.prepare(export)[0], [clean_model]+damaged)
    variants = [mesh_of(source, a) for a in [clean_model]+damaged]
    clean_mesh, clean_vertices, clean_faces, _ = variants[0]
    for mesh, vertices, faces, _ in variants[1:]:
        if (len(vertices) != len(clean_vertices) or [v[3:] for v in vertices] != [v[3:] for v in clean_vertices] or
                [f[:6] for f in faces] != [f[:6] for f in clean_faces] or mesh[4+6:] != clean_mesh[4+6:]):
            raise ValueError('a damaged body does not share the clean body\'s topology and texture coordinates')
    mesh, final_vertices, final_faces, shared = mesh_of(car, clean_model)
    if mesh[10] > 1:
        raise ValueError('the clean body must use supplied normals (mode 0/1)')
    # Final vertex -> source vertex (position and u, v are unique in the source body).
    by_key = {}
    for j, v in enumerate(clean_vertices):
        if by_key.setdefault(v, j) != j:
            raise ValueError('two source vertices with the same position and u, v')
    vertex_map = [by_key.get(v) for v in final_vertices]
    if None in vertex_map:
        raise ValueError('a final vertex of the clean body has no source vertex')
    # Final face -> source faces: every corner a corner of the source face, the same supplied normal.
    corners = [set(f[:f[5]]) for f in clean_faces]
    face_map = []
    for f in final_faces:
        found = [k for k, c in enumerate(corners) if all(vertex_map[i] in c for i in f[:f[5]])
                 and clean_faces[k][8:11] == f[8:11]]
        if not found:
            raise ValueError('a final face of the clean body has no source face')
        face_map.append(found)
    out_records, report = [], dict(variants=[])
    for index, which in enumerate(order):
        _, vertices, faces, _ = variants[which]
        positions = [vertices[j][:3] for j in vertex_map]
        normals = []
        for found in face_map:
            values = {faces[k][8:11] for k in found}
            if len(values) != 1:
                raise ValueError('equal source faces of the clean body have different damaged normals')
            normals.append(values.pop())
        if shared is not None and any(positions[i] != positions[r] for i, r in enumerate(shared)):
            raise ValueError('a variant splits vertices that share a position representative')
        low = [min(p[a] for p in positions) for a in range(3)]
        high = [max(p[a] for p in positions) for a in range(3)]
        bounds = low+high
        if which == 0:
            if positions != [v[:3] for v in final_vertices] or normals != [f[8:11] for f in final_faces]:
                raise ValueError('the clean variant does not reproduce the clean body')
            if [f32(b) for b in bounds] != list(mesh[4:10]):
                raise ValueError('the clean body\'s bounds are not the extent of its vertices')
        address = clean_model if which == 0 else damaged[which-1]
        payload = b''.join(struct.pack('<3f', *p) for p in positions)+b''.join(struct.pack('<3f', *n) for n in normals)
        record = RECORD.pack(address, index, *bounds)+payload
        out_records.append(record+bytes(-len(record) % 32))
        moved = sum(p != v[:3] for p, v in zip(positions, final_vertices))
        turned = sum(n != f[8:11] for n, f in zip(normals, final_faces))
        report['variants'].append(dict(index=index, model=hex(address), moved_vertices=moved, changed_normals=turned,
                                       distinct_normal_classes=len({(f[4], n) for f, n in zip(final_faces, normals)})))
    size = len(out_records[0])
    most = max(v['distinct_normal_classes'] for v in report['variants'])
    data = HEADER.pack(b'DNB1', HEADER.size+count*size, clean_model, len(final_vertices), len(final_faces), count, size, most)
    data += b''.join(out_records)
    report.update(bytes=len(data), vertices=len(final_vertices), faces=len(final_faces),
                  most_normal_classes=max(v['distinct_normal_classes'] for v in report['variants']),
                  sha256=hashlib.sha256(data).hexdigest())
    return data, report


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('export', type=Path, help='m2segment --export-native-race-cars directory')
    parser.add_argument('images', type=Path, help='daytona93 ROM image directory')
    parser.add_argument('car', type=Path, help='the final car.dnm (face ordered, restripped)')
    parser.add_argument('output', type=Path, help='NEW car-damage.dnb (car-hoods.dnb with --hoods)')
    parser.add_argument('--hoods', action='store_true', help='the hood view\'s hoods 231eb0[0] instead of the bodies')
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit('output exists; choose a new file')
    data, report = prepare(args.export, args.images, args.car.read_bytes(), args.hoods)
    with args.output.open('xb') as f:
        f.write(data)
    json.dump(report, sys.stdout, indent=1)
    print()


if __name__ == '__main__':
    main()
