"""ROM-free checks of level-of-detail groups (tools/lod_groups.py,
prepare_face_order.py --lod-groups, the DNM6 table of meshpack.h).

python3 platform/dreamcast-native/tests/test_lod_groups.py [build/check_native_meshpack]
"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

try:
    import numpy as np
except ImportError:      # The tools need numpy, as prepare_palette_materials.py does.
    print('SKIP: numpy is not installed for this interpreter')
    sys.exit(77)

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import prepare_models as models
import prepare_face_order as order
import opaque_cores as cores
import split_cutouts as split
import lod_groups as lod

reader = sys.argv[1] if len(sys.argv) > 1 else None

# A 64x64 texture: an opaque triangle (a tree on its side) with a one-texel
# hole and a notch, a thin trunk, everything else entry 15.
image = np.full((64, 64), 15, np.uint8)
for y in range(64):
    for x in range(48):
        if abs(y-32) <= x//2:
            image[y, x] = 1+(x+y) % 14
image[30:34, 48:64] = 3
image[32, 20] = 15
image[20:25, 40] = 15
near = cores.design_overlay(image, hole=8, close=1, cores=2, vertices=5)
near['options'] = dict(kind='overlay')
far = cores.design_outline(image, vertices=4, thin=3, least=20)
far['options'] = dict(kind='outline')
assert near['fill'] and far['fill'] and {tuple(t) for t in near['fill']} != {tuple(t) for t in far['fill']+far['gutter']}


def pack(faces, vertices, materials, lights=None, word=0):
    """A DNM2 pack of one mesh. faces: (indices, material, attribute, normal); lights: (diffuse, ambient) per
    material (default 100, 60 each); word: the instance's object word (padding[0])."""
    lows = [min(v[a] for v in vertices) for a in range(3)]
    highs = [max(v[a] for v in vertices) for a in range(3)]
    mesh = models.MESH.pack(0, len(vertices), 0, len(faces), *lows, *highs, 0, 0, 0, 0, 0, 0)
    body = b''.join(models.VERTEX.pack(*v) for v in vertices)
    body += b''.join(models.FACE.pack(*(list(i)+[0]*(4-len(i))), m, len(i), 0, a, *n, 0) for i, m, a, n in faces)
    lights = lights or [(100.0, 60.0)]*len(materials)
    body += b''.join(models.MATERIAL.pack(*h, *lights[m], 0.0, 0, 0, 0) for m, h in enumerate(materials))
    body += models.INSTANCE.pack(0, 0x1000, 0, 0, *([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]), 1, 1, 0, 0, 1, 0, 0, 639, 479, 0, 0,
                                 word, 0, 0, 0)
    refs = [next(j for j in range(i+1) if vertices[j][:3] == v[:3]) for i, v in enumerate(vertices)]
    body += struct.pack(f'<{len(refs)}H', *refs)
    size = models.HEADER.size+len(mesh)+len(body)
    return models.HEADER.pack(0x324d4e44, size, 1, len(vertices), len(faces), len(materials), 1, 0, 0, 0, 0, 0, 0, 0, 0, 0)+mesh+body


def tables(data):
    h = models.HEADER.unpack_from(data)
    off = models.table_offsets(h)
    count = h[15] >> 16
    return (h, [order.FACE6.unpack_from(data, off[2]+f*order.FACE6.size) for f in range(h[4])],
            [models.VERTEX.unpack_from(data, off[1]+i*models.VERTEX.size) for i in range(h[3])],
            [struct.unpack_from('<4H', data, off[3]+m*models.MATERIAL.size) for m in range(h[5])],
            [struct.unpack_from('<I', data, off[3]+m*models.MATERIAL.size+24)[0] for m in range(h[5])],
            [lod.LOD.unpack_from(data, len(data)-(count-g)*lod.LOD.size) for g in range(count)],
            models.MESH.unpack_from(data, models.HEADER.size))


def accepted(data):
    if not reader:
        return None
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder)/'lod.dnm'
        path.write_bytes(data)
        return subprocess.run([reader, str(path)], capture_output=True).returncode == 0


CUTOUT, WALL, CHECKER, SIGN = (0x6009, 0, 100, 41216), (0x4009, 0, 200, 41216), (0x8000, 0, 0, 39808), (0x4009, 0, 400, 41216)
two_sided = 0x1a01 | 1 << 17
vertices = [(-2, -2, 5, 0, 0), (8, -2, 5, 512, 0), (8, 8, 5, 512, 512), (-2, 8, 5, 0, 512),       # a board the first tree stands on
            (0, 0, 5, 0, 0), (4, 0, 5, 512, 0), (4, 4, 5, 512, 512), (0, 4, 5, 0, 512),            # the tree, two-sided
            (1, 1, 5, 0, 0), (3, 1, 5, 512, 0), (3, 3, 5, 512, 512), (1, 3, 5, 0, 512),            # a sign nailed to it
            (20, 0, 5, 512, 0), (26, 0, 5, 0, 0), (26, 6, 5, 0, 512), (20, 6, 5, 512, 512),        # a larger tree, UVs turned over
            (0, 0, 30, 0, 0), (0, 4, 30, 0, 0), (8, 4, 30, 0, 0), (8, 0, 30, 0, 0)]                # a fence panel: top A, bottom A, bottom B, top B
faces = [((0, 1, 2, 3), 1, 0x1a01, (0.0, 0.0, 1.0)), ((4, 5, 6, 7), 0, two_sided, (0.0, 0.0, 1.0)),
         ((8, 9, 10, 11), 3, 0x1a01, (0.0, 0.0, 1.0)), ((12, 13, 14, 15), 0, two_sided, (0.0, 0.0, 1.0)),
         ((16, 17, 18, 19), 2, 0x1a01, (0.0, 0.0, 1.0))]
source = pack(faces, vertices, [CUTOUT, WALL, CHECKER, SIGN])
light = (-0.45, -0.89, -0.45)
pictures = {100: image}
spec = dict(reference_focus=280, expected_focus=210, rules=[
    dict(name='trees', textures=[100], levels=[dict(spec=dict(textures={'100': near})), dict(spec=dict(textures={'100': far})), 'none'],
         switch=[dict(pixels=60, of='height'), dict(depth=500.0)]),
    dict(name='fence', checker=True, levels=['source', 'none'], switch=[dict(texel_pixels=1.0, reach='radius')])])
with tempfile.TemporaryDirectory() as folder:
    path = Path(folder)/'lod.json'
    import json
    path.write_text(json.dumps(spec))
    loaded = lod.load(path)

# Texture variants: the far shape fills texels the near shape must leave transparent.
numbers, texels = lod.variants(loaded)
assert numbers == {100: [0, 1, 0]} and set(texels) == {(100, 0), (100, 1)}
assert texels[100, 0] == sorted(tuple(t) for t in near['fill'])
assert texels[100, 1] == sorted({tuple(t) for t in far['fill']+far['gutter']})
base_fill, copies = lod.texture_fill(loaded)
assert set(base_fill) == {100} and set(copies) == {(100, 1)}
tagged = lod.variant_header(split.split_header(CUTOUT), 1)
assert lod.base_header(tagged) == (CUTOUT, 1, True) and lod.base_header(lod.variant_header(CUTOUT, 2)) == (CUTOUT, 2, False)
assert lod.base_header(split.split_header(CUTOUT)) == (CUTOUT, 0, True) and lod.base_header(CUTOUT) is None
assert lod.base_header((0x6012, 0x8000, 0, 39808)) is None   # the stipple header is nobody's variant

plain, _, plain_map = order.sort_pack(source, static_light=light, reduced_headers=[], stipple=True)
data, report, face_map = order.sort_pack(source, static_light=light, reduced_headers=[], stipple=True,
                                         lod=dict(spec=loaded, images=lambda header: pictures[header[2]]))
assert accepted(plain) in (None, True) and accepted(data) in (None, True)
h, out, records, headers, words, groups, mesh = tables(data)
assert not models.HEADER.unpack_from(plain)[15] & lod.PACK_LOD_GROUPS
assert h[15] & 0xffff == order.BAKED_LIGHT | order.SCENERY_CLASSES | lod.PACK_LOD_GROUPS and h[15] >> 16 == 3 == len(groups)
assert mesh[15] == 3 << 16 and report['lod_groups']['groups'] == 3
origin = face_map['new_to_old']
# The faces outside every group come first, in their usual order; then the groups.
first_group = min(g[0] for g in groups)
assert sorted(origin[:first_group]) == [0, 2] and all(origin[f] in (1, 3, 4) for f in range(first_group, len(out)))
at = first_group
stipple = order.stipple_header(CHECKER)
split_near, split_far = split.split_header(CUTOUT), lod.variant_header(split.split_header(CUTOUT), 1)
assert split_near in headers and split_far in headers and stipple in headers
for word, header in zip(words, headers):
    if header in (split_near, split_far):
        assert word == order.STAGE_CUTOUT | order.SCENERY_POINT
seen = {}
for first, c0, c1, c2, levels, spare, cx, cy, cz, d0, d1 in groups:
    assert first == at and spare == 0      # consecutive, in face order
    counts = (c0, c1, c2)
    source_face = origin[first]
    assert all(origin[f] == source_face for f in range(first, first+sum(counts)))
    corners = [vertices[i] for i in faces[source_face][0]]
    assert (cx, cy, cz) == tuple(struct.unpack('<3f', struct.pack('<3f', *[sum(p[a] for p in corners)/4 for a in range(3)])))
    ranges = [list(range(first+sum(counts[:n]), first+sum(counts[:n+1]))) for n in range(3)]
    if source_face == 4:
        # The fence: the stipple panel near, nothing far.
        assert levels == 2 and counts == (1, 0, 0) and headers[out[first][4]] == stipple and d1 == 0
        radius = max(sum((p[a]-c)**2 for a, c in zip(range(3), (cx, cy, cz)))**0.5 for p in corners)
        assert abs(d0-(280*1.25/order.STIPPLE_TEXELS_PER_UNIT+radius*280/210)) < 1e-4
    else:
        # A tree: face and cores, then the outline with the texture copy, then nothing.
        assert levels == 3 and c0 >= 2 and c1 >= 1 and c2 == 0
        height = max(p[1] for p in corners)-min(p[1] for p in corners)
        assert abs(d0-height*280*1.25/60) < 1e-4 and d1 == 500.0
        kinds = [sorted({headers[out[f][4]] for f in level}) for level in ranges]
        assert kinds[0] == sorted([CUTOUT, split_near]) and kinds[1] == [split_far] and kinds[2] == []
        whole = [f for f in ranges[0] if headers[out[f][4]] == CUTOUT]
        assert len(whole) == 1
        # Layers. Near: every core above the punch-through face. The far shape is not
        # lifted by the near one: it lies where the face lay (its trunk rectangle
        # overlaps its body, one layer between them as in a plain outline pack).
        assert all(out[f][12] == out[whole[0]][12]+1 for f in ranges[0] if f != whole[0])
        assert min(out[f][12] for f in ranges[1]) == out[whole[0]][12]
        assert max(out[f][12] for f in ranges[1]) <= out[whole[0]][12]+1
        seen[source_face] = (out[whole[0]][12], max(out[f][12] for f in ranges[0]+ranges[1]))
        # Every piece keeps the source's normal, baked lumas, sides and face group.
        before = next(face for f, face in enumerate(tables(plain)[1]) if plain_map['new_to_old'][f] == source_face)
        for f in ranges[0]+ranges[1]:
            assert out[f][8:12] == before[8:12] and out[f][6] == before[6] and out[f][13] == before[13]
            assert out[f][7] & ~3 == before[7] & ~3
    at += sum(counts)
assert at == len(out) and set(seen) == {1, 3}
# The first tree stands on the board and carries the sign: each shape above the
# board, the sign above every shape. The second tree lies on nothing.
board = next(out[f][12] for f in range(len(out)) if origin[f] == 0)
sign = next(out[f][12] for f in range(len(out)) if origin[f] == 2)
assert board == 0 and seen[1][0] == 1 and sign > seen[1][1] and seen[3][0] == 0
assert [entry['origin'] for entry in face_map['lod']] == [origin[g[0]] for g in groups]

# What the reader must refuse: every rule of the table, one at a time.
table_at = len(data)-3*lod.LOD.size
tree = next(i for i, g in enumerate(groups) if g[4] == 3)


def changed(group, field, value):
    row = list(groups[group])
    row[field] = value
    out_bytes = bytearray(data)
    lod.LOD.pack_into(out_bytes, table_at+group*lod.LOD.size, *row)
    return bytes(out_bytes)


def header_changed(index, value, at=0, layout=models.HEADER):
    out_bytes = bytearray(data)
    row = list(layout.unpack_from(out_bytes, at))
    row[index] = value
    layout.pack_into(out_bytes, at, *row)
    return bytes(out_bytes)


if reader:
    bad = {
        'no level': changed(tree, 4, 0), 'four levels': changed(tree, 4, 4), 'spare byte': changed(tree, 5, 1),
        'faces in an absent level': changed(next(i for i, g in enumerate(groups) if g[4] == 2), 3, 1),
        'switch depths decrease': changed(tree, 10, groups[tree][9]/2),
        'a used switch depth of zero': changed(tree, 9, 0.0),
        'a depth for an absent level': changed(next(i for i, g in enumerate(groups) if g[4] == 2), 10, 5.0),
        'a negative switch depth': changed(tree, 9, -1.0),
        'centre outside the mesh': changed(tree, 6, 1e6),
        'a range into the next group': changed(0, 1, groups[0][1]+1),
        'a group behind the mesh': changed(2, 0, len(out)),
        'a group that starts inside the one before': changed(1, 0, groups[1][0]-1),
        'the flag without a count': header_changed(15, h[15] & 0xffff),
        'a count without the flag': header_changed(15, h[15] & ~lod.PACK_LOD_GROUPS),
        'fewer records than the meshes claim': header_changed(15, (h[15] & 0xffff) | 2 << 16),
        'a mesh that claims no group': header_changed(15, 0, models.HEADER.size, models.MESH),
        'a mesh with another first record': header_changed(15, 3 << 16 | 1, models.HEADER.size, models.MESH),
        'a truncated table': data[:-lod.LOD.size],
    }
    for name, candidate in bad.items():
        if candidate is not None:
            assert accepted(candidate) is False, name
    # The plain pack keeps the mesh word zero.
    spoiled = bytearray(plain)
    row = list(models.MESH.unpack_from(spoiled, models.HEADER.size))
    row[15] = 1 << 16
    models.MESH.pack_into(spoiled, models.HEADER.size, *row)
    assert accepted(bytes(spoiled)) is False

# A three-level fence: the checkerboard near, a sparser stipple in between, nothing far.
assert lod.stipple_cell('quarter') == ('X.', '..') and lod.stipple_cell('eighth') == ('X...', '....', '..X.', '....')
for name, share in (('quarter', 4), ('eighth', 8)):
    sparse = lod.stipple_image(lod.stipple_cell(name), 128, 9)
    assert sum(row.count(9) for row in sparse)*share == 128*128 and {v for row in sparse for v in row} == {9, 15}
    # Every opaque texel is one of the checkerboard's (x+y even): the net thins in place.
    assert all((x+y) & 1 == 0 for y, row in enumerate(sparse) for x, v in enumerate(row) if v == 9)
    # Neither rows nor columns nor diagonals of the cell are filled: a net, not stripes.
    cell = lod.stipple_cell(name)
    assert all(row.count('X') <= 1 for row in cell) and all(col.count('X') <= 1 for col in zip(*cell))
    assert sum(cell[i][i] == 'X' for i in range(len(cell))) < len(cell)
assert lod.stipple_image(('X...', '....', '..X.', '....'), 2, 9) == [[15, 15], [15, 15]]   # a mip level too small for the cell
assert lod.stipple_base((0x6012, 0x8100, 0, 39808)) == ((0x6012, 0x8000, 0, 39808), 1)
assert lod.stipple_base((0x6012, 0x8000, 0, 39808)) is None and lod.stipple_base(lod.variant_header(CUTOUT, 1)) is None
assert lod.base_header((0x6012, 0x8100, 0, 39808)) is None
fence3 = dict(reference_focus=280, expected_focus=210, rules=[
    dict(name='fence', checker=True, levels=['source', dict(stipple='quarter'), 'none'],
         switch=[dict(texel_pixels=0.5, reach='radius'), dict(texel_pixels=0.2, reach='radius')])])
with tempfile.TemporaryDirectory() as folder:
    path = Path(folder)/'fence3.json'
    path.write_text(json.dumps(fence3))
    loaded3 = lod.load(path)
assert lod.stipple_variants(loaded3) == ({0: [0, 1, 0]}, {1: ('X.', '..')}) and lod.texture_fill(loaded3) == ({}, {})
assert lod.stipple_variants(loaded) == ({1: [0, 0]}, {})
data3, report3, map3 = order.sort_pack(source, static_light=light, reduced_headers=[], stipple=True,
                                       lod=dict(spec=loaded3, images=lambda header: pictures[header[2]]))
assert accepted(data3) in (None, True)
h3, out3, records3, headers3, words3, groups3, mesh3 = tables(data3)
sparse_header = (stipple[0], stipple[1] | 1 << lod.VARIANT_SHIFT, stipple[2], stipple[3])
assert order.stipple_header(lod.variant_header(CHECKER, 1)) == sparse_header and lod.stipple_base(sparse_header) == (stipple, 1)
assert stipple in headers3 and sparse_header in headers3 and len(headers3) == 5 and len(out3) == len(faces)+1
assert report3['lod_groups']['stipple_variants'] == {'fence': [0, 1, 0]} and report3['stipple_panels']['panels'] == 1
assert report3['lod_groups']['rules'][0]['levels'][1]['stipple_coverage'] == 0.25
(first, c0, c1, c2, levels, spare, cx, cy, cz, d0, d1), = groups3
assert (c0, c1, c2, levels) == (1, 1, 0, 3) and map3['new_to_old'][first] == map3['new_to_old'][first+1] == 4
assert headers3[out3[first][4]] == stipple and headers3[out3[first+1][4]] == sparse_header
# The same quad: corners (hence texture coordinates), normal, lumas, sides, layer and face group.
assert out3[first][:4] == out3[first+1][:4] and out3[first][5:] == out3[first+1][5:]
assert words3[out3[first][4]] == words3[out3[first+1][4]] == order.STAGE_CHECKER
radius = max(sum((vertices[i][a]-c)**2 for a, c in zip(range(3), (cx, cy, cz)))**0.5 for i in faces[4][0])
assert abs(d0-(280*1.25/(order.STIPPLE_TEXELS_PER_UNIT*0.5)+radius*280/210)) < 1e-4
assert abs(d1-(280*1.25/(order.STIPPLE_TEXELS_PER_UNIT*0.2)+radius*280/210)) < 1e-4
for wrong in (dict(stipple='half'), dict(stipple=['X.', '.X', '..']), dict(stipple=['XX', '..']), dict(stipple=['.X', '..']),
              dict(stipple=['..', '..']), dict(stipple=['X..', '...', '...']), dict(stipple='quarter', spec={})):
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder)/'wrong.json'
        path.write_text(json.dumps(dict(rules=[dict(checker=True, levels=['source', wrong], switch=[dict(depth=5.0)])])))
        try:
            lod.load(path)
            raise SystemExit(f'accepted {wrong}')
        except ValueError:
            pass
with tempfile.TemporaryDirectory() as folder:   # a stipple level is for the checker panels only
    path = Path(folder)/'wrong.json'
    path.write_text(json.dumps(dict(rules=[dict(textures=[100], levels=['source', dict(stipple='quarter')], switch=[dict(depth=5.0)])])))
    try:
        lod.load(path)
        raise SystemExit('accepted a stipple level for a texture')
    except ValueError:
        pass

# The aligned layout carries the table too.
aligned = models.align_tables(data)
assert accepted(aligned) in (None, True)
count = models.HEADER.unpack_from(aligned)[15] >> 16
start = len(aligned)-count*lod.LOD.size
assert start % 32 == 0 and aligned[start:] == data[table_at:]

# A specification the tool must refuse.
for wrong in (dict(rules=[dict(textures=[100], levels=['source', 'none'], switch=[])]),
              dict(rules=[dict(textures=[100], checker=True, levels=['source'], switch=[])]),
              dict(rules=[dict(textures=[100], levels=['source', 'none', 'none', 'none'], switch=[{}, {}, {}])]),
              dict(reference_focus=300, rules=[dict(textures=[100], levels=['source'], switch=[])]),
              dict(rules=[dict(textures=[100], levels=[dict(spec=dict(textures={'100': near}, merge=[100]))], switch=[])])):
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder)/'wrong.json'
        path.write_text(json.dumps(wrong))
        try:
            lod.load(path)
            raise SystemExit(f'accepted {wrong}')
        except ValueError:
            pass
try:
    order.sort_pack(source, static_light=light, reduced_headers=[], stipple=True,
                    lod=dict(spec=loaded, images=lambda header: pictures[header[2]]),
                    cores=dict(spec=dict(textures={'100': near}), images=lambda header: pictures[header[2]]))
    raise SystemExit('level of detail and opaque cores were applied together')
except ValueError:
    pass

import math
# Super-sampled far levels ("supersample" rules): road faces, a stripe on one of them, a patch the arcade drew
# before the second road face (which therefore lies above it), a wall, and a verge texture on the ground and
# on an upright face (road_up keeps the ground face only). Road up is -y.
ROAD, STRIPE, PATCH, VERGE = (0x4009, 0, 300, 41216), (0x4009, 0, 301, 41216), (0x4009, 0, 303, 41216), (0x4009, 0, 302, 41216)
up = (0.0, -1.0, 0.0)
ground = [(0, 0, 0, 0, 0), (8, 0, 0, 512, 0), (8, 0, 8, 512, 1024), (0, 0, 8, 0, 1024),          # road A: 64 x 128 texels
          (2, 0, 2, 0, 0), (4, 0, 2, 256, 0), (4, 0, 6, 256, 512), (2, 0, 6, 0, 512),             # a stripe on road A
          (0, 0, 9, 0, 0), (8, 0, 9, 512, 0), (8, 0, 15, 512, 512), (0, 0, 15, 0, 512),           # a patch under road B
          (0, 0, 8, 0, 0), (8, 0, 8, 512, 0), (8, 0, 16, 512, 1024), (0, 0, 16, 0, 1024),         # road B
          (-4, 0, 0, 0, 0), (0, 0, 0, 256, 0), (0, 0, 8, 256, 512), (-4, 0, 8, 0, 512),           # verge on the ground
          (-4, 0, 0, 0, 0), (-4, -2, 0, 0, 256), (-4, -2, 8, 512, 256), (-4, 0, 8, 512, 0)]       # verge texture on a wall
road_faces = [((0, 1, 2, 3), 0, 0x1a01, up), ((4, 5, 6, 7), 1, 0x1a01, up), ((8, 9, 10, 11), 2, 0x1a01, up),
              ((12, 13, 14, 15), 0, 0x1a01, up), ((16, 17, 18, 19), 3, 0x1a01, up), ((20, 21, 22, 23), 3, 0x1a01, (1.0, 0.0, 0.0))]
road_source = pack(road_faces, ground, [ROAD, STRIPE, PATCH, VERGE])
assert lod.supersample_header(ROAD) == (0x4009, lod.SUPERSAMPLE_MARK, 300, 41216)
assert lod.supersample_base(lod.supersample_header(ROAD)) == ROAD and lod.supersample_base(ROAD) is None
assert lod.base_header(lod.supersample_header(ROAD)) is None and lod.stipple_base(lod.supersample_header(ROAD)) is None
assert lod.supersample_base(lod.variant_header(CUTOUT, 1)) is None and lod.supersample_base((0x6012, 0x8000, 0, 39808)) is None
assert abs(lod.texel_length([(0, 0, 0), (8, 0, 0), (8, 0, 8), (0, 0, 8)], [(0, 0), (512, 0), (512, 1024), (0, 1024)])-0.0625) < 1e-9
for wrong in ((0x0009, 0, 300, 0), (0x4009, 0x2000, 300, 0)):     # untextured, or already marked
    try:
        lod.supersample_header(wrong)
        raise SystemExit(f'super-sampled {wrong}')
    except ValueError:
        pass


def road_spec(group):
    switch = [dict(grazing_texel_pixels=1.0, camera_height=2.0)]
    return dict(reference_focus=280, expected_focus=210, rules=[
        dict(name='road', textures=[300, 301], supersample=dict(group=group), levels=['source', 'supersample'], switch=switch),
        dict(name='verges', textures=[302], supersample=dict(road_up=True), levels=['source', 'supersample'], switch=switch)])


def loaded_road(group):
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder)/'road.json'
        path.write_text(json.dumps(road_spec(group)))
        return lod.load(path)


road_plain, _, road_plain_map = order.sort_pack(road_source, static_light=light, reduced_headers=[])
plain_out, plain_records, plain_words = tables(road_plain)[1], tables(road_plain)[2], tables(road_plain)[4]
plain_layer = {road_plain_map['new_to_old'][f]: face[12] for f, face in enumerate(plain_out)}
assert plain_layer[1] == plain_layer[0]+1 and plain_layer[3] == plain_layer[2]+1    # stripe over road A, road B over the patch
for group, expected_groups in (('face', 4), (dict(run=32, radius=1000.0), 3)):
    spec_road = loaded_road(group)
    assert lod.variants(spec_road) == ({}, {}) and lod.texture_fill(spec_road) == ({}, {})
    data_road, report_road, map_road = order.sort_pack(road_source, static_light=light, reduced_headers=[],
                                                      lod=dict(spec=spec_road, images=lambda header: None))
    assert accepted(data_road) in (None, True)
    h_r, out_r, records_r, headers_r, words_r, groups_r, mesh_r = tables(data_road)
    assert len(groups_r) == expected_groups and report_road['lod_groups']['groups'] == expected_groups
    origin_r = map_road['new_to_old']
    # Faces 0..4 twice (level 0 plain, level 1 marked), the upright verge face once.
    assert sorted(origin_r) == [0, 0, 1, 1, 2, 3, 3, 4, 4, 5]
    for f, face in enumerate(out_r):
        source_face = origin_r[f]
        before = next(p for g, p in enumerate(plain_out) if road_plain_map['new_to_old'][g] == source_face)
        header = headers_r[face[4]]
        plain_header = lod.supersample_base(header) or header
        # The same face: corners, lumas, sides, normal, face group, layer; header up to the mark; the class of the plain material.
        assert [records_r[i] for i in face[:face[5]]] == [plain_records[i] for i in before[:before[5]]]
        assert face[5:] == before[5:] and plain_header == tables(road_plain)[3][before[4]]
        assert words_r[face[4]] == plain_words[before[4]]
    marked = [f for f, face in enumerate(out_r) if lod.supersample_base(headers_r[face[4]])]
    assert sorted(origin_r[f] for f in marked) == [0, 1, 3, 4]
    at = 0
    for first, c0, c1, c2, levels, spare, cx, cy, cz, d0, d1 in groups_r:
        assert levels == 2 and c0 == c1 and c2 == 0 and d1 == 0.0
        assert all(not lod.supersample_base(headers_r[out_r[f][4]]) for f in range(first, first+c0))
        assert all(lod.supersample_base(headers_r[out_r[f][4]]) for f in range(first+c0, first+2*c0))
        members = [origin_r[f] for f in range(first, first+c0)]
        assert [origin_r[f] for f in range(first+c0, first+2*c0)] == members
        texel = min(lod.texel_length([ground[i][:3] for i in road_faces[m][0]], [ground[i][3:] for i in road_faces[m][0]]) for m in members)
        assert abs(d0-math.sqrt(210*1.25*2.0*texel)*280/210) < 1e-4
        corners = [ground[i] for m in members for i in road_faces[m][0]]
        assert abs(cx-sum(p[0] for p in corners)/len(corners)) < 1e-5 and abs(cz-sum(p[2] for p in corners)/len(corners)) < 1e-5
    if group != 'face':
        assert sorted(sorted(origin_r[f] for f in range(g[0], g[0]+g[1])) for g in groups_r) == [[0, 3], [1], [4]]

# Two source materials with one header and different lighting (the casino banner's boards, 2064/2070/3594 in the
# world): each far level keeps its own source's diffuse and ambient, so the baked lumas do not change at the switch.
shared_faces = [((0, 1, 2, 3), 0, 0x1a01, up), ((12, 13, 14, 15), 1, 0x1a01, up)]
shared_source = pack(shared_faces, ground, [ROAD, ROAD], lights=[(100.0, 60.0), (20.0, 10.0)])
data_shared, _, map_shared = order.sort_pack(shared_source, static_light=light, reduced_headers=[],
                                             lod=dict(spec=loaded_road('face'), images=lambda header: None))
h_s, out_s, records_s, headers_s, _, groups_s, _ = tables(data_shared)
off_s = models.table_offsets(h_s)
lights_s = [models.MATERIAL.unpack_from(data_shared, off_s[3]+m*models.MATERIAL.size)[4:6] for m in range(h_s[5])]
assert len(groups_s) == 2 and h_s[5] == 4                # two plain materials, two super-sampled ones
for first, c0, c1, *_ in groups_s:
    near_face, far_face = out_s[first], out_s[first+c0]
    assert lod.supersample_base(headers_s[far_face[4]]) == headers_s[near_face[4]] == ROAD
    assert lights_s[far_face[4]] == lights_s[near_face[4]]
    assert [records_s[i] for i in far_face[:far_face[5]]] == [records_s[i] for i in near_face[:near_face[5]]]
assert sorted(lights_s[out_s[g[0]+g[1]][4]] for g in groups_s) == [(20.0, 10.0), (100.0, 60.0)]
# Super-sampled by instance ("group": "instance"): a world object's mesh (a PLACED instance, the slot machine's kind) of
# two faces around its model origin keeps its faces and materials; the rule's materials carry its switch depth in
# material word padding[1] (meshpack.h, nm_material_supersample_depth): 64 texels over 2 units, one texel a third of a
# pixel at focus 280 (280*1.25*3/32), plus the largest corner distance from the origin times 280/210.
REEL_A, REEL_B, OTHER = (0x4009, 0, 500, 41216), (0x6009, 0, 501, 41216), (0x4009, 0, 502, 41216)
reel_word = 0x80000000 | 11 << 12 | 0x200 | 151        # NM_OBJECT_INSTANCE, kind SLOT, PLACED, cell 151
reel_vertices = [(-1, -1, 0, 0, 0), (1, -1, 0, 512, 0), (1, 1, 0, 512, 512), (-1, 1, 0, 0, 512),
                 (-1, -1, -0.1, 0, 0), (1, -1, -0.1, 512, 0), (1, 1, -0.1, 512, 512), (-1, 1, -0.1, 0, 512)]
reel_faces = [((0, 1, 2, 3), 0, 0x1a01, (0.0, 0.0, -1.0)), ((4, 5, 6, 7), 1, 0x1a01, (0.0, 0.0, -1.0))]
reel_rule = dict(name='reels', textures=[500, 501], supersample=dict(group='instance'), levels=['source', 'supersample'],
                 switch=[dict(texel_pixels=1/3, reach='radius')])


def loaded_rules(*rules):
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder)/'reels.json'
        path.write_text(json.dumps(dict(reference_focus=280, expected_focus=210, rules=list(rules))))
        return lod.load(path)


reel_source = pack(reel_faces, reel_vertices, [REEL_A, REEL_B], word=reel_word)
reel_plain, _, _ = order.sort_pack(reel_source, static_light=light, reduced_headers=[])
reel_data, reel_report, _ = order.sort_pack(reel_source, static_light=light, reduced_headers=[],
                                            lod=dict(spec=loaded_rules(reel_rule), images=lambda header: None))
assert accepted(reel_data) in (None, True)
h_e, out_e, _, headers_e, words_e, groups_e, _ = tables(reel_data)
off_e = models.table_offsets(h_e)
depth_words = [struct.unpack_from('<f', reel_data, off_e[3]+m*models.MATERIAL.size+28)[0] for m in range(h_e[5])]
expected_depth = 280*1.25*3/32+math.sqrt(2.0)*280/210
assert not h_e[15] & lod.PACK_LOD_GROUPS and not groups_e and h_e[4] == 2 and headers_e == [REEL_A, REEL_B]
assert all(abs(d-expected_depth) < 1e-4 for d in depth_words) and words_e == [order.SCENERY_STAGE_OBJECTS]*2
assert reel_report['lod_groups']['rules'][0]['by_instance']['switch_depth_at_focus_280'] == depth_words[0]
# Byte for byte the pack without the rule but for the two depth words.
assert len(reel_plain) == len(reel_data) and sum(a != b for a, b in zip(reel_plain, reel_data)) <= 8
for wrong, why in ((pack(reel_faces, reel_vertices, [REEL_A, REEL_B], word=reel_word & ~0x200), 'not drawn by placed'),
                   (pack(reel_faces+[((0, 1, 2, 3), 2, 0x1a01, (0.0, 0.0, -1.0))], reel_vertices, [REEL_A, REEL_B, OTHER],
                         word=reel_word), 'faces outside the rule')):
    try:
        order.sort_pack(wrong, static_light=light, reduced_headers=[], lod=dict(spec=loaded_rules(reel_rule), images=lambda header: None))
        raise SystemExit(f'accepted a rule by instance with a mesh {why}')
    except ValueError as error:
        assert why in str(error), error
for options in (dict(group='instance', road_up=True),):
    try:
        loaded_rules(dict(reel_rule, supersample=options))
        raise SystemExit('accepted road_up by instance')
    except ValueError:
        pass
# The reader: one depth a mesh, a positive one, on an object material only.
if reader:
    for at, value in ((0, expected_depth+1), (1, -1.0), (0, float('nan'))):
        bad = bytearray(reel_data)
        struct.pack_into('<f', bad, off_e[3]+at*models.MATERIAL.size+28, value)
        assert accepted(bytes(bad)) is False, (at, value)

print('PASS: level-of-detail groups: ranges, switch depths from size and texel density, layers per shape, texture variants, '
      'sparser stipple levels of the checker panels, super-sampled far levels (per face and per run, layers in source order, '
      'the lighting of each source material; by instance: a switch depth on a world object\'s materials), '
      'aligned layout, reader acceptance and malformed tables')
