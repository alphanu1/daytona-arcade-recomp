"""ROM-free exact cutout checks; optional argv[1] is the real C pack reader."""
from pathlib import Path
import hashlib
import json
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"tools"))
import prepare_models as models
from optimize_flat_cutouts import collapse_flat_cutouts


def quad(x):
    return [(x, 0, 0, x*256, 0), (x+1, 0, 0, (x+1)*256, 0),
            (x+1, 1, 0, (x+1)*256, 256), (x, 1, 0, x*256, 256)]


def pack(mesh_faces, mode=1):
    meshes, vertices, faces, instances = [], [], [], []
    for mid, polygons in enumerate(mesh_faces):
        unique = {}
        first_v, first_f = len(vertices), len(faces)
        for polygon in polygons:
            ids = []
            for v in polygon:
                raw = models.VERTEX.pack(*v)
                if raw not in unique:
                    unique[raw] = len(unique)
                    vertices.append(raw)
                ids.append(unique[raw])
            count = len(ids)
            ids += [0]*(4-count)
            faces.append(models.FACE.pack(*ids, 0, count, 0, 0x20201 if count == 4 else 0x20202,
                                           -0.0, 0, 1, 0))
        points = [v[:3] for p in polygons for v in p]
        bounds = [fn(p[k] for p in points) for fn in (min, max) for k in range(3)]
        meshes.append(models.MESH.pack(first_v, len(unique), first_f, len(polygons),
                                       *bounds, mode, 0, 0, 0, 0, 0))
        instances.append(models.INSTANCE.pack(mid, 0x800000+mid, 1, 0,
            1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 10, 256, 256, 0, 0, 1,
            -1, 128, 496, 512, 248, 270, 0, 0, 0, 0))
    material = models.MATERIAL.pack(0x6013, 0, 3600, 41216, 255, 255, 255, 7, 0, 0)
    payload = b''.join(meshes+vertices+faces+[material]+instances)
    return models.HEADER.pack(0x314d4e44, 64+len(payload), len(meshes), len(vertices),
        len(faces), 1, len(instances), 0, -1, 2, 3, -4, 0, 0, 0, 0)+payload


def tables(data):
    # Independent offsets/decoding catch accidental resource/instance rewrites.
    h = struct.unpack_from('<8I4i4I', data)
    vb, fb = 64+h[2]*64, 64+h[2]*64+h[3]*16
    mb, ib = fb+h[4]*32, fb+h[4]*32+h[5]*32
    return h, vb, fb, mb, ib


def mutate(data, offset, fmt, value):
    out = bytearray(data)
    struct.pack_into('<'+fmt, out, offset, value)
    return bytes(out)


def unchanged(data):
    assert collapse_flat_cutouts(data) == (data, 0)


def rejected(data):
    try:
        collapse_flat_cutouts(data)
    except ValueError:
        return
    raise AssertionError('malformed DNM1 accepted')


def merged(data, count):
    out, actual = collapse_flat_cutouts(data)
    assert actual == count, (actual, count)
    h, vb, fb, mb, ib = tables(data)
    oh, ovb, ofb, omb, oib = tables(out)
    assert oh[0] == h[0] and oh[1] == len(out)
    assert oh[2] == h[2] and oh[4] == h[4]-count and oh[5:] == h[5:]
    assert out[omb:oib] == data[mb:ib] and out[oib:] == data[ib:]
    for mid in range(h[2]):
        assert out[64+mid*64+16:64+(mid+1)*64] == data[64+mid*64+16:64+(mid+1)*64]
        old = struct.unpack_from('<4I', data, 64+mid*64)
        new = struct.unpack_from('<4I', out, 64+mid*64)
        old_vs = [data[vb+(old[0]+i)*16:vb+(old[0]+i+1)*16] for i in range(old[1])]
        new_vs = [out[ovb+(new[0]+i)*16:ovb+(new[0]+i+1)*16] for i in range(new[1])]
        assert new_vs == [v for v in old_vs if v in new_vs]
        for i in range(new[3]):
            raw = out[ofb+(new[2]+i)*32:ofb+(new[2]+i+1)*32]
            assert raw[8:] == data[fb+old[2]*32+8:fb+(old[2]+1)*32]
    assert collapse_flat_cutouts(out) == (out, 0)
    valid_outputs.extend((data, out, models.group_faces(out)))
    return out


valid_outputs = []
base = pack([[quad(0), quad(1)]])
h, vb, fb, mb, ib = tables(base)
out = merged(base, 1)
oh, ovb, ofb, omb, oib = tables(out)
assert oh[3] == 4 and oh[4] == 1 and len(base)-len(out) == 64
ids = struct.unpack_from('<4H', out, ofb)
assert [struct.unpack_from('<3f2H', out, ovb+i*16) for i in ids] == [quad(0)[0], quad(1)[1], quad(1)[2], quad(0)[3]]
# Four faces meet at the common vertical edge, in two independent planes.
crossed = [[(1, y, x-1, u, v) for x, y, z, u, v in q] for q in (quad(0), quad(1))]
merged(pack([[quad(0), quad(1), *crossed]]), 2)
merged(pack([[quad(0), quad(1)], [quad(0), quad(1)]]), 2)
# Keep the first face's sequence position, including unaffected triangle siblings.
ordered = pack([[quad(4)[:3], quad(0), quad(1), quad(6)[:3]]])
ordered_out, count = collapse_flat_cutouts(ordered)
assert count == 1
for old_f, new_f in ((0, 0), (3, 2)):
    def face_vertices(data, fid):
        h, vb, fb, mb, ib = tables(data)
        f = struct.unpack_from('<5HBBI3fI', data, fb+fid*32)
        return [data[vb+v*16:vb+(v+1)*16] for v in f[:f[5]]]
    assert face_vertices(ordered, old_f) == face_vertices(ordered_out, new_f)
valid_outputs.extend((ordered, ordered_out))
# Exact slanted planes and normals not parallel to the plane remain eligible.
slanted = [[(x, y, x+2*y, u, v) for x, y, z, u, v in q] for q in (quad(0), quad(1))]
merged(pack([slanted]), 1)
# Preserve negative zero corner bytes; opposite signed zeros at a seam cannot weld.
signed = [[(-0.0 if x == 0 else x, y, z, u, v) for x, y, z, u, v in q] for q in (quad(0), quad(1))]
signed_out = merged(pack([signed]), 1)
assert signed_out[tables(signed_out)[1]:tables(signed_out)[1]+4] == b'\0\0\0\x80'
seam = [quad(0), [(x, y, -0.0, u, v) for x, y, z, u, v in quad(1)]]
unchanged(pack([seam]))
# Class, checker, ambient, side and mode restrictions; nonzero specular 7 is valid.
for offset, fmt, value in ((mb, 'H', 0x4013), (mb, 'H', 0xe013),
                            (mb+12, 'f', 254), (64+40, 'I', 2), (64+40, 'I', 3)):
    unchanged(mutate(base, offset, fmt, value))
no_side = mutate(mutate(base, fb+12, 'I', 0x201), fb+32+12, 'I', 0x201)
unchanged(no_side)
for offset, fmt, value in ((fb+32+12, 'I', 0x20301), (fb+32+16, 'f', 1)):
    unchanged(mutate(base, offset, fmt, value))
# A different material ID is a seam even when material bytes are identical.
two_materials = bytearray(base[:ib]+base[mb:ib]+base[ib:])
struct.pack_into('<I', two_materials, 4, len(two_materials))
struct.pack_into('<I', two_materials, 20, 2)
struct.pack_into('<H', two_materials, fb+32+8, 1)
unchanged(bytes(two_materials))
for polygons in ([quad(0), quad(2)], [quad(0), quad(0)], [quad(0), quad(1)[::-1]]):
    unchanged(pack([polygons]))
for kind in ('uv_seam', 'nonaffine', 'warped', 'concave'):
    q = [list(v) for v in quad(1)]
    if kind == 'uv_seam':
        for v in q: v[3] -= 256
    elif kind == 'nonaffine': q[2][4] += 1
    elif kind == 'warped': q[2][2] = 2**-149
    else: q[2] = [1.25, 0.25, 0, 320, 64]
    unchanged(pack([[quad(0), q]]))
# No greedy overlap: the third face survives this one pass.
three = pack([[quad(0), quad(1), quad(2)]])
three_out, count = collapse_flat_cutouts(three)
assert count == 1 and tables(three_out)[0][4] == 2
valid_outputs.extend((three, three_out))
# Four adjacent faces take two passes; this helper intentionally is not greedy.
four_out, count = collapse_flat_cutouts(pack([[quad(i) for i in range(4)]]))
assert count == 2
four_final, count = collapse_flat_cutouts(four_out)
assert count == 1 and collapse_flat_cutouts(four_final) == (four_final, 0)
valid_outputs.extend((four_out, four_final))
for bad in (b'', base[:63], base[:-1], base+b'x',
            mutate(base, 0, 'I', 0x324d4e44), mutate(base, 0, 'I', 0x344d4e44),
            mutate(base, 4, 'I', len(base)+1), mutate(base, 8, 'I', 0),
            mutate(base, 28, 'I', 1), mutate(base, 48, 'I', 1),
            mutate(base, 64, 'I', 1), mutate(base, 68, 'I', 0),
            mutate(base, 76, 'I', 3), mutate(base, fb, 'H', 65535),
            mutate(base, fb+8, 'H', 1), mutate(base, fb+10, 'B', 2),
            mutate(base, vb, 'f', float('nan'))):
    rejected(bad)
with tempfile.TemporaryDirectory(prefix='flat-cutout-cli-') as temp:
    root = Path(temp)
    (root/'manifest.json').write_text(json.dumps(dict(meshes=1, instances=1,
        unique_faces=2, unindexed_vertices=8, crtc_offset=[-1, 2], render_offset=[3, -4])))
    rows = ['mode 1', 'bounds 0 0 0 2 1 0']
    for q in (quad(0), quad(1)):
        rows.append('face 4 131585 24595 0 3600 41216 -0.0 0 1 255 255 7 255')
        rows.extend('v '+' '.join(map(str, v)) for v in q)
    (root/'mesh_0.txt').write_text('\n'.join(rows)+'\n')
    (root/'instances.txt').write_text('0 8388608 1 0 1 0 0 0 1 0 0 0 1 0 0 10 '
                                     '256 256 0 0 1 -1 128 496 512 248 270\n')
    prepared, _ = models.prepare(root)
    collapsed, count = collapse_flat_cutouts(prepared)
    assert count == 1
    for name, flags, expected in (('default', [], prepared),
        ('collapsed', ['--collapse-flat-cutouts'], collapsed),
        ('grouped', ['--collapse-flat-cutouts', '--face-group-size', '2'], models.group_faces(collapsed, 2))):
        path = root/(name+'.dnm')
        result = subprocess.run([sys.executable, str(Path(models.__file__)), str(root), str(path), *flags],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        report = json.loads(result.stdout)
        assert path.read_bytes() == expected
        assert report['bytes'] == len(expected) and report['sha256'] == hashlib.sha256(expected).hexdigest()
        if flags:
            assert report['flat_cutout_pairs_collapsed'] == 1 and report['faces'] == 1
            assert report['flat_cutout_vertices_removed'] == 2 and report['vertices'] == 4
        else:
            assert 'flat_cutout_pairs_collapsed' not in report
        valid_outputs.append(expected)
if len(sys.argv) > 1:
    with tempfile.TemporaryDirectory(prefix='flat-cutout-test-') as temp:
        path = Path(temp)/'test.dnm'
        for data in valid_outputs:
            path.write_bytes(data)
            result = subprocess.run([sys.argv[1], str(path)], capture_output=True, text=True)
            assert result.returncode == 0, (result.stdout, result.stderr)
print('PASS exact flat cutouts, preservation, exclusions, malformed input and CLI' +
      (' and real C reader' if len(sys.argv) > 1 else ''))
