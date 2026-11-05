"""Validate per-face strip topology and inventory existing DNM packs, read-only.

python3 platform/dreamcast-native/tools/inspect_quad_strips.py --self-test
python3 platform/dreamcast-native/tools/inspect_quad_strips.py READER PACK [PACK ...]

READER is build/check_native_meshpack: reuse the real C input validator.
Cross-face chains are analysis-only: no asset rewriting or target rendering change.
Savings assume fully visible, unclipped geometry; not a frame-time estimate.
"""
import argparse
from collections import Counter, defaultdict
import itertools
import json
from pathlib import Path
import subprocess
import struct
import sys

import prepare_models as models


def strip_indices(face):
    if len(face) == 3:
        return tuple(face)
    if len(face) == 4:
        # Preserve fan diagonal0--2, winding and triangle order. Simply
        # submitting0,1,2,3 would silently replace it with diagonal1--2.
        return tuple(face[i] for i in (1, 2, 0, 3))
    raise ValueError("only triangles and quads have a validated strip mapping")


def oriented(triangle):
    """Cyclic rotations are equivalent; reversing winding is NOT equivalent."""
    a, b, c = triangle
    return min((a, b, c), (b, c, a), (c, a, b))


def fan_triangles(face):
    return [oriented((face[0], face[i], face[i+1])) for i in range(1, len(face)-1)]


def strip_triangles(strip, parity=0):
    return [oriented((strip[i+1], strip[i], strip[i+2]) if (i+parity) & 1
                     else (strip[i], strip[i+1], strip[i+2]))
            for i in range(len(strip)-2)]


def build_chains(faces, stable_materials, reorder=False):
    """Whole-face chains; never change diagonals, vertex attributes or material.

    ponytail: deterministic greedy walks, not globally optimal stripification;
    replace only if measured missed savings justify a better offline heuristic.
    Reorder=False preserves both face and per-face triangle order exactly.
    """
    edge_count = Counter()
    for vertices, _, _ in faces:
        for a, b in zip(vertices, vertices[1:]+vertices[:1]):
            edge_count[tuple(sorted((a[:12], b[:12])))] += 1
    options = {}
    starts = defaultdict(list)
    for index, (vertices, material, _) in enumerate(faces):
        if material not in stable_materials or len({v[:12] for v in vertices}) != len(vertices):
            continue
        target = fan_triangles(vertices)
        for parity in (0, 1):
            valid = []
            for strip in itertools.permutations(vertices):
                triangles = strip_triangles(strip, parity)
                matches = Counter(triangles) == Counter(target) if reorder else triangles == target
                if matches:
                    valid.append(strip)
                    starts[parity, material, strip[:2]].append((index, strip))
            options[index, parity] = valid
    unused = set(range(len(faces)))
    chains = []
    while unused:
        first = min(unused)
        material = faces[first][1]
        best = ([first], strip_indices(faces[first][0]))
        for start in options.get((first, 0), []):
            ids, strip = [first], start
            seen = {first}
            while edge_count[tuple(sorted((strip[-2][:12], strip[-1][:12])))] == 2:
                choices = starts[(len(strip)-2)&1, material, strip[-2:]]
                next_face = next(((i, s) for i, s in choices if i in unused and i not in seen
                                  and (reorder or i == ids[-1]+1)), None)
                if next_face is None:
                    break
                index, continuation = next_face
                ids.append(index); seen.add(index); strip += continuation[2:]
            if len(ids) > len(best[0]):
                best = ids, strip
        ids, strip = best
        expected = [t for i in ids for t in fan_triangles(faces[i][0])]
        actual = strip_triangles(strip)
        assert Counter(actual) == Counter(expected)
        if not reorder:
            assert actual == expected
        assert all(faces[i][1] == material for i in ids)
        unused.difference_update(ids)
        chains.append((ids, strip))
    assert sorted(i for ids, _ in chains for i in ids) == list(range(len(faces)))
    return chains


def edge_audit(faces, stable_materials=frozenset()):
    """Exact XYZ adjacency within one mesh; no welding, reordering or savings claim.

    Candidate means matching source attributes, NOT proven runtime binding/strip
    compatibility: face-position-dependent lighting and clipping still matter.
    """
    edges = defaultdict(list)
    counts = Counter()
    for vertices, material, lighting in faces:
        for a, b in zip(vertices, vertices[1:] + vertices[:1]):
            if a[:12] == b[:12]:
                counts['degenerate'] += 1
                continue
            edges[tuple(sorted((a[:12], b[:12])))].append((a, b, material, lighting))
    for adjacent in edges.values():
        if len(adjacent) != 2:
            counts['boundary' if len(adjacent) == 1 else 'nonmanifold'] += 1
            continue
        a, b = adjacent
        reasons = []
        if a[0][:12] != b[1][:12]:
            reasons.append('winding')
        if a[2] != b[2]:
            reasons.append('material')
        auv = {v[:12]: v[12:] for v in a[:2]}
        buv = {v[:12]: v[12:] for v in b[:2]}
        if auv != buv:
            reasons.append('uv')
        if a[3] != b[3]:
            reasons.append('source_flags_or_normal')
        counts['shared'] += 1
        counts['candidate' if not reasons else '+'.join(reasons)] += 1
        if not any(reason in reasons for reason in ('winding', 'material', 'uv')) and a[2] in stable_materials:
            counts['light_invariant_binding_candidate'] += 1
    return dict(counts)


def material_states(reader, path):
    """Use the target's real host validator before reading DNT2/4 metadata."""
    subprocess.run([str(reader), str(path), '--cpu-backed'], check=True, capture_output=True, text=True)
    data = path.read_bytes()
    magic, size, resources, count, *_ = struct.unpack_from('<4s7I', data)
    if magic not in (b'DNT2', b'DNT4') or size != len(data):
        raise ValueError('binding audit requires validated DNT2 or DNT4')
    base = 32 + resources*16
    result = defaultdict(set)
    for i in range(count):
        binding = struct.unpack_from('<6HI', data, base+i*16)
        modulation = struct.unpack_from('<I', data, base+count*16+i*4)[0] if magic == b'DNT4' else 0xffffffff
        # Same source header fixes list/filter/UV flip and original dimensions.
        # Resource fixes texture/dimensions; color is crop origin or solid ARGB.
        result[binding[:4]].add((binding[5], binding[6], modulation))
    return result


def self_test():
    cases = list(itertools.permutations(range(4)))
    cases += list(itertools.product(range(3), repeat=4))  # repeated/degenerate vertices
    cases += list(itertools.permutations(range(3)))
    for face in cases:
        assert strip_triangles(strip_indices(face)) == fan_triangles(face)
    assert strip_triangles((0, 1, 2, 3)) != fan_triangles((0, 1, 2, 3))
    assert oriented((0, 1, 2)) != oriented((0, 2, 1))
    for invalid in ((), (0,), (0, 1), (0, 1, 2, 3, 4)):
        try:
            strip_indices(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError("unsupported topology accepted")
    vertices = [bytes([i])*12 + bytes([i])*4 for i in range(4)]
    a, b, c, d = vertices
    pair = [([a,b,c], 0, b'light'), ([b,a,d], 0, b'light')]
    assert edge_audit(pair) == {'boundary': 4, 'shared': 1, 'candidate': 1}
    assert edge_audit(pair, {0})['light_invariant_binding_candidate'] == 1
    for second, reason in (([a,b,d], 'winding'),
                           ([b,a[:12]+b'uv!!',d], 'uv')):
        assert edge_audit([pair[0], (second,0,b'light')])[reason] == 1
    assert edge_audit([pair[0], ([b,a,d],1,b'other')])['material+source_flags_or_normal'] == 1
    assert 'light_invariant_binding_candidate' not in edge_audit([pair[0], ([b,a,d],1,b'other')], {0,1})
    assert edge_audit(pair+[([b,a,d],0,b'light')])['nonmanifold'] == 1
    assert edge_audit([([a,a,b],0,b'light')])['degenerate'] == 1
    v = [bytes([i])*16 for i in range(8)]
    connected = [([v[2],v[0],v[1],v[3]],0,b''),
                 ([v[4],v[2],v[3],v[5]],0,b''),
                 ([v[4],v[5],v[6]],0,b''), ([v[6],v[5],v[7]],0,b'')]
    for reorder in (False, True):
        chains = build_chains(connected, {0}, reorder)
        assert len(chains) == 1 and len(chains[0][1]) == 8
        assert len(build_chains(connected, set(), reorder)) == 4
        seam = [(list(p),m,n) for p,m,n in connected]
        seam[1][0][1] = v[2][:12]+b'seam'
        assert len(build_chains(seam, {0}, reorder)) > 1
    for face in cases:
        synthetic = [([v[i] for i in face],0,b'')]
        assert len(build_chains(synthetic, {0}, True)) == 1
    return len(cases)


def inspect(reader, path, states=None):
    subprocess.run([str(reader), str(path)], check=True, capture_output=True, text=True)
    data = path.read_bytes()
    header = models.HEADER.unpack_from(data)
    _, vertex_base, face_base, material_base, _ = models.table_offsets(header)
    stable_materials = set()
    if states is not None:
        for i in range(header[5]):
            material = models.MATERIAL.unpack_from(data, material_base+i*models.MATERIAL.size)
            variants = states.get(material[:4])
            if not variants:
                raise ValueError(f'material {i} has no bindings in supplied pack')
            if len(variants) == 1:
                stable_materials.add(i)
    triangles = quads = 0
    edges = Counter()
    mesh_edges = []
    chain_totals = Counter()
    for mesh_index in range(header[2]):
        mesh = models.MESH.unpack_from(data, models.HEADER.size + mesh_index * models.MESH.size)
        faces = []
        for index in range(mesh[2], mesh[2] + mesh[3]):
            face = models.FACE.unpack_from(data, face_base + index * models.FACE.size)
            indices = face[:face[5]]
            # Compare complete vertex identities (XYZ+UV), not just positions.
            vertices = [data[vertex_base+(mesh[0]+i)*models.VERTEX.size:
                             vertex_base+(mesh[0]+i+1)*models.VERTEX.size] for i in indices]
            assert strip_triangles(strip_indices(vertices)) == fan_triangles(vertices)
            # Raw attributes/normals: exact, conservative source compatibility.
            raw = data[face_base+index*models.FACE.size:face_base+(index+1)*models.FACE.size]
            faces.append((vertices, face[4], raw[12:28]))
            if face[5] == 4:
                quads += 1
            else:
                triangles += 1
        audit = edge_audit(faces, stable_materials)
        edges.update(audit)
        if states is not None:
            trials = [('source_order', stable_materials, False),
                      ('reordered', stable_materials, True),
                      ('optimistic_source_order', set(range(header[5])), False),
                      ('optimistic_reordered', set(range(header[5])), True)]
            for label, eligible, reorder in trials:
                chains = build_chains(faces, eligible, reorder)
                chain_totals[label+'_strips'] += len(chains)
                chain_totals[label+'_vertices'] += sum(len(strip) for _, strip in chains)
                chain_totals[label+'_joined_faces'] += sum(len(ids) for ids, _ in chains if len(ids)>1)
        mesh_edges.append(dict(mesh=mesh_index, faces=mesh[3], edges=audit))
    assert triangles + quads == header[4]
    return dict(pack=str(path), unique_mesh_triangles=triangles, unique_mesh_quads=quads,
                triangle_list_vertices=triangles*3+quads*6,
                unclipped_strip_vertices=triangles*3+quads*4,
                potential_vertex_bytes_saved=quads*2*32,
                cross_face_edges=dict(edges),
                light_invariant_materials=len(stable_materials) if states is not None else None,
                offline_chain_totals=dict(chain_totals),
                chain_scope="optimistic trials IGNORE lighting/binding differences; reordered trials change draw order; no clipping/culling or runtime integration; greedy, not an optimum",
                candidate_meshes=sorted(mesh_edges, key=lambda row: row['edges'].get('candidate', 0), reverse=True)[:12],
                edge_scope="source candidates only; runtime binding/light, strip parity and clipping not proven",
                scope="unique mesh inventory, not visibility/instance-weighted or frame timing")


if __name__ == "__main__":
    cases = self_test()
    if sys.argv[1:] == ["--self-test"]:
        print(f"PASS: {cases} oriented triangle/quad mappings; wrong diagonal/winding rejected")
    else:
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument("reader", type=Path)
        parser.add_argument("packs", nargs="+", type=Path)
        parser.add_argument('--materials', type=Path)
        parser.add_argument('--material-reader', type=Path)
        args = parser.parse_args()
        if bool(args.materials) != bool(args.material_reader) or (args.materials and len(args.packs) != 1):
            parser.error('--materials and --material-reader require exactly one mesh pack')
        states = material_states(args.material_reader.resolve(), args.materials.resolve()) if args.materials else None
        for pack in args.packs:
            print(json.dumps(inspect(args.reader.resolve(), pack.resolve(), states), sort_keys=True))
