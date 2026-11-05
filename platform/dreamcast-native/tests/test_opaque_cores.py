"""ROM-free checks of opaque cores (tools/opaque_cores.py, prepare_face_order.py --opaque-cores).

python3 platform/dreamcast-native/tests/test_opaque_cores.py [build/check_native_meshpack]
"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

try:
    import numpy as np
except ImportError:      # The tool needs numpy, as prepare_palette_materials.py does.
    print('SKIP: numpy is not installed for this interpreter')
    sys.exit(77)

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import prepare_models as models
import prepare_face_order as order
import opaque_cores as cores
import split_cutouts as split

reader = sys.argv[1] if len(sys.argv) > 1 else None

# A 64x64 texture: an opaque triangle (a tree on its side) with a one-texel
# hole and a notch one texel wide, a thin trunk, everything else entry 15.
image = np.full((64, 64), 15, np.uint8)
for y in range(64):
    for x in range(48):
        if abs(y-32) <= x//2:
            image[y, x] = 1+(x+y) % 14
image[30:34, 48:64] = 3
image[32, 20] = 15
image[20:25, 40] = 15


def touched(polygon, shape):
    """Texels with one of 64 interior sample points inside the polygon (1/8 texel units)."""
    mask = np.zeros(shape, bool)
    ys, xs = np.mgrid[0:shape[0], 0:shape[1]]
    for ox in range(8):
        for oy in range(8):
            px, py = 8*xs+ox+0.5, 8*ys+oy+0.5
            inside = np.ones(shape, bool)
            for a, b in zip(polygon, polygon[1:]+polygon[:1]):
                inside &= (b[0]-a[0])*(py-a[1])-(b[1]-a[1])*(px-a[0]) > 0
            mask |= inside
    return mask


def safe_of(picture):
    opaque = np.pad(picture != 15, 1)
    out = np.ones(picture.shape, bool)
    for dy in range(3):
        for dx in range(3):
            out &= opaque[dy:dy+picture.shape[0], dx:dx+picture.shape[1]]
    return out


# Overlay: the hole and the notch are filled with a neighbour's entry, the
# cores touch only texels whose 8 neighbours are opaque in the filled texture.
overlay = cores.design_overlay(image, hole=8, close=1, cores=2, vertices=5)
fill = {(x, y): value for x, y, value in overlay['fill']}
assert (20, 32) in fill and (40, 22) in fill and all(image[y, x] == 15 and 0 <= v < 15 for (x, y), v in fill.items())
assert len(fill) < 40 and overlay['polygons'] and not overlay['bands']
variant = cores.filled(image, overlay['fill'])
for polygon in overlay['polygons']:
    polygon = [tuple(p) for p in polygon]
    assert split._area2(polygon) > 0 and not (touched(polygon, image.shape) & ~safe_of(variant)).any()
assert sum(split._area2([tuple(p) for p in polygon]) for polygon in overlay['polygons']) > 0.5*128*(image != 15).sum()
# A hole larger than the limit, and a texture without closing, stay as they are.
assert (20, 32) not in {(x, y) for x, y, _ in cores.design_overlay(image, hole=0, close=0)['fill']}
# Dark fill takes the darkest neighbour; an inverted ramp is dark at the high entries.
assert fill[20, 32] == min(int(image[y, x]) for y in (31, 32, 33) for x in (19, 20, 21) if image[y, x] != 15)
bright = {(x, y): v for x, y, v in cores.design_overlay(image, hole=8, close=0, inverted=True)['fill']}
assert bright[20, 32] == max(int(image[y, x]) for y in (31, 32, 33) for x in (19, 20, 21) if image[y, x] != 15)

# Outline: one polygon for the body, a rectangle for the trunk; every texel a
# polygon touches, and the ring around them, is opaque afterwards.
outline = cores.design_outline(image, vertices=4, thin=3, least=20)
assert len(outline['polygons']) == 2 and outline['report']['lost_texels'] < 0.1*outline['report']['opaque_texels']
variant = cores.filled(image, outline['fill']+outline['gutter'])
for polygon in outline['polygons']:
    polygon = [tuple(p) for p in polygon]
    area = touched(polygon, image.shape)
    ring = np.zeros_like(area)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            ring |= np.roll(np.roll(area, dy, 0), dx, 1)
    assert not (ring & (variant == 15)).any()
assert all(image[y, x] == 15 and 0 <= v < 15 for x, y, v in outline['fill']+outline['gutter'])

# Rectangles: three bars and a post are found exactly, nothing lost or filled.
bars = np.full((32, 64), 15, np.uint8)
for column in (8, 24, 40):
    bars[4:28, column:column+6] = 2
bars[10:12, 4:60] = 5
rects = cores.design_rects(bars, most=4, tolerance=0.0)
assert len(rects['polygons']) == 4 and rects['report']['lost_texels'] == 0 and rects['report']['filled_texels'] == 0
assert not rects['fill'] and rects['gutter']
assert len(cores.design_rects(bars, most=2, tolerance=0.0)['polygons']) == 2


def pack(faces, vertices, materials):
    """A DNM2 pack of one mesh. faces: (indices, material, attribute, normal)."""
    lows = [min(v[a] for v in vertices) for a in range(3)]
    highs = [max(v[a] for v in vertices) for a in range(3)]
    mesh = models.MESH.pack(0, len(vertices), 0, len(faces), *lows, *highs, 0, 0, 0, 0, 0, 0)
    body = b''.join(models.VERTEX.pack(*v) for v in vertices)
    body += b''.join(models.FACE.pack(*(list(i)+[0]*(4-len(i))), m, len(i), 0, a, *n, 0) for i, m, a, n in faces)
    body += b''.join(models.MATERIAL.pack(*h, 100.0, 60.0, 0.0, 0, 0, 0) for h in materials)
    body += models.INSTANCE.pack(0, 0x1000, 0, 0, *([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]), 1, 1, 0, 0, 1, 0, 0, 639, 479, 0, 0,
                                 0, 0, 0, 0)
    refs = [next(j for j in range(i+1) if vertices[j][:3] == v[:3]) for i, v in enumerate(vertices)]
    body += struct.pack(f'<{len(refs)}H', *refs)
    size = models.HEADER.size+len(mesh)+len(body)
    return models.HEADER.pack(0x324d4e44, size, 1, len(vertices), len(faces), len(materials), 1, 0, 0, 0, 0, 0, 0, 0, 0, 0)+mesh+body


def tables(data):
    h = models.HEADER.unpack_from(data)
    off = models.table_offsets(h)
    return (h, [order.FACE6.unpack_from(data, off[2]+f*order.FACE6.size) for f in range(h[4])],
            [models.VERTEX.unpack_from(data, off[1]+i*models.VERTEX.size) for i in range(h[3])],
            [struct.unpack_from('<4H', data, off[3]+m*models.MATERIAL.size) for m in range(h[5])],
            [struct.unpack_from('<I', data, off[3]+m*models.MATERIAL.size+24)[0] for m in range(h[5])])


CUTOUT, WALL, FENCE = (0x6009, 0, 100, 41216), (0x4009, 0, 200, 41216), (0x6001, 0, 300, 41216)
vertices = [(0, 0, 5, 0, 0), (4, 0, 5, 512, 0), (4, 4, 5, 512, 512), (0, 4, 5, 0, 512),            # the tree, two-sided
            (0, 0, 9, 0, 0), (4, 0, 9, 512, 0), (4, 4, 9, 512, 512), (0, 4, 9, 0, 512),            # a wall behind it
            (6, 0, 5, 512, 0), (10, 0, 5, 0, 0), (10, 4, 5, 0, 512), (6, 4, 5, 512, 512),          # a tree, UVs turned over
            # Two fence quads side by side: 64 texels of a 64-wide texture each, both starting at u = 0.
            (0, 0, 20, 0, 0), (2, 0, 20, 512, 0), (2, 1, 20, 512, 256), (0, 1, 20, 0, 256),
            (2, 0, 20, 0, 0), (4, 0, 20, 512, 0), (4, 1, 20, 512, 256), (2, 1, 20, 0, 256)]
two_sided = 0x1a01 | 1 << 17
source = pack([((0, 1, 2, 3), 0, two_sided, (0.0, 0.0, 1.0)), ((4, 5, 6, 7), 1, 0x1a01, (0.0, 0.0, 1.0)),
               ((8, 9, 10, 11), 0, two_sided, (0.0, 0.0, 1.0)), ((12, 13, 14, 15), 2, 0x1a01, (0.0, 0.0, 1.0)),
               ((16, 17, 18, 19), 2, 0x1a01, (0.0, 0.0, 1.0))], vertices, [CUTOUT, WALL, FENCE])
light = (-0.45, -0.89, -0.45)
pictures = {100: image, 300: bars}
plain, _, plain_map = order.sort_pack(source, static_light=light, reduced_headers=[])
_, plain_faces, plain_records, _, _ = tables(plain)
old = {plain_map['new_to_old'][f]: face for f, face in enumerate(plain_faces)}


def run(spec, **more):
    data, report, face_map = order.sort_pack(source, static_light=light, reduced_headers=[],
                                             cores=dict(spec=spec, images=lambda header: pictures[header[2]], **more))
    if reader:
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'cores.dnm'
            path.write_bytes(data)
            assert subprocess.run([reader, str(path)], capture_output=True).returncode == 0
    return data, report['opaque_cores'], face_map


def check_pieces(data, face_map, kept):
    """Pieces of the two trees: on the source polygon where its UV map puts
    them, source attributes, opaque material and class; returns per source
    the opaque area and the punch-through faces left."""
    h, faces, records, headers, words = tables(data)
    assert headers[:3] == [CUTOUT, WALL, FENCE] and headers[3] == split.split_header(CUTOUT)
    assert words[3] == order.STAGE_CUTOUT | order.SCENERY_POINT and h[15] == order.BAKED_LIGHT | order.SCENERY_CLASSES
    area, left, layers = {0: 0.0, 2: 0.0}, {0: [], 2: []}, {0: [], 2: []}
    for f, face in enumerate(faces):
        origin = face_map['new_to_old'][f]
        if origin not in area:
            continue
        before = old[origin]
        assert face[8:12] == before[8:12] and face[13] == before[13] and face[6] == before[6] and face[7] & ~3 == before[7] & ~3
        if face[4] == 0:
            left[origin].append(face)
            continue
        assert face[4] == 3 and face[7] & 3 == (1 if face[5] == 4 else 2)
        layers[origin].append(face[12])
        for i in range(1, face[5]-1):
            p = [records[face[k]] for k in (0, i, i+1)]
            for x, y, z, u, v in p:
                expect = (u/128 if origin == 0 else 10-u/128, v/128)
                assert z == 5 and abs(x-expect[0]) < 1e-5 and abs(y-expect[1]) < 1e-5, (x, y, u, v)
            winding = (p[1][0]-p[0][0])*(p[2][1]-p[0][1])-(p[1][1]-p[0][1])*(p[2][0]-p[0][0])
            assert winding > 0     # Both source quads run counter-clockwise in x, y.
            area[origin] += winding/2
    for origin in area:
        assert len(left[origin]) in kept and 0 < area[origin] < 16
        # A core lies above the punch-through face it was cut from.
        assert all(layer > face[12] for layer in layers[origin] for face in left[origin])
    assert abs(area[0]-area[2]) < 1e-4
    return area[0]


overlay['options'] = dict(kind='overlay')
done, report, face_map = run(dict(textures={'100': overlay}))
assert report['cores']['textures']['100']['faces_with_cores'] == 2
inner = check_pieces(done, face_map, (1,))
outline['options'] = dict(kind='outline')
done, report, face_map = run(dict(textures={'100': outline}))
assert check_pieces(done, face_map, (0,)) > inner
# Trimmed fringe: the punch-through face shrinks to its opaque texels, the cores stay.
overlay['options'] = dict(kind='overlay', fringe='hull', hull_cuts=2)
done, report, face_map = run(dict(textures={'100': overlay}))
h, faces, records, _, _ = tables(done)
assert abs(check_pieces(done, face_map, (1, 2, 3))-inner) < 1e-6
hull = [face for f, face in enumerate(faces) if face_map['new_to_old'][f] == 0 and face[4] == 0]
corner = {records[i][:2] for face in hull for i in face[:face[5]]}
assert max(x for x, _ in corner) == 4 and min(x for x, _ in corner) == 0 and (0.0, 0.0) not in corner
# A replaced texture may not keep a user (here: the reduced view's material is skipped).
try:
    run(dict(textures={'100': outline}), skip={CUTOUT})
    raise SystemExit('a replaced texture kept a punch-through user')
except ValueError:
    pass
# Bars: the fence faces become rectangles on the opaque list, nothing punch-through is left.
rects['options'] = dict(kind='rects')
done, report, face_map = run(dict(textures={'300': rects}))
h, faces, records, headers, _ = tables(done)
fence = [face for f, face in enumerate(faces) if face_map['new_to_old'][f] in (3, 4)]
assert len(fence) == 8 and all(headers[face[4]] == split.split_header(FENCE) and face[5] == 4 for face in fence)
# Per face three bars of 6x24 texels and the post of 56x2 that crosses them (the rectangles overlap there).
assert sum(abs(split._area2([records[i][3:5] for i in face[:4]]))//2 for face in fence) == 2*64*(3*6*24+56*2)
# Unwrap: the second fence quad takes u = 64..128, so both share the records of their common edge;
# merge: they become one quad over two periods. The texels sampled are the same.
done, report, face_map = run(dict(unwrap=[300]))
h, faces, records, _, _ = tables(done)
fence = [face for f, face in enumerate(faces) if face_map['new_to_old'][f] in (3, 4)]
assert report['unwrap']['edges_joined'] == 1 and len(fence) == 2 and len(set(fence[0][:4]) & set(fence[1][:4])) == 2
assert sorted({records[i][3] for face in fence for i in face[:4]}) == [0, 512, 1024]
done, report, face_map = run(dict(unwrap=[300], merge=[300]))
h, faces, records, _, _ = tables(done)
fence = [face for f, face in enumerate(faces) if face_map['new_to_old'][f] in (3, 4)]
assert report['merge']['faces_merged_away'] == 1 and len(fence) == 1 and face_map['merged'] == {'3': [4]}
assert sorted(records[i][:2]+records[i][3:] for i in fence[0][:4]) == [(0, 0, 0, 0), (0, 1, 0, 256), (4, 0, 1024, 0), (4, 1, 1024, 256)]
assert report['merge'].get('left_over_limit', 0) == 0
assert order.sort_pack(source, static_light=light, reduced_headers=[],
                       cores=dict(spec=dict(unwrap=[300], merge=[300], uv_limit=100),
                                  images=lambda header: bars))[1]['opaque_cores']['merge'].get('faces_merged_away', 0) == 0
# An empty specification changes nothing.
assert run(dict())[0] == plain
print('opaque cores: fill, cores, outline, rectangles, pack pieces, layers, unwrap and merge hold')

# Bands: body strip between the inner lines, a punch-through strip per side, end caps, the trunk.
bands = cores.design_bands(image, points=3, thin=3, least=20)
kinds = [quad['list'] for quad in bands['quads']]
assert kinds.count('O') == 2 and kinds.count('M') == 2*2+2 and len(bands['polygons']) == 1
assert bands['report']['lost_percent'] < 3 and all(image[y, x] == 15 and 0 <= v < 15 for x, y, v in bands['fill'])
variant = cores.filled(image, bands['fill'])
for quad in bands['quads']:
    corner = [tuple(p) for p in quad['corners']]
    # Strictly convex, so either diagonal draws the same quad.
    assert all(split._cross(corner[i-1], corner[i], corner[(i+1) % 4]) > 0 for i in range(4))
    if quad['list'] == 'O':
        assert not (touched(corner, image.shape) & ~safe_of(variant)).any()
body = [tuple(map(tuple, quad['corners'])) for quad in bands['quads'] if quad['list'] == 'O']
side = [tuple(map(tuple, quad['corners'])) for quad in bands['quads'] if quad['list'] == 'M']
# Consecutive quads of a strip share the edge c3-c0 / c1-c2; every inner-line edge of a side quad is a body edge.
assert {body[0][3], body[0][0]} == {body[1][1], body[1][2]}
edges = {frozenset((q[i], q[(i+1) % 4])) for q in body for i in range(4)}
assert sum(1 for q in side for i in range(4) if frozenset((q[i], q[(i+1) % 4])) in edges) == 2*2+2
bands['options'] = dict(kind='bands')
done, report, face_map = run(dict(textures={'100': bands}))
h, faces, records, headers, _ = tables(done)
for origin in (0, 2):
    mine = [face for f, face in enumerate(faces) if face_map['new_to_old'][f] == origin]
    # Two body quads and the trunk; six punch-through quads.
    assert sum(1 for face in mine if face[4] == 3) == 3 and sum(1 for face in mine if face[4] == 0) == 6
    # Opaque pieces lie a layer above the punch-through ones (the trunk crosses the end cap).
    assert min(face[12] for face in mine if face[4] == 3) > max(face[12] for face in mine if face[4] == 0)
check_pieces(done, face_map, (6,))
strips = order.sort_pack(source, static_light=light, reduced_headers=[], strip_scope='material',
                         cores=dict(spec=dict(textures={'100': bands}), images=lambda header: pictures[header[2]]))[1]
assert strips['strips'] >= 4 and strips['longest_strip_faces'] >= 2
print('opaque cores: bands hold')

# Cone: the body starts at one apex vertex and touches no texel that is transparent or next to one
# (only the enclosed pinhole is filled); the bands start at one outer apex vertex shared by both sides.
cone = cores.design_cone(image, points=2, thin=3, least=20)
assert cone['kind'] == 'bands' and cone['report']['body_transparent_texels'] == 0
filled_texels = {(x, y) for x, y, _ in cone['fill']}
# Both holes of the test image are enclosed (a pinhole and a slit inside the crown); the rest of the fill is the trunk's ring.
assert (20, 32) in filled_texels and (40, 22) in filled_texels and all(image[y, x] == 15 for x, y in filled_texels)
assert cone['report']['filled_pinhole_texels'] == 6
pinholes_only = cores.filled(image, [entry for entry in cone['fill'] if entry[0] in (20, 40)])
solid = [quad for quad in cone['quads'] if quad['list'] == 'O']
assert [len(quad['corners']) for quad in solid] == [3, 4]
for quad in solid:
    assert not (touched([tuple(p) for p in quad['corners']], image.shape) & ~safe_of(pinholes_only)).any()
side = [quad for quad in cone['quads'] if quad['list'] == 'M']
assert len(side) == 2*2+1 and all(len(quad['corners']) == 4 for quad in side)
apex, outer = tuple(solid[0]['corners'][1]), tuple(side[0]['corners'][2])
assert sum(1 for quad in side if apex in map(tuple, quad['corners']) and outer in map(tuple, quad['corners'])) == 2
for quad in cone['quads']:
    corner = [tuple(p) for p in quad['corners']]
    assert all(split._cross(corner[i-1], corner[i], corner[(i+1) % len(corner)]) > 0 for i in range(len(corner)))
assert len(cores.design_cone(image, points=2, skirt=False, thin=3, least=20)['quads']) == len(cone['quads'])-1
cone['options'] = dict(kind='cone')
done, report, face_map = run(dict(textures={'100': cone}))
h, faces, records, headers, _ = tables(done)
for origin in (0, 2):
    mine = [face for f, face in enumerate(faces) if face_map['new_to_old'][f] == origin]
    assert sorted(face[5] for face in mine if face[4] == 3) == [3, 4, 4] and sum(1 for face in mine if face[4] == 0) == 5
print('opaque cores: cone holds')

# Enclosing outline: every opaque crown texel inside (nothing lost), the ring round the body tiled exactly.
hull = cores.design_cone(image, points=2, thin=3, least=20, outline=6, curve=[5, 8])
assert hull['curve'] and all(row['lost_texels'] == 0 for row in hull['curve'])
assert hull['curve'][0]['band_texels'] >= hull['curve'][-1]['band_texels']
outline = [tuple(p) for p in hull['outline']]
body = [tuple(p) for quad in hull['quads'] if quad['list'] == 'O' for p in quad['corners']]
ring = [[tuple(p) for p in quad['corners']] for quad in hull['quads'] if quad['list'] == 'M']
assert all(split._cross(q[i-1], q[i], q[(i+1) % len(q)]) > 0 for q in ring for i in range(len(q)))
assert {p for q in ring for p in q} <= set(outline) | set(body)       # no new corner: no T-junction
inner = [tuple(p) for p in [hull['quads'][-2]['corners'][1], hull['quads'][-2]['corners'][2], hull['quads'][-1]['corners'][3],
                            hull['quads'][-1]['corners'][0], hull['quads'][-2]['corners'][0]]]
assert sum(split._area2(q) for q in ring) == split._area2(outline)-split._area2(inner)
crown_rows, crown_columns = np.nonzero(image[:, :48] != 15)
for y, x in zip(crown_rows, crown_columns):
    for corner in ((8*x, 8*y), (8*x+8, 8*y), (8*x+8, 8*y+8), (8*x, 8*y+8)):
        assert all(split._cross(outline[i-1], outline[i], corner) >= 0 for i in range(len(outline)))
print('opaque cores: enclosing outline holds')

# Strips: a ring between two squares is tiled exactly by their own corners, here as ONE strip; its faces
# chain in the packer whole, as given and with every face turned round (a face that maps the texture mirrored).
square, hole = [(0, 0), (64, 0), (64, 64), (0, 64)], [(16, 16), (48, 16), (48, 48), (16, 48)]
strips, triangles = cores.tile_strips(square, hole)
assert sum(split._cross(*t) for t in triangles) == split._area2(square)-split._area2(hole) and len(triangles) == 8
assert len(strips) == 1 and len(strips[0]) == 10 and {p for t in triangles for p in t} == set(square) | set(hole)
assert {frozenset(t) for t in cores._strip_check(strips[0], triangles)} == {frozenset(t) for t in triangles}
assert all(split._cross(*t) > 0 for t in cores._strip_check(strips[0], triangles))
faces = cores.strip_faces(strips[0])
assert sum(len(face)-2 for face in faces) == 8 and all(split._cross(f[i-1], f[i], f[(i+1) % len(f)]) > 0 for f in faces for i in range(len(f)))
ids = {}
as_given = {i: tuple(ids.setdefault(p, len(ids)) for p in face) for i, face in enumerate(faces)}
assert [len(chain[0]) for chain in order.chain_faces(as_given)] == [len(faces)]
mirror = cores.strips_of([(( -t[1][0], t[1][1]), (-t[0][0], t[0][1]), (-t[2][0], t[2][1])) for t in triangles])
turned = {i: tuple(ids.setdefault(p, len(ids)) for p in face) for i, face in enumerate(f for strip in mirror for f in cores.strip_faces(strip))}
assert len(order.chain_faces(turned)) == len(mirror) == 1
# A strip of five triangles: two quads and a last triangle; the odd triangle is the last face.
assert [len(f) for f in cores.strip_faces([(0, 8), (0, 0), (8, 8), (8, 0), (16, 8), (16, 0), (24, 8)])] == [4, 4, 3]
# A body cut by its face lies against the outline: still tiled exactly, the cut corners shared (no T-junction).
half = cores.tile_strips([(0, 0), (64, 0), (64, 32), (0, 32)], [(16, 16), (48, 16), (48, 32), (16, 32)])
assert sum(split._cross(*t) for t in half[1]) == 2*64*32-2*32*16 and (16, 32) in {p for t in half[1] for p in t}
# A concave outline (one reflex corner) round a body: exact, every triangle positive.
dented = [(0, 0), (32, 12), (64, 0), (64, 64), (0, 64)]
tiled = cores.tile_strips(dented, [(16, 24), (48, 24), (48, 48), (16, 48)])
assert sum(split._cross(*t) for t in tiled[1]) == split._area2(dented)-2*32*24 and all(split._cross(*t) > 0 for t in tiled[1])
print('opaque cores: strip tilings hold')

# Tree: body one strip under the hard rule, outline holds every crown texel with its margin (convex and
# free), nothing lost, trunk beside the body; faces chain into the reported strips; a face that shows half
# the texture gets its own exact tiling.
for concave in (False, True):
    tree = cores.design_tree(image, corners=5, outline=7, concave=concave, thin=3, least=20, hole=8, close=0)
    assert tree['report']['lost_texels'] == 0 and tree['report']['body_transparent_texels'] == 0
    outline = [tuple(p) for p in tree['tree']['outline']]
    assert len(outline) <= 7 and cores.encloses(outline, (image != 15) & ~cores._inside(
        [(p[0]/8, p[1]/8) for p in map(tuple, tree['polygons'][0])], *np.meshgrid(np.arange(64)+0.5, np.arange(64)+0.5)), 0.375) == 0
    body = [tuple(p) for p in tree['tree']['body']]
    ring = [[tuple(p) for p in q['corners']] for q in tree['quads'] if q['list'] == 'M']
    solid = [[tuple(p) for p in q['corners']] for q in tree['quads'] if q['list'] == 'O']
    assert all(split._cross(q[i-1], q[i], q[(i+1) % len(q)]) > 0 for q in ring+solid for i in range(len(q)))
    assert {p for q in ring for p in q} <= set(outline) | set(body) and {p for q in solid for p in q} == set(body)
    assert sum(split._area2(q) for q in ring)+sum(split._area2(q) for q in solid) == split._area2(outline)
    variant = cores.filled(image, tree['fill'])
    for q in solid:
        assert not (touched(q, image.shape) & (variant == 15)).any()
    assert min(p[0] for p in tree['polygons'][0]) >= max(p[0] for p in body)       # the trunk does not lie under the body
    ids = {}
    chains = order.chain_faces({i: tuple(ids.setdefault(p, len(ids)) for p in q) for i, q in enumerate(ring+solid)})
    assert len(chains) <= tree['report']['strips_per_plane']-1
    for footprint in ([(0, 0), (512, 0), (512, 256), (0, 256)], [(0, 256), (512, 256), (512, 512), (0, 512)], [(0, 0), (384, 0), (384, 512), (0, 512)]):
        for mirrored in (False, True):
            pieces = cores.tree_pieces(body, outline, footprint, mirrored)
            cut = cores._cut_to(outline, footprint, True)
            assert sum(split._area2(face) for face, _ in pieces) == split._area2(cut)
            assert all(0 <= p[0] <= 512 and footprint[0][1] <= p[1] <= footprint[2][1] and p[0] <= footprint[1][0] for face, _ in pieces for p in face)
    done, report, face_map = run(dict(textures={'100': dict(tree, options=dict(tree['options'], kind='tree'))}))
    h, faces, records, headers, _ = tables(done)
    for origin in (0, 2):
        mine = [face for f, face in enumerate(faces) if face_map['new_to_old'][f] == origin]
        assert sum(face[5]-2 for face in mine) == tree['report']['triangles_per_plane']
        assert len({face[12] for face in mine if face[4] == 3}) == 1                 # one layer for the opaque pieces
free = cores.enclosing_free(image[:, :48] != 15, 9)
assert free[0] and all(cores.encloses(poly, image[:, :48] != 15) == 0 for poly in free[0].values())
assert all(free[1][a] >= free[1][b] for a, b in zip(sorted(free[1]), sorted(free[1])[1:]))
print('opaque cores: tree strips hold')

# Far shape: apex on the tip, convex, a base edge across the trunk, the trunk from it; everything under it filled.
far = cores.design_far(image, corners=5, thin=3, least=20)
crown_far, trunk_far = [[tuple(p) for p in poly] for poly in far['polygons']]
assert crown_far[0] == (0, 8*32) and len(crown_far) == 5 and len(trunk_far) == 4
assert all(split._cross(crown_far[i-1], crown_far[i], crown_far[(i+1) % 5]) > 0 for i in range(5))
assert crown_far[2][0] == crown_far[3][0] == trunk_far[0][0] and far['report']['deviation_max_texels'] < 4
variant = cores.filled(image, far['fill']+far['gutter'])
assert not ((touched(crown_far, image.shape) | touched(trunk_far, image.shape)) & (variant == 15)).any()
assert cores.silhouette_deviation(image != 15, [[(0, 32), (48, 8), (48, 56)], [(48, 30), (64, 30), (64, 34), (48, 34)]])['max'] < 1.5
print('opaque cores: far shape holds')

# Wall with a ragged edge that repeats along v: nothing lost, the solid part obeys the rule, every face tiled exactly.
wall_image = np.full((64, 32), 15, np.uint8)
for y in range(64):
    wall_image[y, 8+abs((y % 32)-16)//2:] = 1+y % 14
wall_image[40, 24] = 15
wall = cores.design_wall(wall_image, outer=4, inner=4, hole=8)
assert wall['report']['lost_texels'] == 0 and wall['report']['filled_pinhole_texels'] == 1 and wall['wall']['period'] == 512
assert wall['wall']['inner'][0][0] == wall['wall']['inner'][-1][0] and wall['wall']['inner'][-1][1]-wall['wall']['inner'][0][1] == 512
filled_wall = cores.filled(wall_image, wall['fill'])
for footprint in ([(8, 0), (248, 0), (248, 512), (8, 512)], [(8, 100), (248, 100), (248, 900), (8, 900)], [(8, 37), (248, 37), (240, 301), (0, 301)]):
    for mirrored in (False, True):
        pieces = cores.wall_pieces(wall['wall']['inner'], wall['wall']['outer'], 512, footprint, mirrored)
        assert pieces and all(split._cross(f[i-1], f[i], f[(i+1) % len(f)]) > 0 for f, _ in pieces for i in range(len(f)))
        covered = np.zeros((2*64, 32), bool)
        tall = np.concatenate([filled_wall, filled_wall])
        for face, solid_piece in pieces:
            mask = touched(face, covered.shape)
            assert not solid_piece or not (mask & (tall == 15)).any()
        ys, xs = np.mgrid[0:128, 0:32]
        inside = np.ones(covered.shape, bool)
        for a, b in zip(footprint, footprint[1:]+footprint[:1]):
            inside &= (b[0]-a[0])*(8*ys+4-a[1])-(b[1]-a[1])*(8*xs+4-a[0]) > 0
        for face, _ in pieces:
            part = np.ones(covered.shape, bool)
            for a, b in zip(face, face[1:]+face[:1]):
                part &= (b[0]-a[0])*(8*ys+4-a[1])-(b[1]-a[1])*(8*xs+4-a[0]) >= 0
            covered |= part
        assert not (inside & (tall != 15) & ~covered).any()                           # no opaque texel centre lost
print('opaque cores: wall holds')
