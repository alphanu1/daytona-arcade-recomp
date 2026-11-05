"""ROM-free checks of split cutouts (tools/split_cutouts.py, prepare_face_order.py --split-cutouts).

python3 platform/dreamcast-native/tests/test_split_cutouts.py [build/check_native_meshpack]
"""
from fractions import Fraction
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
import split_cutouts as split

reader = sys.argv[1] if len(sys.argv) > 1 else None

# A 64x64 texture: an opaque triangle (a tree on its side) with a one-texel
# hole, a thin trunk, everything else the transparent entry 15.
image = np.full((64, 64), 15, np.uint8)
for y in range(64):
    for x in range(48):
        if abs(y-32) <= x//2:
            image[y, x] = (x+y) % 15
image[30:34, 48:64] = 3
image[32, 20] = 15


def samples(image, footprint, flip=0):
    """Sample points of the footprint (16 per texel; in quarters of the 1/8
    texel unit and off the lattice lines, so no lattice vertex or
    axis-aligned edge passes through one) with opaque and safe flags,
    computed without the module."""
    height, width = image.shape
    xs = [p[0] for p in footprint]
    ys = [p[1] for p in footprint]
    tx = np.arange(min(xs)//8-1, -(-max(xs)//8)+1)
    ty = np.arange(min(ys)//8-1, -(-max(ys)//8)+1)

    def wrap(values, size, mirror):
        values = values % (2*size)
        return np.where(values >= size, 2*size-1-values if mirror else values-size, values)

    def texels(dx, dy):
        return image[np.ix_(wrap(ty+dy, height, flip & 2), wrap(tx+dx, width, flip & 1))] != 15
    opaque = texels(0, 0)
    safe = np.ones_like(opaque)
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            safe &= texels(dx, dy)
    px = (32*tx[:, None]+8*np.arange(4)+3).reshape(-1)
    py = (32*ty[:, None]+8*np.arange(4)+5).reshape(-1)
    x, y = np.meshgrid(px, py)
    inside = inside_of([(4*p[0], 4*p[1]) for p in footprint], x, y, True)
    return x[inside], y[inside], np.repeat(np.repeat(opaque, 4, 0), 4, 1)[inside], np.repeat(np.repeat(safe, 4, 0), 4, 1)[inside]


def inside_of(polygon, x, y, strict):
    mask = np.ones(x.shape, bool)
    for a, b in zip(polygon, polygon[1:]+polygon[:1]):
        side = (b[0]-a[0])*(y-a[1])-(b[1]-a[1])*(x-a[0])
        mask &= side > 0 if strict else side >= 0
    return mask


def check_plan(image, footprint, plan, flip=0):
    lattice = [v[1] for v in plan['vertices']]
    triangles = []
    for kind, face in plan['faces']:
        assert len(face) in (3, 4)
        for i in range(1, len(face)-1):
            triangle = (lattice[face[0]], lattice[face[i]], lattice[face[i+1]])
            assert split._cross(*triangle) > 0
            triangles.append((kind, triangle))
    # No vertex of the plan inside an edge of a triangle (T-junction).
    for _, t in triangles:
        for a, b in zip(t, t[1:]+t[:1]):
            assert not any(split._between(a, b, r) for r in lattice)
    area = sum(split._cross(*t) for _, t in triangles)
    x, y, opaque, safe = samples(image, footprint, flip)
    count = np.zeros(x.shape, int)
    drawn = np.zeros(x.shape, bool)
    in_opaque = np.zeros(x.shape, bool)
    for kind, t in triangles:
        corners = [(4*p[0], 4*p[1]) for p in t]
        count += inside_of(corners, x, y, True)       # A sample on a shared edge is in neither interior.
        inside = inside_of(corners, x, y, False)
        drawn |= inside
        if kind == 'O':
            in_opaque |= inside
    assert count.max() <= 1, 'pieces overlap'
    assert not (opaque & ~drawn).any(), 'a visible texel is no longer drawn'
    assert not (in_opaque & ~safe).any(), 'an opaque piece reaches a texel next to a transparent one'
    assert area <= split._area2(footprint)
    return area


full = [(0, 0), (512, 0), (512, 512), (0, 512)]
drawn = {}
for name, options in [('hull', split.PRESETS['hull'][0]), ('lean', split.PRESETS['lean'][0]),
                      ('outline', split.PRESETS['outline'][0])]+[('auto%d' % i, o) for i, o in enumerate(split.AUTO)]:
    plan = split.plan_face(image, 0, full, 'outline', options)
    assert plan is not None, name
    drawn[name] = check_plan(image, full, plan)
    kinds = {kind for kind, _ in plan['faces']}
    assert ('O' in kinds) == bool(options['cores']), (name, kinds)
    assert 0 < plan['punch'] < 1 and plan['punch']+plan['opaque'] < 1
assert drawn['hull'] < split._area2(full)
for cell in (16, 8, 4):
    plan = split.plan_face(image, 0, full, 'grid', dict(cell=cell))
    assert plan is not None and {kind for kind, _ in plan['faces']} >= {'M'}
    assert cell == 16 or any(kind == 'O' for kind, _ in plan['faces'])
    check_plan(image, full, plan)
# Part of the texture, off the cell grid; a triangle; two repeats; mirrored.
for footprint, flip in (([(44, 100), (460, 100), (460, 412), (44, 412)], 0), ([(0, 0), (512, 256), (0, 512)], 0),
                        ([(0, 0), (1024, 0), (1024, 512), (0, 512)], 0), ([(0, 0), (1024, 0), (1024, 512), (0, 512)], 1)):
    for mode, options in (('grid', dict(cell=8)), ('outline', split.PRESETS['outline'][0]), ('outline', split.PRESETS['hull'][0])):
        plan = split.plan_face(image, flip, footprint, mode, options)
        if plan is not None:      # A plan that fails its own checks is not made; one that is made must hold.
            check_plan(image, footprint, plan, flip)
assert split.plan_face(image, 0, [(44, 100), (460, 100), (460, 412), (44, 412)], 'grid', dict(cell=8)) is not None
# A wholly transparent footprint draws nothing; a wholly safe one is one opaque piece.
assert split.plan_face(image, 0, [(416, 0), (512, 0), (512, 96), (416, 96)], 'outline', split.PRESETS['outline'][0])['faces'] == []
solid = np.zeros((32, 32), np.uint8)
plan = split.plan_face(solid, 0, [(0, 0), (256, 0), (256, 256), (0, 256)], 'outline', split.PRESETS['outline'][0])
assert [kind for kind, _ in plan['faces']] == ['O'] and plan['opaque'] == 1

# Triangulation with a vertex inside an edge: both parts of that edge are used.
F = Fraction
ring = split.conform([('M', [(F(0), F(0)), (F(8), F(0)), (F(8), F(8)), (F(0), F(8))]),
                      ('M', [(F(8), F(0)), (F(16), F(0)), (F(16), F(4)), (F(8), F(4))])])[0][1]
assert len(ring) == 5
parts = split.triangulate(ring)
assert len(parts) == 3 and sum(split._cross(*t) for t in parts) == 128
assert not any(split._between(a, b, r) for t in parts for a, b in zip(t, t[1:]+t[:1]) for r in ring)
assert split.source_header(split.split_header((0x6013, 5, 3600, 41216))) == (0x6013, 5, 3600, 41216)
assert split.split_header((0x6013, 5, 3600, 41216)) == (0x4013, 0x2005, 3600, 41216)


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
    refs = []
    for i, v in enumerate(vertices):
        refs.append(next(j for j in range(i+1) if vertices[j][:3] == v[:3]))
    body += struct.pack(f'<{len(refs)}H', *refs)
    size = models.HEADER.size+len(mesh)+len(body)
    return models.HEADER.pack(0x324d4e44, size, 1, len(vertices), len(faces), len(materials), 1, 0, 0, 0, 0, 0, 0, 0, 0, 0)+mesh+body


CUTOUT, WALL = (0x6009, 0, 100, 41216), (0x4009, 0, 200, 41216)
vertices = [(0, 0, 5, 0, 0), (4, 0, 5, 512, 0), (4, 4, 5, 512, 512), (0, 4, 5, 0, 512),            # the tree, two-sided
            (0, 0, 9, 0, 0), (4, 0, 9, 512, 0), (4, 4, 9, 512, 512), (0, 4, 9, 0, 512),            # a wall behind it
            (6, 0, 5, 512, 0), (10, 0, 5, 0, 0), (10, 4, 5, 0, 512), (6, 4, 5, 512, 512)]          # a tree, UVs turned over
two_sided = 0x1a01 | 1 << 17
source = pack([((0, 1, 2, 3), 0, two_sided, (0.0, 0.0, 1.0)), ((4, 5, 6, 7), 1, 0x1a01, (0.0, 0.0, 1.0)),
               ((8, 9, 10, 11), 0, two_sided, (0.0, 0.0, 1.0))], vertices, [CUTOUT, WALL])
light = (-0.45, -0.89, -0.45)
plain, _, plain_map = order.sort_pack(source, static_light=light, reduced_headers=[])
for mode in ('outline', 'grid'):
    done, report, face_map = order.sort_pack(source, static_light=light, reduced_headers=[],
                                             split=dict(images=lambda header: image, mode=mode, cell=8))
    assert report['split_cutouts']['faces_split'] == 2 and report['split_cutouts']['split_materials'] == 1
    h = models.HEADER.unpack_from(done)
    off = models.table_offsets(h)
    assert h[5] == 3 and h[15] == order.BAKED_LIGHT | order.SCENERY_CLASSES
    headers = [struct.unpack_from('<4H', done, off[3]+m*models.MATERIAL.size) for m in range(3)]
    words = [struct.unpack_from('<I', done, off[3]+m*models.MATERIAL.size+24)[0] for m in range(3)]
    assert headers == [CUTOUT, WALL, split.split_header(CUTOUT)]
    # The opaque pieces keep the cutout's stage and are marked for point sampling.
    assert words == [order.STAGE_CUTOUT, order.STAGE_OPAQUE, order.STAGE_CUTOUT | order.SCENERY_POINT], words
    assert done[off[3]+8:off[3]+24] == done[off[3]+2*models.MATERIAL.size+8:off[3]+2*models.MATERIAL.size+24]
    faces = [order.FACE6.unpack_from(done, off[2]+f*order.FACE6.size) for f in range(h[4])]
    records = [models.VERTEX.unpack_from(done, off[1]+i*models.VERTEX.size) for i in range(h[3])]
    old = [order.FACE6.unpack_from(plain, models.table_offsets(models.HEADER.unpack_from(plain))[2]+f*order.FACE6.size)
           for f in range(3)]
    old = {plain_map['new_to_old'][f]: face for f, face in enumerate(old)}
    area = {0: 0.0, 2: 0.0}
    lists = {0: set(), 2: set()}
    for f, face in enumerate(faces):
        origin = face_map['new_to_old'][f]
        before = old[origin]
        if origin == 1:
            assert face == before
            continue
        # Normal, baked lumas (front and back), two-sided bit, group and layer are the source's.
        assert face[8:] == before[8:] and face[6] == before[6] and face[7] & ~3 == before[7] & ~3
        assert face[7] & 3 == (1 if face[5] == 4 else 2) and face[4] in (0, 2)
        lists[origin].add(face[4])
        for i in range(1, face[5]-1):
            p = [records[face[k]] for k in (0, i, i+1)]
            # On the source polygon, where the source's UV map puts that UV.
            for x, y, z, u, v in p:
                expect = (u/128 if origin == 0 else 10-u/128, v/128)
                assert z == 5 and abs(x-expect[0]) < 1e-5 and abs(y-expect[1]) < 1e-5, (x, y, u, v)
            winding = (p[1][0]-p[0][0])*(p[2][1]-p[0][1])-(p[1][1]-p[0][1])*(p[2][0]-p[0][0])
            assert winding > 0     # Both source quads run counter-clockwise in x, y.
            area[origin] += winding/2
    assert lists == {0: {0, 2}, 2: {0, 2}}
    assert 0 < area[0] < 16 and abs(area[0]-area[2]) < 1e-4
    refs = struct.unpack_from(f'<{h[3]}H', done, off[4]+h[6]*models.INSTANCE.size)
    assert all(refs[refs[i]] == refs[i] and records[refs[i]][:3] == records[i][:3] and refs[i] <= i for i in range(h[3]))
    if reader:
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'split.dnm'
            path.write_bytes(done)
            assert subprocess.run([reader, str(path)], capture_output=True).returncode == 0
# The reduced view's cutouts stay whole unless asked for; without the option nothing changes.
kept, report, _ = order.sort_pack(source, static_light=light, reduced_headers=[],
                                  split=dict(images=lambda header: image, mode='outline', skip={CUTOUT}))
assert report['split_cutouts']['left_reduced_view'] == 2 and report['split_cutouts'].get('faces_split', 0) == 0 and kept == plain
# Tight outlines: punch-through pieces only, every visible texel still drawn, less area than the hull
# cuts alone once the trunk is its own piece; the pack keeps its materials and loses no opaque texel.
tight = {}
for options in split.TIGHT:
    plan = split.plan_face(image, 0, full, 'tight', options)
    assert plan is not None and {kind for kind, _ in plan['faces']} == {'M'} and plan['opaque'] == 0, options
    tight[options['pieces'], options['hull_cuts']] = check_plan(image, full, plan)
assert tight[2, 4] < tight[1, 4] < split._area2(full) and tight[4, 6] <= tight[2, 4]
assert split.plan_face(image, 0, [(416, 0), (512, 0), (512, 96), (416, 96)], 'tight', split.TIGHT[0])['faces'] == []
done, report, face_map = order.sort_pack(source, static_light=light, reduced_headers=[],
                                         split=dict(images=lambda header: image, mode='tight', least_gain=0.1))
h = models.HEADER.unpack_from(done)
off = models.table_offsets(h)
assert report['split_cutouts']['faces_split'] == 2 and report['split_cutouts']['split_materials'] == 0 and h[5] == 2
faces = [order.FACE6.unpack_from(done, off[2]+f*order.FACE6.size) for f in range(h[4])]
assert h[4] > 3 and all(face[4] == (1 if face_map['new_to_old'][f] == 1 else 0) for f, face in enumerate(faces))
assert sum(face[5]-2 for f, face in enumerate(faces) if face_map['new_to_old'][f] == 0) <= 8
# Measurement aids: faces removed, or whole on the opaque list; nothing else moves.
gone, report, face_map = order.sort_pack(source, static_light=light, reduced_headers=[],
                                         split=dict(images=lambda header: image, mode='none', drop={100}))
assert report['split_cutouts']['faces_dropped'] == 2 and face_map['new_to_old'] == [1]
gone, report, face_map = order.sort_pack(source, static_light=light, reduced_headers=[],
                                         split=dict(images=lambda header: image, mode='none', drop_faces=[2]))
assert report['split_cutouts']['faces_dropped'] == 1 and sorted(face_map['new_to_old']) == [0, 1]
solid_pack, report, face_map = order.sort_pack(source, static_light=light, reduced_headers=[],
                                               split=dict(images=lambda header: image, mode='none', opaque={100}))
h = models.HEADER.unpack_from(solid_pack)
off = models.table_offsets(h)
assert report['split_cutouts']['faces_made_opaque'] == 2 and h[4] == 3 and h[5] == 3
assert struct.unpack_from('<4H', solid_pack, off[3]+2*models.MATERIAL.size) == split.split_header(CUTOUT)
for f in range(3):
    face = order.FACE6.unpack_from(solid_pack, off[2]+f*order.FACE6.size)
    assert face[4] == (1 if face_map['new_to_old'][f] == 1 else 2) and face[5] == 4
untouched, report, _ = order.sort_pack(source, static_light=light, reduced_headers=[],
                                       split=dict(images=lambda header: image, mode='none'))
assert untouched == plain
print('split cutouts: plans, tiling, pack pieces and classes hold')
