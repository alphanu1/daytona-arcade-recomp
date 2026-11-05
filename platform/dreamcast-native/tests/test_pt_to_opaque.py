"""ROM-free checks of punch-through faces moved to the opaque list (tools/pt_to_opaque.py,
prepare_face_order.py --pt-to-opaque).

python3 platform/dreamcast-native/tests/test_pt_to_opaque.py [build/check_native_meshpack]
"""
from fractions import Fraction
from pathlib import Path
import random
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
import pt_to_opaque as po

reader = sys.argv[1] if len(sys.argv) > 1 else None

# reach(): exactly the texels a bilinear sampler (texel centres at half integers) reads with a
# non-zero weight somewhere inside the open polygon; brute force on a fine grid of interior points.
def brute(poly, scale=8):
    us, vs = [p[0] for p in poly], [p[1] for p in poly]
    found = set()
    step = Fraction(1, 7)
    u = Fraction(min(us))+step/3
    while u < max(us):
        v = Fraction(min(vs))+step/5
        while v < max(vs):
            if all((q[0]-p[0])*(v-p[1])-(q[1]-p[1])*(u-p[0]) > 0 for p, q in zip(poly, poly[1:]+poly[:1])):
                tu, tv = u/scale-Fraction(1, 2), v/scale-Fraction(1, 2)
                for i in {tu.__floor__(), tu.__floor__()+(tu != tu.__floor__())}:
                    for j in {tv.__floor__(), tv.__floor__()+(tv != tv.__floor__())}:
                        found.add((i, j))
            v += step
        u += step
    return found


random.seed(5)
for trial in range(40):
    while True:
        poly = [(random.randint(0, 40), random.randint(0, 40)) for _ in range(3)]
        area = (poly[1][0]-poly[0][0])*(poly[2][1]-poly[0][1])-(poly[1][1]-poly[0][1])*(poly[2][0]-poly[0][0])
        if abs(area) > 30:
            break
    if area < 0:
        poly = poly[::-1]
    xs, ys = po.reach(poly)
    exact = set(zip(xs.tolist(), ys.tolist()))
    sampled = brute(poly)
    # The grid finds a subset; every texel it misses must be one whose weight region the polygon only grazes.
    assert sampled <= exact, (poly, sampled-exact)
    assert len(exact-sampled) <= 2, (poly, exact-sampled)
# A texel-aligned rectangle: 2..62 x 2..30 texels reads texels 1..62 x 1..30 (half a texel beyond each edge).
xs, ys = po.reach([(16, 16), (496, 16), (496, 240), (16, 240)])
assert set(zip(xs.tolist(), ys.tolist())) == {(i, j) for i in range(1, 63) for j in range(1, 31)}
# Mip level 1 (texels 16 units wide): the same rectangle reads level texels 0..31 x 0..15.
xs, ys = po.reach([(16, 16), (496, 16), (496, 240), (16, 240)], 16)
assert set(zip(xs.tolist(), ys.tolist())) == {(i, j) for i in range(0, 32) for j in range(0, 16)}

# Two 64x64 textures with a transparent one-texel frame; the first also has a hole at (40, 40).
framed = np.full((64, 64), 3, np.uint8)
framed[0, :] = framed[63, :] = framed[:, 0] = framed[:, 63] = 15
holed = framed.copy()
holed[40, 40] = 15
holes = po.level_holes(holed)
assert len(holes) == 7 and holes[0][40, 40] and not holes[1][20, 20] and holes[6].shape == (1, 1)
pictures = {100: holed, 200: framed}

A, B, C, D = (0x6009, 0, 100, 41216), (0x6009, 0, 200, 41216), (0x6009, 1, 100, 41216), (0x6009, 2, 200, 41216)
vertices = [(0, 0, 5, 16, 16), (4, 0, 5, 496, 16), (4, 2, 5, 496, 240), (0, 2, 5, 16, 240),     # 0 A: inside the frame -> opaque
            (0, 3, 5, 0, 256), (4, 3, 5, 512, 256), (4, 5, 5, 512, 512), (0, 5, 5, 0, 512),     # 4 A: over the hole -> holes
            (6, 0, 5, 0, 0), (10, 0, 5, 512, 0), (10, 4, 5, 512, 512), (6, 4, 5, 0, 512),       # 8 B: whole texture -> cut to 1..63
            (12, 0, 5, 0, 0), (16, 0, 5, 512, 0), (12, 4, 5, 0, 512),                           # 12 B: triangle -> cut
            (20, 0, 5, 528, 16), (22, 0, 5, 600, 16), (22, 1, 5, 600, 100), (20, 1, 5, 528, 100),  # 15 C: wraps to 1..11 -> opaque
            (30, 0, 5, 16, 16), (34, 0, 5, 496, 16), (34, 2, 5, 496, 240), (30, 2, 5, 16, 240)]  # 19 D: reduced view -> kept


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


normal = (0.0, 0.0, 1.0)
source = pack([((0, 1, 2, 3), 0, 0x1a01, normal), ((4, 5, 6, 7), 0, 0x1a01, normal), ((8, 9, 10, 11), 1, 0x1a01, normal),
               ((12, 13, 14), 1, 0x1a02, normal), ((15, 16, 17, 18), 2, 0x1a01, normal), ((19, 20, 21, 22), 3, 0x1a01, normal)],
              vertices, [A, B, C, D])
light = (-0.45, -0.89, -0.45)
plain, plain_report, plain_map = order.sort_pack(source, static_light=light, reduced_headers=[D])
again, _, _ = order.sort_pack(source, static_light=light, reduced_headers=[D])
assert plain == again and 'pt_to_opaque' not in plain_report and 'pt_to_opaque' not in plain_map
data, report, face_map = order.sort_pack(source, static_light=light, reduced_headers=[D],
                                         to_opaque=dict(images=lambda header: pictures[header[2]], skip={D}))
total = report['pt_to_opaque']['total']
assert total['opaque'] == 2 and total['trimmed'] == 2 and total['holes'] == 1 and total['kept_reduced_view'] == 1, total
assert total['faces_converted'] == 4 and total['faces_cut'] == 2 and report['pt_to_opaque']['converted_materials'] == 3
if reader:
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder)/'converted.dnm'
        path.write_bytes(data)
        assert subprocess.run([reader, str(path)], capture_output=True).returncode == 0

h, faces, records, headers, classes = tables(data)
_, plain_faces, _, plain_headers, plain_classes = tables(plain)
by_source = {}
for f, old in enumerate(face_map['new_to_old']):
    by_source.setdefault(old, []).append(f)
old_of = {old: f for f, old in enumerate(plain_map['new_to_old'])}
for source_face, made in by_source.items():
    reference = plain_faces[old_of[source_face]]
    for f in made:
        face = faces[f]
        header, base = headers[face[4]], plain_headers[reference[4]]
        if source_face in (1, 5):          # holes, reduced view: untouched
            assert header == base and face[:4] == reference[:4]
            continue
        # Opaque list, the source's texture, colour and luma word, the mark; the source's class and light.
        assert header == po.converted_header(base) and po.converted_base(header) == base
        assert (header[0] >> 13) & 7 == 2 and classes[face[4]] == plain_classes[reference[4]]
        assert face[6] == reference[6] and face[8:11] == reference[8:11] and face[13] == reference[13]
        corners = [records[i] for i in face[:face[5]]]
        if source_face in (0, 4):
            assert corners == [records[i] for i in face[:face[5]]] and len(made) == 1
            assert sorted((c[3], c[4]) for c in corners) == sorted((c[3], c[4]) for c in
                                                                   [vertices[i] for i in ([0, 1, 2, 3] if source_face == 0 else [15, 16, 17, 18])])
        else:
            # Cut to the opaque texels 1..62 (UV 8..504); every corner where the source's own map puts its UV.
            assert all(8 <= c[3] <= 504 and 8 <= c[4] <= 504 for c in corners)
            x0, y0 = (6, 0) if source_face == 2 else (12, 0)
            for c in corners:
                assert abs(c[0]-(x0+4*c[3]/512)) < 1e-5 and abs(c[1]-4*c[4]/512) < 1e-5 and c[2] == 5
    if source_face == 2:
        assert len(made) == 1 and sorted((records[i][3], records[i][4]) for i in faces[made[0]][:4]) == \
            [(8, 8), (8, 504), (504, 8), (504, 504)]
# Material A keeps its punch-through record for the face with the hole; the converted faces of A use a copy.
assert A in headers and po.converted_header(A) in headers and B not in [headers[f[4]] for f in faces]
# Only the opaque list draws converted faces; the source's order and layers otherwise.
assert sum((headers[f[4]][0] >> 13) & 3 == 3 for f in faces) == 2

# The texture words of level-of-detail rules and cores are left alone; without trimming the cut faces stay.
data, report, _ = order.sort_pack(source, static_light=light, reduced_headers=[D],
                                  to_opaque=dict(images=lambda header: pictures[header[2]], skip={D}, trim=False))
total = report['pt_to_opaque']['total']
assert total['opaque'] == 2 and total['holes'] == 3 and 'trimmed' not in total
data, report, _ = order.sort_pack(source, static_light=light, reduced_headers=[D],
                                  to_opaque=dict(images=lambda header: pictures[header[2]], skip={D}, skip_words={100}))
total = report['pt_to_opaque']['total']
assert total['kept_level_of_detail_or_cores_texture'] == 3 and total['trimmed'] == 2 and 'opaque' not in total
# Mip levels: the frame's corner blocks are holes of level 1 (one opaque texel of four), within the
# level-1 reach of every converted face here: reported per face; --pt-to-opaque-strict-levels keeps them all.
_, report, face_map = order.sort_pack(source, static_light=light, reduced_headers=[D],
                                      to_opaque=dict(images=lambda header: pictures[header[2]], skip={D}))
assert all(e['level_holes'][0] > 0 for e in face_map['pt_to_opaque'] if e['kind'] in ('opaque', 'trimmed'))
data, report, _ = order.sort_pack(source, static_light=light, reduced_headers=[D],
                                  to_opaque=dict(images=lambda header: pictures[header[2]], skip={D}, strict_levels=True))
total = report['pt_to_opaque']['total']
assert 'faces_converted' not in total and total['level_holes'] == 4 and report['pt_to_opaque']['converted_materials'] == 0
print('PASS: pt_to_opaque reach, classes, cut faces, materials and classes')
