"""ROM-free DNM5 topology/preservation checks and malformed C-reader inputs."""
import struct
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import prepare_models as models
from prepare_strips import prepare_strips, STRIP


def fixture():
    meshes, vertices, faces, instances = [], [], [], []
    for mid in range(2):
        points = [(i//2, i%2, 1+(i%3)/32, i*64, i*32) for i in range(8)]
        raw = [models.VERTEX.pack(*p) for p in points]
        raw += [models.VERTEX.pack(*points[0][:3], 1, 0),
                models.VERTEX.pack(-0.0, *points[0][1:]), raw[0]]
        bounds = [fn(p[a] for p in points) for fn in (min, max) for a in range(3)]
        meshes.append(models.MESH.pack(len(vertices), len(raw), len(faces), 4,
                                       *bounds, 1, 0, 0, 0, 0, 0))
        vertices.extend(raw)
        for fid, polygon in enumerate(((2, 0, 1, 3), (4, 2, 3, 5), (4, 5, 6), (6, 5, 7))):
            count = len(polygon)
            faces.append(models.FACE.pack(*(list(polygon)+[0]*(4-count)), 0, count, 0,
                0x201 if count == 4 else 0x202, 0, fid%2, 1-fid%2, 0))
        instances.append(models.INSTANCE.pack(mid, 0x800000+mid, 0, 0,
            1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 10, 256, 256, 0, 0, 1,
            0, 0, 496, 384, 248, 192, 0, 0, 0, 0))
    material = models.MATERIAL.pack(0x4013, 0, 3600, 41216, 128, 64, 0, 0, 0, 0)
    payload = b''.join(meshes+vertices+faces+[material, material]+instances)
    header = models.HEADER.pack(0x314d4e44, 64+len(payload), 2, len(vertices), 8, 2, 2,
                               0, 0, 0, 0, 0, 0, 0, 0, 0)
    return models.group_faces(header+payload, 2)


def change(data, offset, fmt, *values):
    out = bytearray(data)
    struct.pack_into('<'+fmt, out, offset, *values)
    return bytes(out)


grouped = fixture()
out, report = prepare_strips(grouped)
h = models.HEADER.unpack_from(out)
assert h[0] == 0x354d4e44 and h[12:14] == (2, 16)
assert report == dict(strip_candidates=2, strip_candidate_faces=8, strip_candidate_vertices_saved=12)
vb = 64+h[2]*64
assert out[vb:len(grouped)] == grouped[vb:]
rb = (len(grouped)+31)&~31
indices = rb+2*32
assert rb%32 == indices%32 == 0
assert out[len(grouped):rb] == bytes(rb-len(grouped))
assert STRIP.unpack_from(out, rb)[:4] == (0, 4, 8, 0)
assert STRIP.unpack_from(out, rb+32)[:4] == (4, 4, 8, 8)
assert prepare_strips(grouped, set())[0] == grouped
selected, selected_report = prepare_strips(grouped, {1})
assert selected_report['strip_candidates'] == 1
assert models.MESH.unpack_from(selected, 64)[13:15] == (0, 0)
assert models.MESH.unpack_from(selected, 128)[13:15] == (0, 1)
# Independent ordered fan/strip check, including nonplanar points and normals.
for mid in range(2):
    mesh = models.MESH.unpack_from(out, 64+mid*64)
    record = STRIP.unpack_from(out, rb+mid*32)
    ids = struct.unpack_from('<8H', out, indices+record[3]*2)
    raw = [out[vb+(mesh[0]+i)*16:vb+(mesh[0]+i+1)*16] for i in ids]
    actual = [(raw[i+1], raw[i], raw[i+2]) if i&1 else tuple(raw[i:i+3]) for i in range(6)]
    expected = []
    for fid in range(mesh[2], mesh[2]+4):
        face = models.FACE.unpack_from(out, vb+h[3]*16+fid*32)
        v = [out[vb+(mesh[0]+i)*16:vb+(mesh[0]+i+1)*16] for i in face[:face[5]]]
        expected.extend((v[0], v[i], v[i+1]) for i in range(1, len(v)-1))
    assert all(a in (b, (b[1], b[2], b[0]), (b[2], b[0], b[1])) for a, b in zip(actual, expected))

bad = [out[:-1], out+b'x', change(out, 4, 'I', len(out)+1),
       change(out, 48, 'I', 0), change(out, 52, 'I', 0xffffffff), change(out, 56, 'I', 1),
       change(out, 64+52, 'I', 1), change(out, 64+56, 'I', 3), change(out, 64+60, 'I', 1),
       change(out, rb, 'I', 1), change(out, rb+4, 'H', 1), change(out, rb+4, 'H', 65535),
       change(out, rb+6, 'H', 7), change(out, rb+6, 'H', 9), change(out, rb+8, 'I', 1),
       change(out, rb+12, 'I', 1), change(out, rb+32, 'I', 3),
       change(out, indices, 'H', 65535), change(out, indices, 'H', 8),
       change(out, indices, 'H', 9), change(out, indices, '2H', 1, 0)]
if len(grouped) != rb:
    bad.append(change(out, len(grouped), 'B', 1))
# Wrong material while retaining an otherwise valid legacy source table.
fb = vb+h[3]*16
bad.append(change(out, fb+32+8, 'H', 1))
valid_mixed_material = change(grouped, fb+32+8, 'H', 1)
with tempfile.TemporaryDirectory(prefix='prepare-strips-') as temp:
    root = Path(temp)
    path = root/'fixture.dnm'
    if len(sys.argv) > 1:
        for data in (grouped, valid_mixed_material, out, selected):
            path.write_bytes(data)
            result = subprocess.run([sys.argv[1], str(path)], capture_output=True, text=True)
            assert result.returncode == 0, (result.stdout, result.stderr)
        for data in bad:
            path.write_bytes(data)
            result = subprocess.run([sys.argv[1], str(path)], capture_output=True, text=True)
            assert result.returncode == 1, (result.returncode, result.stdout, result.stderr)
    # Round trip the source text boundary through the real CLI and sidecar.
    (root/'manifest.json').write_text(json.dumps(dict(meshes=2, instances=2,
        unique_faces=8, unindexed_vertices=28, crtc_offset=[0, 0], render_offset=[0, 0])))
    source_instances = []
    for mid in range(2):
        mesh = models.MESH.unpack_from(grouped, 64+mid*64)
        rows = ['mode 1', 'bounds '+' '.join(map(str, mesh[4:10]))]
        for fid in range(mesh[2], mesh[2]+4):
            f = models.FACE.unpack_from(grouped, fb+fid*32)
            rows.append(f'face {f[5]} {f[7]} 16403 0 3600 41216 '+
                        ' '.join(map(str, f[8:11]))+' 128 64 0 0')
            for index in f[:f[5]]:
                vertex = models.VERTEX.unpack_from(grouped, vb+(mesh[0]+index)*16)
                rows.append('v '+' '.join(map(str, vertex)))
        (root/f'mesh_{mid}.txt').write_text('\n'.join(rows)+'\n')
        source_instances.append(f'{mid} {0x800000+mid} 0 0 1 0 0 0 1 0 0 0 1 0 0 10 '
                                '256 256 0 0 1 0 0 496 384 248 192')
    (root/'instances.txt').write_text('\n'.join(source_instances)+'\n')
    (root/'cells.txt').write_text('0 0 2\n'+''.join(f'{i} 2 0\n' for i in range(1, 256)))
    sidecar = root/'rules.json'
    sidecar.write_text('{"version":1,"defaults":{"join_strips":true,"face_group_size":2},'
                       '"geometry":{"0x800000":{"join_strips":false}}}')
    output = root/'world.dnm'
    command = [sys.executable, str(Path(models.__file__)), str(root), str(output),
               '--world-cells', '--asset-rules', str(sidecar)]
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    packed, _ = models.prepare(root)
    packed = models.group_faces(models.world_cells(packed, root), 2)
    expected, _ = prepare_strips(packed, {1})
    assert output.read_bytes() == expected
    assert json.loads(result.stdout)['strip_candidates'] == 1
    if len(sys.argv) > 1:
        result = subprocess.run([sys.argv[1], str(output)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
    # Sidecar can disable a CLI default without requiring grouped output.
    sidecar.write_text('{"version":1,"defaults":{"join_strips":false}}')
    output.unlink()
    result = subprocess.run(command+['--join-strips'], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    prepared, _ = models.prepare(root)
    assert output.read_bytes() == models.world_cells(prepared, root)
print(f'PASS DNM5 ordered nonplanar strips, unchanged source tables, selection and {len(bad)} malformed inputs'+
      (' with real C reader' if len(sys.argv) > 1 else ' (reader checks not run)'))
