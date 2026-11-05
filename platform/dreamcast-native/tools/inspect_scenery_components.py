"""Inventory exact-edge components touching selected scenery materials.

Read-only geometry evidence, NOT automatic semantic classification or removal.
Usage: python3 inspect_scenery_components.py READER DNM MATERIAL [MATERIAL ...]
"""
from collections import Counter, defaultdict
import json
from pathlib import Path
import subprocess
import sys

import prepare_models as models


def connected(faces, seeds):
    edges = defaultdict(set)
    face_edges = {}
    for index, (material, vertices) in faces.items():
        face_edges[index] = [tuple(sorted((a,b))) for a,b in zip(vertices, vertices[1:]+vertices[:1]) if a != b]
        for edge in face_edges[index]:
            edges[edge].add(index)
    visited = set()
    result = []
    for seed in sorted(i for i,(material,_) in faces.items() if material in seeds):
        if seed in visited:
            continue
        component, pending, used_edges = set(), [seed], set()
        while pending:
            index = pending.pop()
            if index in component:
                continue
            component.add(index)
            for edge in face_edges[index]:
                if edge not in used_edges:
                    used_edges.add(edge)
                    pending.extend(edges[edge]-component)
        visited.update(component)
        result.append(sorted(component))
    return result


def self_test():
    faces = {0:(1,[0,1,2]),1:(2,[2,1,3]),2:(3,[3,4,5]),3:(1,[8,9,10])}
    assert connected(faces,{1}) == [[0,1],[3]]  # A shared point is not an edge.
    assert connected(faces,{2}) == [[0,1]]
    assert connected(faces,{99}) == []
    assert connected({0:(1,[0,0,1]),1:(2,[2,3,4])},{1}) == [[0]]


def inspect(reader, path, seeds):
    subprocess.run([str(reader),str(path)],check=True,capture_output=True)
    data = path.read_bytes()
    h = models.HEADER.unpack_from(data)
    _, vertex_base, face_base, _, _ = models.table_offsets(h)
    faces, owners = {}, {}
    for mesh_id in range(h[2]):
        mesh = models.MESH.unpack_from(data,models.HEADER.size+mesh_id*models.MESH.size)
        for f in range(mesh[2],mesh[2]+mesh[3]):
            face = models.FACE.unpack_from(data,face_base+f*models.FACE.size)
            vertices = [models.VERTEX.unpack_from(data,vertex_base+(mesh[0]+i)*models.VERTEX.size)[:3] for i in face[:face[5]]]
            faces[f] = face[4],vertices
            owners[f] = mesh_id
    result = []
    for ids in connected(faces,seeds):
        vertices = [p for i in ids for p in faces[i][1]]
        result.append(dict(faces=ids, count=len(ids), meshes=sorted({owners[i] for i in ids}),
            materials=dict(Counter(faces[i][0] for i in ids)),
            bounds=[[min(p[a] for p in vertices),max(p[a] for p in vertices)] for a in range(3)]))
    return dict(scope='exact XYZ edges across source meshes; no semantic or instance-transform inference', components=result)


if __name__ == '__main__':
    self_test()
    if sys.argv[1:] == ['--self-test']:
        print('PASS: edge-connected, isolated, point-only and degenerate components')
    elif len(sys.argv) >= 4:
        print(json.dumps(inspect(Path(sys.argv[1]).resolve(),Path(sys.argv[2]),set(map(int,sys.argv[3:]))),indent=2))
    else:
        raise SystemExit(__doc__)
