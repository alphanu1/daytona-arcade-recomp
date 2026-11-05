"""ROM-free DNM6 checks: layers, sorted face order, strips, C reader inputs.

python3 platform/dreamcast-native/tests/test_prepare_face_order.py build/check_native_meshpack
"""
import struct
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import prepare_models as models
import prepare_face_order as order
from prepare_strips import prepare_strips

BODY, SHELL = 0x900000, 0x900001
OPAQUE_A, OPAQUE_B, CUTOUT, CHECKER = range(4)


def quad(corners, uvs):
    return [(*p, *uv) for p, uv in zip(corners, uvs)]


def fixture():
    """Two meshes of one 'car'. Source face order is the arcade's draw order."""
    square = [(0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]
    warped = [(0, 3, 1), (1, 3, 1), (1, 4, 0.98), (0, 4, 1)]
    tile = [(0, 0), (64, 0), (64, 64), (0, 64)]
    other = [(128, 0), (192, 0), (192, 64), (128, 64)]
    strip = [(2+i//2, i % 2, 1, 256+i*8, i*16) for i in range(6)]
    up, down = (0, 0, 1), (0, 0, -1)
    body = [
        # 0: base quad.
        (quad(square, tile), OPAQUE_A, up),
        # 1: first half of a two-quad strip, elsewhere in the same plane.
        ([strip[2], strip[0], strip[1], strip[3]], OPAQUE_B, up),
        # 2: decal on the corners of face 0, other fan diagonal and UVs.
        (quad(square[1:]+square[:1], other), CUTOUT, up),
        # 3: second half of the strip (shares two complete vertices with 1).
        ([strip[4], strip[2], strip[3], strip[5]], OPAQUE_B, up),
        # 4: small patch lying on faces 0 and 2.
        (quad([(0.1, 0.1, 1), (0.4, 0.1, 1), (0.4, 0.4, 1), (0.1, 0.4, 1)], tile), OPAQUE_A, up),
        # 5: parallel to face 0 but 0.05 away: the depth buffer's business.
        (quad([(x, y, 1.05) for x, y, _ in square], tile), OPAQUE_B, up),
        # 6: in the plane of face 0, one-sided the other way: never seen with it.
        (quad([(0, 0, 1), (0, 1, 1), (1, 1, 1), (1, 0, 1)], other), OPAQUE_B, down),
        # 7: screen-checker material the renderer never draws.
        (quad(square, other), CHECKER, up),
        # 8/9: slightly warped quad and a decal on its corners, other diagonal:
        # the base's diagonal 0-2 is the nearer one (visible side is -normal),
        # so the decal's middle lies 0.01 behind it.
        (quad(warped, tile), OPAQUE_A, up),
        (quad(warped[1:]+warped[:1], other), CUTOUT, up),
    ]
    shell = [(quad([(0.6, 0.6, 1), (0.9, 0.6, 1), (0.9, 0.9, 1), (0.6, 0.9, 1)], other), CUTOUT, up)]
    meshes, vertices, faces, instances = [], [], [], []
    for mid, (address, mesh) in enumerate(((BODY, body), (SHELL, shell))):
        unique, first_face = {}, len(faces)
        for corners, material, normal in mesh:
            ids = []
            for corner in corners:
                raw = models.VERTEX.pack(*corner)
                ids.append(unique.setdefault(raw, len(unique)))
            faces.append(models.FACE.pack(*ids, material, 4, 0, 0x201, *normal, 0))
        points = [models.VERTEX.unpack(raw)[:3] for raw in unique]
        bounds = [fn(p[a] for p in points) for fn in (min, max) for a in range(3)]
        meshes.append(models.MESH.pack(len(vertices), len(unique), first_face, len(mesh), *bounds, 1, 0, 0, 0, 0, 0))
        vertices.extend(unique)
        instances.append(models.INSTANCE.pack(mid, address, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 10,
                                              256, 256, 0, 0, 1, 0, 0, 496, 384, 248, 192, 0, 0, 0, 0))
    materials = [models.MATERIAL.pack(header, 0, 3600+i, 41216, 128, 64, 0, 0, 0, 0)
                 for i, header in enumerate((0x4013, 0x4013, 0x6013, 0xc013))]
    payload = b''.join(meshes+vertices+faces+materials+instances)
    return models.HEADER.pack(0x314d4e44, 64+len(payload), 2, len(vertices), len(faces), 4, 2,
                              0, 0, 0, 0, 0, 0, 0, 0, 0)+payload


def reader_accepts(reader, data):
    with tempfile.TemporaryDirectory(prefix='daytona-face-order-') as temp:
        path = Path(temp)/'pack.dnm'
        path.write_bytes(data)
        return subprocess.run([str(reader), str(path)], capture_output=True, text=True).returncode == 0


def tables(data):
    h = models.HEADER.unpack_from(data)
    return h, models.table_offsets(h)


def new_faces(data):
    h, off = tables(data)
    return [order.FACE6.unpack_from(data, off[2]+i*order.FACE6.size) for i in range(h[4])]


plain = fixture()
shared = models.share_positions(plain)
grouped = models.group_faces(plain, 4)
stripped, _ = prepare_strips(grouped)
parts = [[BODY, SHELL]]

# Layers: geometry and source order only.
layer, report, pairs = order.find_layers(order.read_pack(grouped), parts)
assert layer == [0, 0, 1, 0, 2, 0, 0, 0, 0, 1, 2], layer
assert {(u, o) for u, o, *_ in pairs} == {(0, 2), (0, 4), (2, 4), (8, 9), (0, 10), (2, 10)}
assert report['pairs_within_mesh'] == 4 and report['pairs_across_parts'] == 2
assert report['faces_by_layer'] == {0: 6, 1: 2, 2: 2}  # The checker face is not drawable.
dips = report['same_corner_dips']
gap, dip = next((g, d) for u, o, g, d in pairs if (u, o) == (8, 9))
# The warped decal parts from its base by half the corner's 0.02 offset.
assert gap > 1e-3 and abs(dip-0.01) < 1e-6
assert dips['count'] == 1 and abs(dips['deepest']-0.01) < 1e-6 and dips['deeper_than_0.005'] == 1
assert all(d == 0 for u, o, g, d in pairs if (u, o) != (8, 9))
# Without the part list the shell is an independent mesh.
assert order.find_layers(order.read_pack(grouped))[0][10] == 0
try:
    order.find_layers(order.read_pack(grouped), [[BODY, 0x123456]])
except ValueError:
    pass
else:
    raise AssertionError('unknown part accepted')

reader = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
for name, source in (('DNM2', shared), ('DNM4', grouped), ('DNM5', stripped),
                     ('aligned DNM4', models.align_tables(grouped))):
    out, sort_report, face_map = order.sort_pack(source, parts)
    h, off = tables(out)
    aligned = bool(h[0] & models.ALIGNED)
    assert h[0] & ~models.ALIGNED == order.LAYERED and h[1] == len(out) and aligned == name.startswith('aligned')
    assert h[14] == 3 and h[15] == 0 and h[7] == (0 if name == 'DNM2' else models.HEADER.unpack_from(grouped)[7])
    new_to_old = face_map['new_to_old']
    assert sorted(new_to_old) == list(range(11)) and face_map['layer'] == [layer[old] for old in new_to_old]
    source_h, source_off = tables(source)
    faces = new_faces(out)
    for new, old in enumerate(new_to_old):
        before = models.FACE.unpack_from(source, source_off[2]+old*models.FACE.size)
        # Only the layer is new; the group (when the source has groups) travels.
        assert faces[new][:11] == before[:11] and faces[new][11] == before[11] and faces[new][12:] == (layer[old], 0)
    # Mesh 0: opaque A (layers 0, then 2), opaque B, cutout, checker last.
    materials = [faces[i][4] for i in range(10)]
    assert materials == [OPAQUE_A]*3+[OPAQUE_B]*4+[CUTOUT]*2+[CHECKER], materials
    assert [faces[i][12] for i in range(3)] == [0, 0, 2] and new_to_old[2] == 4 and new_to_old[9] == 7
    assert sort_report['material_runs_sorted'] == sort_report['material_run_floor'] == 4
    # Source order, opaque: A B B A B B A, cutout: C C; the shell: C.
    assert sort_report['material_runs_source_order'] == 7
    # Vertices, materials, instances and position references are untouched.
    for table, size in ((1, models.VERTEX.size*h[3]), (3, models.MATERIAL.size*h[5]), (4, models.INSTANCE.size*h[6])):
        assert out[off[table]:off[table]+size] == source[source_off[table]:source_off[table]+size]
    # The two strip quads became neighbours and one strip of six vertices.
    assert h[12] == 1 and h[13] == 6 and sort_report['strip_faces'] == 2
    strip_at = out.rfind(order.STRIP.pack(new_to_old.index(1), 2, 6, 0, 0, 0, 0, 0, 0))
    assert strip_at > 0 and strip_at % 32 == 0 and {new_to_old.index(1)+1} == {new_to_old.index(3)}
    mesh0 = models.MESH.unpack_from(out, off[0])
    mesh1 = models.MESH.unpack_from(out, off[0]+models.MESH.size)
    assert mesh0[13:15] == (0, 1) and mesh1[13:15] == (1, 0)
    if reader:
        assert reader_accepts(reader, out), name
        assert reader_accepts(reader, source), name+' source'
        face_at = off[2]
        layer_at = lambda i: face_at+i*order.FACE6.size+30
        bad = bytearray(out); bad[layer_at(0)] = 3  # At or above the layer count.
        assert not reader_accepts(reader, bytes(bad))
        bad = bytearray(out); bad[layer_at(0)+1] = 1  # Remaining face padding.
        assert not reader_accepts(reader, bytes(bad))
        bad = bytearray(out); bad[layer_at(new_to_old.index(3))] = 1  # A strip of two layers.
        assert not reader_accepts(reader, bytes(bad))
        # 2026-10-12: header.padding[3] bits 0 and 1 are the static-scenery
        # flags (baked light, scenery classes; checked below); every other bit
        # stays reserved. Formerly any nonzero value: 1 was the probe.
        for field, value in ((14, 0), (14, 256), (15, 4), (13, 0)):
            fields = list(h); fields[field] = value
            bad = bytearray(out); models.HEADER.pack_into(bad, 0, *fields)
            assert not reader_accepts(reader, bytes(bad)), (name, field)
        assert not reader_accepts(reader, out[:-4])
    try:
        order.sort_pack(out, parts)
    except ValueError:
        pass
    else:
        raise AssertionError('DNM6 input accepted: its source order is gone')

# Optional: a dipping decal takes its base's diagonal. Same quad, corners,
# UVs, winding, layer and order; one corner of rotation; nothing else moves.
strict, _, strict_map = order.sort_pack(grouped, parts)
matched, matched_report, matched_map = order.sort_pack(grouped, parts, match_diagonals=True)
assert matched_report['decal_diagonals_matched'] == 1 and matched_report['decal_diagonal_conflicts'] == 0
assert matched_report['same_corner_dips_before']['count'] == 1 and matched_report['same_corner_dips']['count'] == 0
assert matched_map['rotated'] == [9] and strict_map['rotated'] == []
assert matched_map['new_to_old'] == strict_map['new_to_old'] and matched_map['layer'] == strict_map['layer']
assert len(matched) == len(strict)
for new, (a, b) in enumerate(zip(new_faces(strict), new_faces(matched))):
    if strict_map['new_to_old'][new] == 9:
        # Base corners 0..3, decal 1,2,3,0: three corners back puts the base's
        # corner 2 at the decal's corner 2 as well as matching the diagonal.
        assert b[:4] == a[3:4]+a[:3] and b[4:] == a[4:]
    else:
        assert a == b
h, off = tables(strict)
assert strict[:off[2]] == matched[:off[2]] and strict[off[3]:] == matched[off[3]:]
if reader:
    assert reader_accepts(reader, matched)

# Static scenery: baked light and scenery classes (meshpack.h flags).
light = (0.0, 0.0, 0.5)
baked, baked_report, baked_map = order.sort_pack(grouped, parts, static_light=light,
                                                 reduced_headers=None)
h, off = tables(baked)
assert h[15] == order.BAKED_LIGHT and baked_map['new_to_old'] == strict_map['new_to_old']
for new, face in enumerate(new_faces(baked)):
    old_face = strict_map['new_to_old'][new]
    # Fixture materials: diffuse 128, ambient 64; normals (0,0,+-1); one-sided.
    # The side the normal points to is lit when it points along the light.
    lit = old_face != 6
    assert face[6] == (128 if lit else 64) and face[13] == 0, (old_face, face[6], face[13])
    assert face[:6] == new_faces(strict)[new][:6] and face[7:13] == new_faces(strict)[new][7:13]
assert baked_report['baked_two_sided_faces_with_two_lumas'] == 0
# A two-sided face carries the other side's luma as well.
two_sided = bytearray(grouped)
g_h, g_off = tables(grouped)
at = g_off[2]+6*models.FACE.size
face = list(models.FACE.unpack_from(two_sided, at)); face[7] |= 1 << 17
models.FACE.pack_into(two_sided, at, *face)
both, both_report, both_map = order.sort_pack(bytes(two_sided), parts, static_light=light)
face = new_faces(both)[both_map['new_to_old'].index(6)]
assert (face[6], face[13]) == (64, 128) and both_report['baked_two_sided_faces_with_two_lumas'] == 1
try:
    computed = bytearray(grouped)
    mesh = list(models.MESH.unpack_from(computed, g_off[0])); mesh[10] = 2
    models.MESH.pack_into(computed, g_off[0], *mesh)
    order.sort_pack(bytes(computed), parts, static_light=light)
except ValueError:
    pass
else:
    raise AssertionError('view-dependent light was baked')
# Classes: material A is the "reduced view" here; B is opaque but not ground
# (ground needs every normal to point road-up, -Y; these are +-Z); cutout and
# checker follow from the texture header.
track = 'static const uint16_t surface_headers[][4]={\n' + ',\n'.join(
    '{%d,0,%d,41216}' % ((0x4013, 3600) if i == 0 else (0x4000+i, 9000+i)) for i in range(40)) + '\n};\nint nm_track_surface_select'
assert order.reduced_headers(track)[0] == (0x4013, 0, 3600, 41216)
classed, classed_report, _ = order.sort_pack(grouped, parts, static_light=light,
                                             reduced_headers=[(0x4013, 0, 3600, 41216)])
h, off = tables(classed)
assert h[15] == order.BAKED_LIGHT | order.SCENERY_CLASSES
words = [struct.unpack_from('<2I', classed, off[3]+m*models.MATERIAL.size+24) for m in range(4)]
assert words == [(order.STAGE_REDUCED, 0), (order.STAGE_OPAQUE, 0), (order.STAGE_CUTOUT, 0), (order.STAGE_CHECKER, 0)], words
assert classed_report['scenery_classes'] == {'stage0': 1, 'stage1': 1, 'stage3': 1, 'stage4': 1}
try:
    order.sort_pack(grouped, parts, reduced_headers=[(1, 2, 3, 4)])
except ValueError:
    pass
else:
    raise AssertionError('unknown reduced-view header accepted')
if reader:
    assert reader_accepts(reader, baked) and reader_accepts(reader, both) and reader_accepts(reader, classed)
    h, off = tables(baked)
    one_sided = next(i for i, f in enumerate(new_faces(baked)) if not f[7] & (1 << 17))
    bad = bytearray(baked); bad[off[2]+one_sided*order.FACE6.size+31] = 9   # Back luma of a one-sided face.
    assert not reader_accepts(reader, bytes(bad))
    # Without the flags the same bytes are padding and must be zero.
    fields = list(h); fields[15] = 0
    bad = bytearray(baked); models.HEADER.pack_into(bad, 0, *fields)
    assert not reader_accepts(reader, bytes(bad))
    h, off = tables(classed)
    # A stage beyond the last and an undefined class bit are refused. 0x200 is
    # NM_SCENERY_POINT (split cutouts) since that class exists; 0x400 is the
    # lowest undefined bit now.
    accepted = (order.SCENERY_SURFACE | order.STAGE_GROUND, order.SCENERY_POINT | order.STAGE_CUTOUT)
    assert order.SCENERY_POINT == 0x200
    # The last stage is the world objects' (meshpack.h, NM_SCENERY_STAGE_OBJECTS = STAGE_CHECKER+1).
    assert order.SCENERY_STAGE_OBJECTS == order.STAGE_CHECKER+1
    for word in (order.SCENERY_STAGE_OBJECTS+1, 0x400, 0x400 | order.SCENERY_POINT, order.SCENERY_POINT | (order.SCENERY_STAGE_OBJECTS+1))+accepted:
        bad = bytearray(classed); struct.pack_into('<I', bad, off[3]+24, word)
        assert reader_accepts(reader, bytes(bad)) == (word in accepted), word
    bad = bytearray(classed); struct.pack_into('<I', bad, off[3]+28, 1)
    assert not reader_accepts(reader, bytes(bad))
    fields = list(h); fields[15] = order.BAKED_LIGHT
    bad = bytearray(classed); models.HEADER.pack_into(bad, 0, *fields)
    assert not reader_accepts(reader, bytes(bad))   # Class words without their flag.

# Static scenery without a part list: the shell's quad lies in the plane of
# the body's faces 0, 2 and 4 and is smaller than 0 and 2: it goes on top of
# them, whichever mesh comes first.
by_area, area_report, area_pairs = order.find_layers(order.read_pack(grouped), cross_mesh_by_area=True)
assert area_report['pairs_across_meshes_by_area'] == 2 and area_report['pairs_across_parts'] == 0
assert {(u, o) for u, o, *_ in area_pairs if 10 in (u, o)} == {(0, 10), (2, 10)} and by_area[10] == 2

# Without a part list the body's own decals still give three layers.
single = models.share_positions(fixture())
flat, flat_report, _ = order.sort_pack(single, strip_scope='normal')
assert models.HEADER.unpack_from(flat)[14] == 3
if reader:
    # Older formats keep rejecting what DNM6 newly allows.
    h, off = tables(shared)
    bad = bytearray(shared); bad[off[2]+30] = 1
    assert reader_accepts(reader, shared) and not reader_accepts(reader, bytes(bad))

# Strip chains keep each face's fan triangles, order and winding.
corners = {7: (2, 0, 1, 3), 8: (4, 2, 3, 5), 9: (4, 5, 6), 10: (6, 5, 7)}
chains = order.chain_faces(corners)
assert chains == [([7, 8, 9, 10], [0, 1, 2, 3, 4, 5, 6, 7])], chains
assert order.chain_faces({1: (0, 1, 2, 3), 2: (4, 5, 6, 7)}) == [([1], None), ([2], None)] or \
    [ids for ids, _ in order.chain_faces({1: (0, 1, 2, 3), 2: (4, 5, 6, 7)})] == [[1], [2]]

# Part lists from a generated header: body/window/wheel-phase and crash/body.
ids = list(range(100, 132))
text = ('static const uint32_t nm_intro_model_ids[] = {'+','.join(map(str, ids))+'};\n'
        'static const uint32_t nm_intro_wheel_phase_models[] = {'+','.join(map(str, range(8, 24)))+'};\n'
        'static const uint32_t nm_intro_window_models[] = {0,1};\n'
        'static const uint32_t nm_intro_crash_models[] = {2,3};\n'
        'static const uint32_t nm_intro_special_model = 4;\n'
        'static const uint32_t nm_intro_body_models[][8] = {\n{5,5,5,6,7,7,7,5},\n};\n')
lists = order.intro_part_lists(text)
assert [105, 100, 108] in lists and [106, 101, 116] in lists       # Levels 4 and 3 (columns 2, 3).
assert [103, 107] in lists and [102, 107] in lists                  # Crash attachment first.
assert [104, 100, 115] in lists and lists[-1] == [0x95ff9d, 0x13b, 0x961bbf]
assert [105, 101, 108] not in lists and [105, 100, 116] not in lists   # No cross-level mixes.

# Static scenery: flattened stacks and stipple panels.
def scenery_fixture():
    """One mesh. 0..2: an opaque face and two punch-through faces on the very
    same polygon and texture coordinates (a stack); 3: an opaque face alone;
    4: a punch-through face on the corners of 3 with other texture coordinates
    (not a stack); 5, 6: two screen-checker panels sharing a post."""
    tile = [(0, 0), (64, 0), (64, 64), (0, 64)]
    other = [(128, 0), (192, 0), (192, 64), (128, 64)]
    first = [(0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]
    second = [(4, 0, 1), (5, 0, 1), (5, 1, 1), (4, 1, 1)]
    nothing = [(0, 0)]*4
    up = (0, 0, 1)
    mesh = [(quad(first, tile), 0), (quad(first, tile), 1), (quad(first[2:]+first[:2], tile[2:]+tile[:2]), 2),
            (quad(second, tile), 0), (quad(second, other), 1),
            (quad([(0, 1, 5), (0, 0, 5), (3, 0, 5), (3, 1, 5)], nothing), 3),
            (quad([(3, 1, 5), (3, 0, 5), (5, 0, 5), (5, 1, 5)], nothing), 3)]
    unique, faces = {}, []
    for corners, material in mesh:
        ids = [unique.setdefault(models.VERTEX.pack(*corner), len(unique)) for corner in corners]
        faces.append(models.FACE.pack(*ids, material, 4, 0, 0x201, *up, 0))
    points = [models.VERTEX.unpack(raw)[:3] for raw in unique]
    bounds = [fn(p[a] for p in points) for fn in (min, max) for a in range(3)]
    meshes = [models.MESH.pack(0, len(unique), 0, len(mesh), *bounds, 1, 0, 0, 0, 0, 0)]
    instances = [models.INSTANCE.pack(0, BODY, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 10,
                                      256, 256, 0, 0, 1, 0, 0, 496, 384, 248, 192, 0, 0, 0, 0)]
    materials = [models.MATERIAL.pack(header, 0, 3600+i, 41216+i, 128, 64, 0, 0, 0, 0)
                 for i, header in enumerate((0x4013, 0x6013, 0x6013, 0x8000))]
    payload = b''.join(meshes+list(unique)+faces+materials+instances)
    return models.HEADER.pack(0x314d4e44, 64+len(payload), 1, len(unique), len(faces), 4, 1,
                              0, 0, 0, 0, 0, 0, 0, 0, 0)+payload


scenery = models.group_faces(scenery_fixture(), 4)
track = [(0x4013, 0, 3600, 41216)]
untouched, _, untouched_map = order.sort_pack(scenery, static_light=light, reduced_headers=track)
assert tables(untouched)[0][4:6] == (7, 4) and untouched_map['composites'] == []
made, made_report, made_map = order.sort_pack(scenery, static_light=light, reduced_headers=track, stipple=True, flatten=True)
h, off = tables(made)
flat_report = made_report['flattened_stacks']
assert h[4:6] == (5, 5) and flat_report['stacks'] == 1 and flat_report['faces_removed'] == 2
assert [r['face'] for r in flat_report['removed']] == [1, 2] and all(r['under'] == 0 for r in flat_report['removed'])
assert sorted(made_map['new_to_old']) == [0, 3, 4, 5, 6]
composite, = made_map['composites']
assert composite['header'] == [0x4013, order.COMPOSITE_MARK, 3600, 41216] and composite['faces'] == 1
assert composite['layers'] == [[0x4013, 0, 3600, 41216], [0x6013, 0, 3601, 41217], [0x6013, 0, 3602, 41218]]
assert list(composite['lumas'].values()) == [[128]*3]
headers = [struct.unpack_from('<4H', made, off[3]+m*models.MATERIAL.size) for m in range(5)]
assert headers[4] == tuple(composite['header']) and headers[3] == order.stipple_header((0x8000, 0, 3603, 41219))
assert (headers[3][0] >> 13) & 7 == 3 and headers[3][1] == order.STIPPLE_MARK
# The composite keeps its base's light terms; the panels keep the last stage.
assert made[off[3]+4*models.MATERIAL.size+8:off[3]+4*models.MATERIAL.size+24] == made[off[3]+8:off[3]+24]
assert struct.unpack_from('<I', made, off[3]+3*models.MATERIAL.size+24)[0] == order.STAGE_CHECKER
faces_made = new_faces(made)
assert sorted(f[4] for f in faces_made) == [0, 1, 3, 3, 4]
# Panels: u runs 3 units along the fence and turns back by 2 at the shared
# post (the stipple is its own mirror image); v is the height, one unit.
unit = order.STIPPLE_TEXELS_PER_UNIT*8
panel_uv = {}
for face in faces_made:
    if face[4] == 3:
        for i in face[:4]:
            x, y, z, u, v = models.VERTEX.unpack_from(made, off[1]+i*models.VERTEX.size)
            panel_uv[x, y] = (u, v)
assert {x: panel_uv[x, 0.0][0] for x in (0.0, 3.0, 5.0)} == {0.0: 0, 3.0: 3*unit, 5.0: unit}, panel_uv
assert all(panel_uv[x, 0.0][0] == panel_uv[x, 1.0][0] and abs(panel_uv[x, 0.0][1]-panel_uv[x, 1.0][1]) == unit and
           panel_uv[x, 0.0][1] == panel_uv[0.0, 0.0][1] for x in (0.0, 3.0, 5.0)), panel_uv
assert made_report['stipple_panels']['panels'] == 2
# A limit of zero flattens nothing and names the stack as a candidate.
none, none_report, none_map = order.sort_pack(scenery, static_light=light, reduced_headers=track, flatten=0)
assert none == untouched
try:
    order.sort_pack(scenery, reduced_headers=track, flatten=True)
except ValueError:
    pass
else:
    raise AssertionError('stacks flattened without baked light')
if reader:
    assert reader_accepts(reader, made) and reader_accepts(reader, untouched)
# Road decals (--drape-road-decals, road_drape.py): a road over a crest (two
# planar quads meeting at x=2, 0.4 high) and two punch-through decal quads
# 0.05 above it: one on the first quad alone, one across the crest, whose
# chord dips 0.25 under the road there.
def road_fixture():
    def height(x):
        return 0.4*x/2 if x <= 2 else 0.4*(4-x)/2
    def strip(x0, x1, lift):
        return [(x0, height(x0)+lift, 0.25), (x1, height(x1)+lift, 0.25), (x1, height(x1)+lift, 0.75), (x0, height(x0)+lift, 0.75)]
    tile = [(0, 0), (64, 0), (64, 64), (0, 64)]
    mesh = [(quad([(0, 0, 0), (2, 0.4, 0), (2, 0.4, 1), (0, 0, 1)], tile), 0),
            (quad([(2, 0.4, 0), (4, 0, 0), (4, 0, 1), (2, 0.4, 1)], tile), 0),
            (quad(strip(0.2, 1.8, 0.05), tile), 1), (quad(strip(0.5, 3.5, 0.05), tile), 1)]
    unique, faces = {}, []
    for corners, material in mesh:
        ids = [unique.setdefault(models.VERTEX.pack(*corner), len(unique)) for corner in corners]
        faces.append(models.FACE.pack(*ids, material, 4, 0, 0x201, 0, -1, 0, 0))
    points = [models.VERTEX.unpack(raw)[:3] for raw in unique]
    bounds = [fn(p[a] for p in points) for fn in (min, max) for a in range(3)]
    meshes = [models.MESH.pack(0, len(unique), 0, len(mesh), *bounds, 1, 0, 0, 0, 0, 0)]
    instances = [models.INSTANCE.pack(0, BODY, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 10,
                                      256, 256, 0, 0, 1, 0, 0, 496, 384, 248, 192, 0, 0, 0, 0)]
    materials = [models.MATERIAL.pack(header, 0, 3600+i, 41216+i, 128, 64, 0, 0, 0, 0)
                 for i, header in enumerate((0x4013, 0x6013))]
    payload = b''.join(meshes+list(unique)+faces+materials+instances)
    return models.HEADER.pack(0x314d4e44, 64+len(payload), 1, len(unique), len(faces), 2, 1,
                              0, 0, 0, 0, 0, 0, 0, 0, 0)+payload


road = models.group_faces(road_fixture(), 4)
lifted, lifted_report, _ = order.sort_pack(road, static_light=light, reduced_headers=track)
assert lifted_report['scenery_classes']['road_decal_materials'] == 1
assert sorted(f[12] for f in new_faces(lifted)) == [0, 0, order.ROAD_DECAL_LAYER, order.ROAD_DECAL_LAYER]
draped, draped_report, draped_map = order.sort_pack(road, static_light=light, reduced_headers=track, drape={})
drape_report = draped_report['road_drape']
assert drape_report['faces_raised'] == 1 and drape_report['corners_raised'] == 4 and drape_report['deepest_under_road_left'] == 0.0
assert abs(drape_report['deepest_under_road_before_cuts']-0.25) < 1e-5 and abs(drape_report['raise_max']-0.25) < 1e-5
assert draped_report['road_decal_layer'] == 1 and sorted(f[12] for f in new_faces(draped)) == [0, 0, 1, 1]
h, off = tables(draped)
by_face = {}
for new, face in zip(draped_map['new_to_old'], new_faces(draped)):
    by_face[new] = [models.VERTEX.unpack_from(draped, off[1]+i*models.VERTEX.size) for i in face[:4]]
source = models.group_faces(road_fixture(), 4)
sh, soff = tables(source)
def source_corners(f):
    face = models.FACE.unpack_from(source, soff[2]+f*models.FACE.size)
    return [models.VERTEX.unpack_from(source, soff[1]+i*models.VERTEX.size) for i in face[:4]]
assert by_face[2] == source_corners(2)                     # On the first road quad: as the arcade has it.
for got, was in zip(by_face[3], source_corners(3)):         # Across the crest: x, z, u, v kept, 0.25 higher.
    assert got[0] == was[0] and got[2:] == was[2:] and abs(got[1]-was[1]-0.25) < 1e-5, (got, was)
for f in (0, 1):
    assert by_face[f] == source_corners(f)
if reader:
    assert reader_accepts(reader, draped)
print('PASS: DNM6 layers from geometry and part order, sorted runs, strips, preserved tables, baked light, scenery classes, flattened stacks, stipple panels, road decals on or above the road, reader acceptance and malformed inputs')
