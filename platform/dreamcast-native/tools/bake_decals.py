"""Layered decals baked into the texture underneath (static scenery).

Used by prepare_face_order.py --bake-decals SPEC.json (mesh side) and by
prepare_palette_materials.py --composites (texture side); also a program:

python3 platform/dreamcast-native/tools/bake_decals.py candidates SOURCE.dnm NEW_SPEC.json
    [--static-light X Y Z] [--fill ORDER.json FILL]
python3 platform/dreamcast-native/tools/bake_decals.py preview COMPOSITES.json VIDEO_DUMP NEW_DIR

The arcade builds some pictures from an opaque polygon and punch-through
polygons drawn in its plane afterwards (a carving on a rock wall whose rock
texture has a hole where the figure's own texture goes, a logo on a banner,
moss over a wall, the finish line on the asphalt). On the PowerVR every such
layer is a punch-through polygon: paid for on the SH4 and, pixel by pixel, on
the punch-through list. Here a pile (one base face and the punch-through
faces lying on it) becomes ONE face of the base's list whose texture is the
layers laid over each other.

Piles (find_piles):
  same     the faces have the same corner positions (one mesh, arcade order:
           base first). Their texture coordinates may differ.
  inside   a punch-through face lies in the plane of, and inside, one earlier
           face (prepare_face_order.find_layers' evidence).
A pile is left alone when a layer is untextured or synthetic, when the faces
disagree in sidedness or are two-sided with different lumas per side, or when
a face that is not part of the pile lies between its layers.

Composites: piles with the same texture words (base, overlays) form one
composite, named in the specification file, with ONE new texture:
  texture  every layer of every pile has the base's vertex records (position
           and UV) and texture size: the new texture has the base's size and
           the faces keep their vertices.
  atlas    otherwise each face gets a rectangle of a new texture, rendered at
           the densest layer's texel density (times "density") with a texel
           of margin, and new vertex records (same positions, new UVs).
           Faces that would hold the same texels share a rectangle.
An inside pile cuts its base (an affine quad): the part the overlays cover
takes the composite, the rest keeps the base material. Cuts are strips across
the quad or, when the covered part touches no edge, a frame of four
trapezoids around it. A cut may put a vertex on an outer edge only if no
other face of the pack has that edge (both corner positions): otherwise the
composite grows to the whole face, so no T-junction is ever created. Pieces
share vertex records along their common edges.

Texels and palette ("palette" of a composite in the specification):
  ramp     no palette bank. The composite stays an index image on the base's
           luma ramp and is drawn with the base's own palette and vertex
           colour: base texels keep their index, an overlay texel takes the
           index whose colour under the base's ramp and colour word is
           nearest to its own. The overlay loses its own tint.
  own      one bank: 16 colours (15 and the transparent entry for a
           punch-through base) quantised from the composite.
  shared:T one bank for all composites with the same tag T, quantised
           jointly.
With a palette of its own the baked face light is the vertex colour, as for
every binding: per base header and luma a gain g per channel, the base
palette's brightest entry times one factor per composite chosen so that every
overlay colour fits; a texel holds colour/g. Where base and overlay carry
different lumas or colour words the ratio is thereby in the texels.
The error of every choice against the exact layered picture is measured per
face and reported.

A composite material is a copy of the base's record with a new header and
takes the base's scenery stage and depth class (inherit_classes).
ROM-derived output stays under build/.
"""
import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import struct

import numpy as np

import prepare_models as pm

COMPOSITE_MARK = 0x4000   # header word 1, as prepare_face_order.flatten_stacks
RAMP_WORD = 0x8000        # header word 2 of a ramp composite: no arcade texture word has this bit
INSIDE = 1e-3             # parametric tolerance of "inside the base quad"
AFFINE = 1e-3             # model units: quad corner against the parallelogram
ATLAS_SIZES = (32, 64, 128, 256, 512, 1024)
SHARED_SAMPLES = 16384    # texels one composite contributes to a shared palette's quantisation


def _kind(header):
    return (header[0] >> 13) & 7


def _plain(header, kinds=(2, 3)):
    """An arcade textured header (opaque or punch-through), not a synthetic one."""
    return _kind(header) in kinds and not header[1] & 0xff00 and not header[2] & RAMP_WORD


def _size(header):
    return 32 << (header[0] & 7), 32 << ((header[0] >> 3) & 7)


def _basis(points):
    """Origin and orthonormal in-plane axes of a polygon (Newell normal)."""
    p = np.array(points, float)
    n = np.zeros(3)
    for a, b in zip(p, np.roll(p, -1, 0)):
        n += np.cross(a, b)
    n /= np.linalg.norm(n)
    e1 = p[1]-p[0]
    e1 -= n*np.dot(e1, n)
    e1 /= np.linalg.norm(e1)
    return p[0], e1, np.cross(n, e1)


def _flat(points, basis):
    origin, e1, e2 = basis
    d = np.array(points, float)-origin
    return np.stack([d @ e1, d @ e2], 1)


def find_piles(pack, find_layers):
    """[dict(base, overlays, kind, words)] over the faces of `pack` (pack face
    indices, arcade order) and a Counter of what was left and why."""
    size = pm.MATERIAL.size
    headers = [struct.unpack_from('<4H', pack['materials'], m*size) for m in range(pack['header'][5])]
    faces = pack['faces']
    records = [pm.VERTEX.unpack_from(pack['vertices'], i*pm.VERTEX.size) for i in range(pack['header'][3])]
    left = Counter()

    def header(f):
        return headers[faces[f][4]]

    def corners(f, mesh):
        return [records[mesh[0]+i] for i in faces[f][:faces[f][5]]]

    owner = {}
    for mesh in pack['meshes']:
        for f in range(mesh[2], mesh[2]+mesh[3]):
            owner[f] = mesh
    piles, member = {}, {}
    for mesh in pack['meshes']:
        by_polygon = defaultdict(list)
        for f in range(mesh[2], mesh[2]+mesh[3]):
            if faces[f][7] & (3 << 8):
                by_polygon[frozenset(c[:3] for c in corners(f, mesh))].append(f)
        for members in by_polygon.values():
            if len(members) < 2 or not _plain(header(members[0])) or \
                    any(not _plain(header(f), (3,)) for f in members[1:]):
                continue
            piles[members[0]] = dict(base=members[0], overlays=members[1:], kind='same')
            for f in members:
                member[f] = members[0]
    _, _, pairs = find_layers(pack)
    under, over = defaultdict(list), defaultdict(list)
    for i, j, gap, _ in pairs:
        under[j].append((i, gap))
        over[i].append(j)
    for j in sorted(under):
        if j in member or not _plain(header(j), (3,)) or not faces[j][7] & (3 << 8):
            continue
        bases = {member.get(i, i) for i, _ in under[j]}
        if len(bases) != 1:
            left['overlay_on_several_faces'] += 1
            continue
        base = bases.pop()
        if owner[base] is not owner[j] or not _plain(header(base)) or faces[base][5] != 4 or \
                max(gap for _, gap in under[j]) > 2e-3:
            left['overlay_base_unsuitable'] += 1
            continue
        mesh = owner[base]
        p = np.array([c[:3] for c in corners(base, mesh)], float)
        if np.abs(p[0]+p[2]-p[1]-p[3]).max() > AFFINE:
            left['overlay_base_not_affine'] += 1
            continue
        st = [np.linalg.lstsq(np.stack([p[1]-p[0], p[3]-p[0]], 1), np.array(c[:3], float)-p[0], rcond=None)[0]
              for c in corners(j, mesh)]
        if min(min(v) for v in st) < -INSIDE or max(max(v) for v in st) > 1+INSIDE:
            left['overlay_reaches_beyond_its_base'] += 1
            continue
        if base in member and member[base] != base:
            left['overlay_on_an_overlay'] += 1
            continue
        pile = piles.setdefault(base, dict(base=base, overlays=[], kind='inside'))
        if pile['kind'] == 'same':
            left['overlay_on_a_pile'] += 1
            continue
        pile['overlays'].append(j)
        member[base] = member[j] = base
    result = []
    for base, pile in sorted(piles.items()):
        members = [base]+pile['overlays']
        sides = {(faces[f][7] >> 17) & 1 for f in members}
        if len(sides) != 1:
            left['pile_sidedness_differs'] += 1
            continue
        # A face outside the pile that lies on one of its members must have
        # been drawn after every layer: then it stays on top of the composite.
        if any(j not in members and j < max(members) for f in members for j in over[f]):
            left['pile_with_a_foreign_face_between'] += 1
            continue
        words = [header(base)[2]]
        for f in pile['overlays']:
            if header(f)[2] not in words[1:]:
                words.append(header(f)[2])
        result.append(dict(pile, words=words, two_sided=bool(sides.pop())))
    return result, left


def _pack_rectangles(sizes):
    """Shelf packing of (w, h) rectangles into the smallest power-of-two
    texture: (width, height, [(x, y)]) or None."""
    order = sorted(range(len(sizes)), key=lambda i: (-sizes[i][1], -sizes[i][0], i))
    options = sorted(((w*h, max(w, h), w, h) for w in ATLAS_SIZES for h in ATLAS_SIZES))
    for _, _, width, height in options:
        place, x, y, shelf = [None]*len(sizes), 0, 0, 0
        for i in order:
            w, h = sizes[i]
            if w > width:
                break
            if x+w > width:
                x, y, shelf = 0, y+shelf, 0
            if y+h > height:
                break
            place[i] = (x, y)
            x, shelf = x+w, max(shelf, h)
        else:
            return width, height, place
    return None


def bake(pack, lumas, spec, find_layers, first_number=0, only=None, skip=()):
    """Bake the composites of `spec` into `pack` (prepare_face_order.read_pack
    tables, arcade face order) in place. lumas: (front, back) per source
    face. Returns the composite descriptions for the texture side and a
    report. pack['decal_materials'] maps each new material to its base."""
    size = pm.MATERIAL.size
    headers = [struct.unpack_from('<4H', pack['materials'], m*size) for m in range(pack['header'][5])]
    faces = pack['faces']
    origin = pack.setdefault('origin', list(range(len(faces))))
    records = [pm.VERTEX.unpack_from(pack['vertices'], i*pm.VERTEX.size) for i in range(pack['header'][3])]
    piles, left = find_piles(pack, find_layers)
    owner = {}
    for index, mesh in enumerate(pack['meshes']):
        for f in range(mesh[2], mesh[2]+mesh[3]):
            owner[f] = index
    entries = []
    for entry in spec['composites']:
        if not entry.get('enabled', True) or entry['name'] in skip or (only is not None and entry['name'] not in only):
            continue
        entries.append(entry)
    if len({e['name'] for e in entries}) != len(entries):
        raise ValueError('composite names of the specification repeat')
    by_words = defaultdict(list)
    for pile in piles:
        by_words[pile['words'][0], frozenset(pile['words'][1:])].append(pile)

    def corners(f):
        mesh = pack['meshes'][owner[f]]
        return [records[mesh[0]+i] for i in faces[f][:faces[f][5]]]

    def light(f):
        front, back = lumas[origin[f]]
        return front

    # Faces that own an edge (two corner positions), for the T-junction rule.
    edge_users = defaultdict(set)
    for f in range(len(faces)):
        p = [c[:3] for c in corners(f)]
        for a, b in zip(p, p[1:]+p[:1]):
            edge_users[frozenset((a, b))].add(f)

    materials = bytearray(pack['materials'])
    decal_materials, composites, report = {}, [], []
    drop, replace = set(), {}
    new_vertices = defaultdict(list)     # mesh index -> packed records appended
    number, texture_number = first_number, 0
    used_headers = set(headers)

    def same_records(pile):
        mesh = pack['meshes'][owner[pile['base']]]
        base = sorted(pack['vertices'][(mesh[0]+i)*16:(mesh[0]+i)*16+16] for i in faces[pile['base']][:faces[pile['base']][5]])
        return pile['kind'] == 'same' and all(
            sorted(pack['vertices'][(mesh[0]+i)*16:(mesh[0]+i)*16+16] for i in faces[f][:faces[f][5]]) == base and
            headers[faces[f][4]][0] & 0x33f == headers[faces[pile['base']][4]][0] & 0x33f for f in pile['overlays'])

    jobs = []
    for entry in entries:
        key = (entry['layers'][0], frozenset(entry['layers'][1:]))
        members = by_words.get(key, [])
        mode = entry.get('palette', 'own')
        if not (mode in ('ramp', 'own') or mode.startswith('shared:')):
            raise ValueError(f'composite {entry["name"]}: palette must be ramp, own or shared:TAG')
        note = Counter()
        chosen = []
        for pile in members:
            every = [pile['base']]+pile['overlays']
            if pile['two_sided'] and any(lumas[origin[f]][0] != lumas[origin[f]][1] for f in every):
                note['left_two_sided_with_two_lumas'] += 1
                continue
            chosen.append(pile)
        if not chosen:
            report.append(dict(name=entry['name'], piles=0, **note))
            continue
        in_texture = all(same_records(pile) for pile in chosen) and not entry.get('atlas')
        sets = [chosen]
        if in_texture and not entry.get('merge'):
            # One texture holds piles whose texels agree: the same colour words
            # and luma ratios, or footprints that do not overlap.
            width, height = _size(headers[faces[chosen[0]['base']][4]])
            flip = headers[faces[chosen[0]['base']][4]][0]
            classes = defaultdict(list)
            for pile in chosen:
                h = headers[faces[pile['base']][4]]
                classes[h[1], h[3], tuple((headers[faces[f][4]], round(light(f)/max(light(pile['base']), 1), 2))
                                          for f in pile['overlays'])].append(pile)
            sets, masks = [], []
            for signature in sorted(classes):
                mask = np.zeros((height, width), bool)
                for pile in classes[signature]:
                    uv = np.array([c[3:5] for c in corners(pile['base'])], float)/8
                    x0, y0 = np.floor(uv.min(0)).astype(int)
                    x1, y1 = np.ceil(uv.max(0)).astype(int)
                    xs, ys = np.meshgrid(np.arange(x0, x1), np.arange(y0, y1))
                    points = np.stack([xs.reshape(-1)+0.5, ys.reshape(-1)+0.5], 1)
                    inside = _interpolate(uv, uv, points)[1] >= 0.25
                    mask[_wrap(points[inside, 1], height, flip & 0x200), _wrap(points[inside, 0], width, flip & 0x100)] = True
                for k in range(len(sets)):
                    if not (masks[k] & mask).any():
                        sets[k] += classes[signature]
                        masks[k] |= mask
                        break
                else:
                    sets.append(list(classes[signature]))
                    masks.append(mask)
        for k, subset in enumerate(sets):
            jobs.append((entry, entry['name']+('#%d' % (k+1) if k else ''), subset, in_texture, Counter(note)))
    for entry, name, chosen, in_texture, note in jobs:
        mode = entry.get('palette', 'own')
        base_headers = sorted({headers[faces[p['base']][4]] for p in chosen})
        cutout = {_kind(h) == 3 for h in base_headers}
        if len(cutout) != 1 or len({h[0] & 0x33f for h in base_headers}) != 1:
            raise ValueError(f'composite {name}: its bases differ in list, size or flip')
        cutout = cutout.pop()
        if cutout and mode == 'ramp':
            raise ValueError(f'composite {name}: a punch-through base needs a palette of its own')
        density = float(entry.get('density', 1.0))
        described, shapes = [], []
        for pile in chosen:
            base = pile['base']
            mesh_index = owner[base]
            mesh = pack['meshes'][mesh_index]
            base_corners = corners(base)
            basis = _basis([c[:3] for c in base_corners])
            base_xy = _flat([c[:3] for c in base_corners], basis)
            layers = [dict(header=list(headers[faces[base][4]]), luma=light(base), xy=base_xy.tolist(),
                           uv=[list(c[3:5]) for c in base_corners], clip=False)]
            for f in pile['overlays']:
                c = corners(f)
                layers.append(dict(header=list(headers[faces[f][4]]), luma=light(f),
                                   xy=_flat([p[:3] for p in c], basis).tolist(), uv=[list(p[3:5]) for p in c],
                                   clip=pile['kind'] == 'inside'))
            pieces = [dict(st=None, composite=True)]
            if pile['kind'] == 'inside':
                p = np.array([c[:3] for c in base_corners], float)
                frame = np.stack([p[1]-p[0], p[3]-p[0]], 1)
                st = np.array([np.linalg.lstsq(frame, np.array(c[:3], float)-p[0], rcond=None)[0]
                               for f in pile['overlays'] for c in corners(f)])
                low, high = [float(v) for v in st.min(0)], [float(v) for v in st.max(0)]
                for a in range(2):
                    low[a] = 0.0 if low[a] < INSIDE else low[a]
                    high[a] = 1.0 if high[a] > 1-INSIDE else high[a]
                uv = np.array([c[3:5] for c in base_corners], float)
                affine_uv = not np.abs(uv[0]+uv[2]-uv[1]-uv[3]).max()
                bp = [c[:3] for c in base_corners]

                def free(a, b):
                    return edge_users[frozenset((bp[a], bp[b]))] <= set([base]+pile['overlays'])
                interior = [low[a] > 0 or high[a] < 1 for a in range(2)]
                framed = all(low[a] > 0 and high[a] < 1 for a in range(2))
                # A cut across axis a ends on the two outer edges that run along a.
                along = {0: (free(0, 1) and free(2, 3)), 1: (free(1, 2) and free(3, 0))}
                if not affine_uv or entry.get('whole_face'):
                    low, high = [0.0, 0.0], [1.0, 1.0]
                    note['whole_face_not_affine_or_requested'] += 1
                elif framed:
                    pass
                else:
                    if all(interior):
                        # A corner or edge region: strips across the axis that grows least.
                        grow = [(high[1-a]-low[1-a])*(1-(high[a]-low[a])) for a in range(2)]
                        a = 0 if grow[0] <= grow[1] else 1
                        low[a], high[a] = 0.0, 1.0
                        interior[a] = False
                        note['corner_region_grown_to_a_strip'] += 1
                    for a in range(2):
                        if interior[a] and not along[a]:
                            low[a], high[a] = 0.0, 1.0
                            note['strip_grown_to_whole_face_shared_edge'] += 1
                if (low, high) != ([0.0, 0.0], [1.0, 1.0]):
                    (s0, t0), (s1, t1) = low, high
                    inner = [(s0, t0), (s1, t0), (s1, t1), (s0, t1)]
                    pieces = [dict(st=inner, composite=True)]
                    if framed:
                        outer = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
                        for k in range(4):
                            pieces.append(dict(st=[outer[k], outer[(k+1) % 4], inner[(k+1) % 4], inner[k]], composite=False))
                    else:
                        if s0 > 0:
                            pieces.append(dict(st=[(0.0, 0.0), (s0, 0.0), (s0, 1.0), (0.0, 1.0)], composite=False))
                        if s1 < 1:
                            pieces.append(dict(st=[(s1, 0.0), (1.0, 0.0), (1.0, 1.0), (s1, 1.0)], composite=False))
                        if t0 > 0:
                            pieces.append(dict(st=[(0.0, 0.0), (1.0, 0.0), (1.0, t0), (0.0, t0)], composite=False))
                        if t1 < 1:
                            pieces.append(dict(st=[(0.0, t1), (1.0, t1), (1.0, 1.0), (0.0, 1.0)], composite=False))
                    note['faces_cut'] += 1
            described.append(dict(pile=pile, layers=layers, pieces=pieces, basis=basis, mesh=mesh_index))
        # Target polygons of the composite pieces.
        targets = []
        for item in described:
            base = item['pile']['base']
            base_corners = corners(base)
            for piece in item['pieces']:
                if not piece['composite']:
                    continue
                if piece['st'] is None:
                    xy = np.array(item['layers'][0]['xy'])
                else:
                    b = np.array(item['layers'][0]['xy'])
                    xy = np.array([b[0]+s*(b[1]-b[0])+t*(b[3]-b[0]) for s, t in piece['st']])
                piece['xy'] = xy.tolist()
                targets.append((item, piece))
        if in_texture:
            width, height = _size(base_headers[0])
            flip = base_headers[0][0] & 0x300
            for item, piece in targets:
                piece['target'] = item['layers'][0]['uv']
        else:
            flip = 0
            # Texel extent of the target's two edge directions in every layer.
            sizes, slot_of, slots = [], {}, []
            for item, piece in targets:
                xy = np.array(piece['xy'])
                n = len(xy)
                edges = [(xy[1]-xy[0]), (xy[2]-xy[1])] if n == 4 else [(xy[1]-xy[0]), (xy[2]-xy[0])]
                if n == 4:
                    edges = [edges[0], edges[1], xy[2]-xy[3], xy[3]-xy[0]]
                extent = [0.0, 0.0]
                for layer in item['layers']:
                    lx, lu = np.array(layer['xy']), np.array(layer['uv'], float)/8
                    jac = np.linalg.solve(np.stack([lx[1]-lx[0], lx[2]-lx[0]], 1).T, np.stack([lu[1]-lu[0], lu[2]-lu[0]]))
                    for k, e in enumerate(edges):
                        extent[k & 1] = max(extent[k & 1], float(np.linalg.norm(e @ jac)))
                w, h = (max(4, min(1022, int(math.ceil(v*density-1e-6)))) for v in extent)
                # The margin must not push a rectangle over a power of two.
                w, h = (v if v+2 <= 1 << (v-1).bit_length() else (1 << (v-1).bit_length())-2 for v in (w, h))
                # Faces that would hold the same texels share a rectangle.
                signature = (n, w, h, tuple(
                    (tuple(layer['header'][:3]), layer['header'][3], layer['luma'], layer['clip'],
                     tuple(map(tuple, layer['uv'])),
                     tuple(tuple(round(v, 3) for v in q) for q in (np.array(layer['xy'])-xy[0]).tolist()))
                    for layer in item['layers']), tuple(tuple(round(v, 3) for v in q) for q in (xy-xy[0]).tolist()))
                if signature not in slot_of:
                    slot_of[signature] = len(slots)
                    slots.append((w+2, h+2))
                piece['slot'], piece['wh'] = slot_of[signature], (w, h)
            packed = _pack_rectangles(slots)
            if packed is None:
                raise ValueError(f'composite {name}: its faces do not fit a 1024x1024 texture; lower "density"')
            width, height, place = packed
            for item, piece in targets:
                x, y = place[piece['slot']]
                w, h = piece['wh']
                x0, y0, x1, y1 = 8*(x+1), 8*(y+1), 8*(x+1+w), 8*(y+1+h)
                piece['target'] = [[x0, y0], [x1, y0], [x1, y1], [x0, y1]][:len(piece['xy'])] \
                    if len(piece['xy']) == 4 else [[x0, y0], [x1, y0], [x1, y1]]
                piece['rect'] = [x, y, w+2, h+2]
        # Materials: one per base header.
        size_bits = (width.bit_length()-6) | (height.bit_length()-6) << 3
        material_of, material_rows = {}, []
        for h in base_headers:
            base_material = next(faces[p['base']][4] for p in chosen if headers[faces[p['base']][4]] == h)
            if mode == 'ramp':
                new = ((0x6000 if cutout else 0x4000) | size_bits | flip, h[1] & 0xff, RAMP_WORD | texture_number, h[3])
            else:
                if number > 0xff:
                    raise ValueError('more than 256 composite materials')
                new = ((0x6000 if cutout else 0x4000) | size_bits | flip, COMPOSITE_MARK | number, h[2], h[3])
                number += 1
            if new in used_headers:
                raise ValueError('composite header collides with a material of the pack')
            used_headers.add(new)
            record = bytearray(materials[base_material*size:(base_material+1)*size])
            struct.pack_into('<4H', record, 0, *new)
            for p in chosen:
                m = faces[p['base']][4]
                if headers[m] == h and materials[m*size+8:(m+1)*size] != record[8:]:
                    raise ValueError('bases of one composite material differ in their light terms')
            material_of[h] = len(materials)//size
            decal_materials[material_of[h]] = base_material
            materials += record
            material_rows.append(dict(header=list(new), base=list(h)))
        texture_number += 1
        # Faces.
        out_faces, removed, added, uv_error = [], [], 0, 0.0
        for item in described:
            pile, mesh_index = item['pile'], item['mesh']
            base = pile['base']
            face = faces[base]
            mesh = pack['meshes'][mesh_index]
            base_corners = corners(base)
            pieces_out = []
            for piece in item['pieces']:
                if piece['st'] is None and in_texture:
                    pieces_out.append((*face[:4], material_of[headers[face[4]]], *face[5:]))
                else:
                    indices = []
                    st = piece['st'] or [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)][:face[5]]
                    for k, (s, t) in enumerate(st):
                        corner = {(0.0, 0.0): 0, (1.0, 0.0): 1, (1.0, 1.0): 2, (0.0, 1.0): 3}.get((s, t)) \
                            if piece['st'] is not None else k
                        if corner is not None:
                            position = base_corners[corner][:3]
                            uv = base_corners[corner][3:5]
                        else:
                            p0, p1, p3 = (np.array(base_corners[c][:3], np.float64) for c in (0, 1, 3))
                            position = tuple(float(np.float32(v)) for v in p0+s*(p1-p0)+t*(p3-p0))
                            u0, u1, u3 = (np.array(base_corners[c][3:5], np.float64) for c in (0, 1, 3))
                            exact = u0+s*(u1-u0)+t*(u3-u0)
                            uv = tuple(int(v) for v in np.rint(exact))
                            uv_error = max(uv_error, float(np.abs(exact-uv).max())/8)
                        if piece['composite']:
                            uv = tuple(piece['target'][k])
                        if not 0 <= uv[0] <= 65535 or not 0 <= uv[1] <= 65535:
                            raise ValueError('composite vertex UV out of range')
                        if corner is not None and tuple(uv) == tuple(base_corners[corner][3:5]):
                            indices.append(face[corner])
                        else:
                            new_vertices[mesh_index].append(pm.VERTEX.pack(*position, *uv))
                            indices.append(('new', len(new_vertices[mesh_index])-1))
                    material = material_of[headers[face[4]]] if piece['composite'] else face[4]
                    pieces_out.append((*indices, *([0]*(4-len(indices))), material, *face[5:]))
                if piece['composite']:
                    out_faces.append(dict(face=origin[base], luma=light(base), base=list(headers[face[4]]),
                                          xy=piece['xy'], target=piece['target'], rect=piece.get('rect'),
                                          layers=item['layers']))
            replace[base] = pieces_out
            added += len(pieces_out)-1
            for f in pile['overlays']:
                drop.add(f)
                removed.append(dict(face=origin[f], under=origin[base], header=list(headers[faces[f][4]]),
                                    triangles=faces[f][5]-2))
        for row in material_rows:
            row['lumas'] = sorted({f['luma'] for f in out_faces if tuple(f['base']) == tuple(row['base'])})
        composites.append(dict(decal=name, source=entry['name'], header=material_rows[0]['header'], materials=material_rows,
                               size=[width, height], flip=flip >> 8, placement='texture' if in_texture else 'atlas',
                               palette=mode if mode != 'own' else 'own', cutout=cutout, faces=out_faces))
        report.append(dict(name=name, layers=entry['layers'], palette=mode, piles=len(chosen),
                           placement='texture' if in_texture else 'atlas', texture=[width, height],
                           texture_bytes=width*height//2, materials=len(material_rows),
                           faces_removed=len(removed), triangles_removed=sum(r['triangles'] for r in removed),
                           faces_added_by_cuts=added, rectangles=len({tuple(f['rect']) for f in out_faces if f['rect']}),
                           cut_uv_rounding_texels=uv_error, removed=removed, **note))
    if not replace:
        return [], dict(composites=report, piles_found=len(piles), left=dict(left))
    # Rebuild the tables: faces replaced or dropped, vertex records appended,
    # records no face uses any more removed, position references recomputed.
    new_faces, new_origin, new_meshes = [], [], []
    vertices_out, refs_out = bytearray(), []
    cut_faces = defaultdict(list)
    for mesh_index, mesh in enumerate(pack['meshes']):
        mesh = list(mesh)
        raw = [pack['vertices'][(mesh[0]+i)*16:(mesh[0]+i+1)*16] for i in range(mesh[1])]
        base_count = len(raw)
        appended = {}
        local = []
        for f in range(mesh[2], mesh[2]+mesh[3]):
            if f in drop:
                continue
            for face in replace.get(f, [faces[f]]):
                indices = []
                for value in face[:face[5]]:
                    if isinstance(value, tuple):
                        record = new_vertices[mesh_index][value[1]]
                        if record not in appended:
                            appended[record] = len(raw)
                            raw.append(record)
                        value = appended[record]
                    indices.append(value)
                local.append(((*indices, *([0]*(4-len(indices))), *face[4:]), origin[f], f in replace and len(replace[f]) > 1))
        used = sorted({i for face, _, _ in local for i in face[:face[5]]})
        if len(used) != len(raw):
            remap = {old: new for new, old in enumerate(used)}
            raw = [raw[i] for i in used]
            local = [((*[remap[i] for i in face[:face[5]]], *([0]*(4-face[5])), *face[4:]), source, cut)
                     for face, source, cut in local]
        if len(raw) > 65536:
            raise ValueError('a mesh exceeds 65536 vertices')
        positions = {}
        refs = [positions.setdefault(record[:12], i) for i, record in enumerate(raw)]
        first = len(new_faces)
        for face, source, cut in local:
            if cut:
                cut_faces[face[11]].append(struct.unpack('<3f', raw[face[2]][:12]))
            new_faces.append(face)
            new_origin.append(source)
        mesh[0], mesh[1], mesh[2], mesh[3] = len(refs_out), len(raw), first, len(local)
        vertices_out += b''.join(raw)
        refs_out += refs
        new_meshes.append(mesh)
    groups = bytearray(pack['groups'])
    enlarged = 0
    if groups and pm.GROUP.size == 64:
        # Facing spheres (split_cutouts.py): enclose corner 2 of the cut pieces.
        for group, points in cut_faces.items():
            values = list(pm.GROUP.unpack_from(groups, group*pm.GROUP.size))
            radius = max(math.dist(p, values[10:13]) for p in points)
            if radius > values[13]:
                values[13] = pm.outward_float(radius)
                pm.GROUP.pack_into(groups, group*pm.GROUP.size, *values)
                enlarged += 1
    pack['faces'], pack['origin'], pack['meshes'] = new_faces, new_origin, new_meshes
    pack['vertices'], pack['refs'] = bytes(vertices_out), struct.pack(f'<{len(refs_out)}H', *refs_out)
    pack['materials'], pack['groups'] = bytes(materials), bytes(groups)
    pack['header'][3], pack['header'][4], pack['header'][5] = len(refs_out), len(new_faces), len(materials)//size
    pack['decal_materials'] = decal_materials
    return composites, dict(composites=report, piles_found=len(piles), left=dict(left),
                            faces_removed=len(drop), faces_added_by_cuts=sum(r.get('faces_added_by_cuts', 0) for r in report),
                            vertices=len(refs_out), facing_spheres_enlarged=enlarged,
                            composite_materials=len(decal_materials))


def inherit_classes(pack, scenery_classes, reduced_headers, surface_ground):
    """prepare_face_order.scenery_classes for a pack with baked decals: every
    material is classed as if the composite faces still were faces of their
    base material; a composite material then takes its base's class word
    (scenery stage, road-surface depth class) and road-decal lift."""
    base = pack['decal_materials']
    faces = [face[:4]+(base.get(face[4], face[4]),)+face[5:] for face in pack['faces']]
    classes, decals, report = scenery_classes(dict(pack, faces=faces), reduced_headers, surface_ground)
    for new, old in base.items():
        classes[new] = classes[old]
        (decals.add if old in decals else decals.discard)(new)
    return classes, decals, dict(report, decal_materials=len(base))


# Texture side ---------------------------------------------------------------

def banks(decals):
    """Palette banks the decal composites need."""
    return len({c['decal'] if c['palette'] == 'own' else c['palette'] for c in decals if c['palette'] != 'ramp'})


def ramp_rows(decals):
    """Source rows (as prepare_palette_materials.read_pack) for the ramp
    composites: ordinary textured bindings, fitted like any other."""
    return [(tuple(m['header']), light, 0, 0, 0) for c in decals if c['palette'] == 'ramp'
            for m in c['materials'] for light in m['lumas']]


def _interpolate(source, values, points):
    """Per-triangle affine map of a triangle or fan quad: corner coordinates
    `source` (n x 2) carry `values` (n x k); result at `points` (m x 2), and
    the signed distance of each point to the polygon (positive inside)."""
    source, values = np.asarray(source, float), np.asarray(values, float)

    def affine(a, b, c):
        m = np.stack([source[b]-source[a], source[c]-source[a]], 1)
        weights = np.linalg.solve(m, (points-source[a]).T).T
        return values[a]+weights[:, :1]*(values[b]-values[a])+weights[:, 1:]*(values[c]-values[a])
    result = affine(0, 1, 2)
    if len(source) == 4:
        d = source[2]-source[0]

        def side(q):
            return d[0]*(q[..., 1]-source[0][1])-d[1]*(q[..., 0]-source[0][0])
        other = side(points)*side(source[1]) < 0
        if other.any():
            result = np.where(other[:, None], affine(0, 2, 3), result)
    area = 0.0
    for a, b in zip(source, np.roll(source, -1, 0)):
        area += a[0]*b[1]-a[1]*b[0]
    inside = np.full(len(points), np.inf)
    for a, b in zip(source, np.roll(source, -1, 0)):
        e = b-a
        length = np.hypot(*e)
        if length > 0:
            inside = np.minimum(inside, (e[0]*(points[:, 1]-a[1])-e[1]*(points[:, 0]-a[0]))/length*(1 if area > 0 else -1))
    return result, inside


def _wrap(value, size, mirror):
    value = np.floor(value).astype(np.int64)
    if mirror:
        value %= 2*size
        return np.where(value >= size, 2*size-1-value, value)
    return value % size


def render(decals, pal, luma, curve, sheets, tools):
    """Texels, palettes and errors of the decal composites.

    tools: quantize, texture_indices, exact_palette (prepare_palette_materials
    and prepare_palette_probe functions). Returns per composite name:
    index (H x W uint8), palette (16 x 3 or None for ramp), vertex {(header,
    luma): 0xffRRGGBB} (own palettes), report, and the pictures `exact` and
    `shown` (H x W x 3, the last face that wrote each texel) for previews."""
    images = {}

    def image(header):
        key = (header[2], _size(header))
        if key not in images:
            images[key] = tools['texture_indices'](sheets, header[2], *_size(header))
        return images[key]

    def colours(header, light):
        return tools['exact_palette'](pal, luma, curve, header, light).astype(float)

    work = {}
    for composite in decals:
        width, height = composite['size']
        mirror = (composite['flip'] & 1, composite['flip'] & 2)
        cutout, mode = composite['cutout'], composite['palette']
        in_texture = composite['placement'] == 'texture'
        # Gains (module text): per base header and luma.
        top = defaultdict(lambda: np.zeros(3))
        for face in composite['faces']:
            for layer in face['layers'][1:]:
                key = (tuple(face['base']), face['luma'])
                top[key] = np.maximum(top[key], colours(layer['header'], layer['luma'])[:15].max(0))
        base_top = {key: np.maximum(colours(key[0], key[1])[:15 if cutout else 16].max(0), 1.0) for key in top}
        factor = np.maximum(np.max([top[key]/base_top[key] for key in top], axis=0), 1.0)
        gain = {key: np.minimum(base_top[key]*factor, 255.0)/255.0 for key in top}
        samples = []
        for face in composite['faces']:
            target = np.array(face['target'], float)/8
            if in_texture:
                x0, y0 = np.floor(target.min(0)).astype(int)-1
                x1, y1 = np.ceil(target.max(0)).astype(int)+1
            else:
                x0, y0, w, h = face['rect']
                x1, y1 = x0+w, y0+h
            xs, ys = np.meshgrid(np.arange(x0, x1), np.arange(y0, y1))
            points = np.stack([xs.reshape(-1)+0.5, ys.reshape(-1)+0.5], 1)
            xy, inside = _interpolate(target, face['xy'], points)
            if in_texture:
                keep = inside >= -0.75
                points, xy = points[keep], xy[keep]
            px = _wrap(points[:, 0], width, mirror[0] if in_texture else 0)
            py = _wrap(points[:, 1], height, mirror[1] if in_texture else 0)
            exact = np.zeros((len(points), 3))
            hole = np.ones(len(points), bool)
            base_index = np.zeros(len(points), np.int64)
            from_base = np.zeros(len(points), bool)
            for depth, layer in enumerate(face['layers']):
                uv, within = _interpolate(layer['xy'], np.array(layer['uv'], float)/8, xy)
                texture = image(layer['header'])
                lw, lh = _size(layer['header'])
                index = texture[_wrap(uv[:, 1], lh, layer['header'][0] & 0x200), _wrap(uv[:, 0], lw, layer['header'][0] & 0x100)]
                opaque = np.ones(len(points), bool) if _kind(layer['header']) == 2 else index != 15
                if layer['clip']:
                    opaque &= within >= -1e-4
                exact[opaque] = colours(layer['header'], layer['luma'])[index[opaque]]
                hole &= ~opaque
                from_base = np.where(opaque, depth == 0, from_base)
                if depth == 0:
                    base_index = index.astype(np.int64)
            samples.append(dict(face=face, px=px, py=py, exact=exact, hole=hole, base_index=base_index,
                                from_base=from_base, key=(tuple(face['base']), face['luma'])))
        work[composite['decal']] = dict(composite=composite, samples=samples, gain=gain, factor=factor)
    # Texels.
    for item in work.values():
        composite = item['composite']
        width, height = composite['size']
        if composite['palette'] == 'ramp':
            cost = np.zeros((height, width, 16))
            forced = np.full((height, width), -1, np.int64)
            for s in item['samples']:
                ramp = colours(*s['key'])
                error = ((s['exact'][:, None, :]-ramp[None, :, :])**2).sum(2)
                np.add.at(cost, (s['py'], s['px']), error)
                forced[s['py'][s['from_base']], s['px'][s['from_base']]] = s['base_index'][s['from_base']]
            index = np.where(forced >= 0, forced, np.argmin(cost, 2))
            if composite['placement'] == 'texture':
                # Texels no face shows: the base's own.
                covered = np.zeros((height, width), bool)
                for s in item['samples']:
                    covered[s['py'], s['px']] = True
                base = image(composite['faces'][0]['layers'][0]['header'])
                index = np.where(covered, index, base)
            item['index'] = index.astype(np.uint8)
            item['palette'] = None
        else:
            total, count = np.zeros((height, width, 3)), np.zeros((height, width))
            holes = np.zeros((height, width))
            for s in item['samples']:
                value = np.minimum(s['exact']/item['gain'][s['key']], 255.0)
                solid = ~s['hole']
                np.add.at(total, (s['py'][solid], s['px'][solid]), value[solid])
                np.add.at(count, (s['py'][solid], s['px'][solid]), 1)
                np.add.at(holes, (s['py'][s['hole']], s['px'][s['hole']]), 1)
            item['texel'] = np.rint(total/np.maximum(count, 1)[..., None]).astype(np.int64)
            item['solid'] = (count > 0) & (count >= holes)
            item['hole'] = holes > count
    groups = defaultdict(list)
    for name, item in work.items():
        mode = item['composite']['palette']
        if mode != 'ramp':
            groups[name if mode == 'own' else mode].append(name)
    palettes = {}
    for group, names in groups.items():
        cutouts = {work[name]['composite']['cutout'] for name in names}
        if len(cutouts) != 1:
            raise ValueError(f'palette {group}: opaque and punch-through composites cannot share it')
        entries = 15 if cutouts.pop() else 16
        # Jointly: every composite counts alike, whatever its texture size.
        share = [work[name]['texel'][work[name]['solid']] for name in names]
        pixels = np.concatenate([p[::max(1, -(-len(p)//SHARED_SAMPLES))] for p in share]) if len(share) > 1 else share[0]
        palette, _ = tools['quantize'](pixels, entries)
        palette = np.clip(np.rint(palette), 0, 255)
        full = np.zeros((16, 3))
        full[:entries] = palette
        palettes[group] = full
        for name in names:
            item = work[name]
            flat = item['texel'].reshape(-1, 3).astype(float)
            nearest = np.concatenate([np.argmin(((flat[at:at+65536, None, :]-full[None, :entries, :])**2).sum(2), axis=1)
                                      for at in range(0, len(flat), 65536)])
            index = nearest.reshape(item['texel'].shape[:2])
            if entries == 15:
                index = np.where(item['hole'] | ~item['solid'], 15, index)
            item['index'], item['palette'], item['palette_group'] = index.astype(np.uint8), full, group
    # What is shown, and its error against the exact layered picture.
    result = {}
    for name, item in work.items():
        composite = item['composite']
        width, height = composite['size']
        exact_picture, shown_picture = np.zeros((height, width, 3)), np.zeros((height, width, 3))
        vertex, rows = {}, defaultdict(lambda: dict(texels=0, total=0.0, square=0.0, worst=0.0, above16=0, faces=0, holes_wrong=0))
        for s in item['samples']:
            index = item['index'][s['py'], s['px']]
            if item['palette'] is None:
                shown = colours(*s['key'])[index]
            else:
                value = np.clip(np.rint(item['gain'][s['key']]*255), 0, 255)
                vertex[s['key']] = 0xff000000 | int(value[0]) << 16 | int(value[1]) << 8 | int(value[2])
                shown = np.rint(item['palette'][index]*value/255)
            solid = ~s['hole']
            error = np.abs(shown-s['exact'])[solid]
            row = rows[s['key']]
            row['faces'] += 1
            row['texels'] += int(solid.sum())
            row['total'] += float(error.sum())/3
            row['square'] += float((error**2).sum())/3
            row['worst'] = max(row['worst'], float(error.max()) if error.size else 0.0)
            row['above16'] += int((error.max(1) > 16).sum()) if error.size else 0
            if composite['cutout']:
                row['holes_wrong'] += int(((index == 15) != s['hole']).sum())
            exact_picture[s['py'], s['px']] = np.where(s['hole'][:, None], (255.0, 0.0, 255.0), s['exact'])
            shown_picture[s['py'], s['px']] = np.where((index == 15)[:, None] & composite['cutout'], (255.0, 0.0, 255.0), shown)
        lumas = [dict(base=list(key[0]), luma=key[1], faces=row['faces'], texels=row['texels'],
                      vertex='%08x' % vertex[key] if key in vertex else None,
                      max_error=row['worst'], mean_error=row['total']/max(row['texels'], 1),
                      rms_error=math.sqrt(row['square']/max(row['texels'], 1)),
                      texels_off_by_more_than_16=row['above16']/max(row['texels'], 1),
                      **(dict(coverage_texels_wrong=row['holes_wrong']) if composite['cutout'] else {}))
                 for key, row in sorted(rows.items())]
        texels = sum(r['texels'] for r in lumas)
        report = dict(decal=name, header=composite['header'], placement=composite['placement'], size=composite['size'],
                      texture_bytes=width*height//2, palette=composite['palette'],
                      palette_colours=None if item['palette'] is None else
                      ['%02x%02x%02x' % tuple(int(v) for v in c) for c in item['palette']],
                      gain_factor=[float(v) for v in item['factor']], faces=len(item['samples']),
                      max_error=max(r['max_error'] for r in lumas),
                      mean_error=sum(r['mean_error']*r['texels'] for r in lumas)/max(texels, 1),
                      rms_error=math.sqrt(sum(r['rms_error']**2*r['texels'] for r in lumas)/max(texels, 1)),
                      texels_off_by_more_than_16=sum(r['texels_off_by_more_than_16']*r['texels'] for r in lumas)/max(texels, 1),
                      lumas=lumas)
        result[name] = dict(index=item['index'], palette=item['palette'], palette_group=item.get('palette_group'),
                            vertex=vertex, report=report, exact=exact_picture, shown=shown_picture)
    return result


def ramp_images(decals, baked):
    """{(texture word, size): index image} of the ramp composites, for the
    material packer's texture table."""
    return {(c['header'][2], tuple(c['size'])): baked[c['decal']]['index'] for c in decals if c['palette'] == 'ramp'}


def add_materials(decals, baked, weights, textures, images, custom_palettes, out_rows, reports):
    """Textures, palettes and binding rows of the composites with a palette
    of their own, in prepare_palette_materials.prepare's tables; all reports."""
    for composite in decals:
        result = baked[composite['decal']]
        reports.append(result['report'])
        if composite['palette'] == 'ramp':
            continue
        key = ('composite', 'decal:'+result['palette_group'], 0, 0, 1)
        custom_palettes[key] = result['palette']
        name = ('decal', composite['decal'])
        entry = textures.setdefault(name, {'index': len(textures), 'inverted': {False}, 'cutout': {composite['cutout']}})
        entry['size'] = tuple(composite['size'])
        images[name] = result['index']
        for material in composite['materials']:
            for light in material['lumas']:
                header = tuple(material['header'])
                if (header, light) not in weights:
                    raise ValueError(f'composite {composite["decal"]}: {header} luma {light} is not in the mesh')
                out_rows.append([header, light, entry['index'], 0, result['vertex'][tuple(material['base']), light], key,
                                 None, weights[header, light]])


# Program ----------------------------------------------------------------------

def _candidates(args):
    import prepare_face_order as pfo
    pack = pfo.read_pack(args.source.read_bytes())
    lumas = pfo.static_lumas(pack, args.static_light)
    headers = [struct.unpack_from('<4H', pack['materials'], m*pm.MATERIAL.size) for m in range(pack['header'][5])]
    piles, left = find_piles(pack, lambda p: pfo.find_layers(p))
    fill = Counter()
    frames = 1
    if args.fill:
        order = json.loads(args.fill[0].read_text())['new_to_old']
        lines = args.fill[1].read_text().splitlines()
        frames = int(lines[0])
        for line in lines[1:]:
            face, samples, _, _ = map(int, line.split())
            fill[order[face]] += samples
    groups = defaultdict(list)
    for pile in piles:
        groups[tuple(pile['words'][:1])+tuple(sorted(pile['words'][1:]))].append(pile)
    rows = []
    for words, members in groups.items():
        faces = pack['faces']
        overlays = [f for p in members for f in p['overlays']]
        rows.append(dict(
            name='+'.join(map(str, words)), layers=list(words), palette='own', enabled=False,
            piles=len(members), kinds=sorted({p['kind'] for p in members}),
            base_list='punch-through' if _kind(headers[faces[members[0]['base']][4]]) == 3 else 'opaque',
            base_headers=len({headers[faces[p['base']][4]] for p in members}),
            overlay_headers=len({headers[faces[f][4]] for f in overlays}),
            light_variants=len({(headers[faces[p['base']][4]], lumas[p['base']][0]) +
                                tuple((headers[faces[f][4]], lumas[f][0]) for f in p['overlays']) for p in members}),
            two_sided=sum(p['two_sided'] for p in members),
            faces_removable=len(overlays), triangles_removable=sum(faces[f][5]-2 for f in overlays),
            punch_through_samples_per_frame=round(sum(fill[f] for f in overlays)/frames, 1)))
    rows.sort(key=lambda r: (-r['punch_through_samples_per_frame'], -r['faces_removable'], r['name']))
    with args.output.open('x') as out:
        json.dump(dict(composites=rows, left=dict(left)), out, indent=1)
    for row in rows:
        print('%-22s piles %3d %-12s base %-13s headers %2d/%2d variants %3d faces -%3d  PT %8.1f/frame' % (
            row['name'], row['piles'], ','.join(row['kinds']), row['base_list'], row['base_headers'],
            row['overlay_headers'], row['light_variants'], row['faces_removable'], row['punch_through_samples_per_frame']))
    print('left:', dict(left))


def _preview(args):
    from PIL import Image
    import prepare_palette_materials as ppm
    from prepare_palette_probe import video, exact_palette
    decals = [c for c in json.loads(args.composites.read_text()) if c.get('decal')]
    pal, luma, sheets, curve = video(args.video)
    baked = render(decals, pal, luma, curve, sheets,
                   dict(quantize=ppm.quantize, texture_indices=ppm.texture_indices, exact_palette=exact_palette))
    args.output.mkdir()
    for name, result in baked.items():
        difference = np.clip(np.abs(result['exact']-result['shown'])*4, 0, 255)
        picture = np.concatenate([result['exact'], result['shown'], difference], 1).astype(np.uint8)
        scale = max(1, 768//picture.shape[1])
        Image.fromarray(picture).resize((picture.shape[1]*scale, picture.shape[0]*scale), Image.NEAREST).save(
            args.output/f'{name}.png')
        r = result['report']
        print('%-22s %4dx%-4d %-10s max %5.1f mean %5.2f rms %5.2f >16: %.4f' % (
            name, *r['size'], r['palette'], r['max_error'], r['mean_error'], r['rms_error'], r['texels_off_by_more_than_16']))
    (args.output/'errors.json').write_text(json.dumps([r['report'] for r in baked.values()], indent=1))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest='command', required=True)
    c = commands.add_parser('candidates', help='every pile of a source pack, grouped into composites: a specification to edit')
    c.add_argument('source', type=Path, help='validated DNM2/4/5 scenery pack in arcade face order')
    c.add_argument('output', type=Path, help='NEW specification JSON (every composite disabled)')
    c.add_argument('--static-light', type=float, nargs=3, default=(-0.45, -0.89, -0.45))
    c.add_argument('--fill', type=Path, nargs=2, metavar=('ORDER_JSON', 'FILL'),
                   help='order map of a pack and the simulator fill file recorded with it')
    c.set_defaults(run=_candidates)
    c = commands.add_parser('preview', help='exact | baked | 4x difference per composite, and errors.json')
    c.add_argument('composites', type=Path, help='composites JSON of prepare_face_order.py --bake-decals')
    c.add_argument('video', type=Path, help='reference video dump')
    c.add_argument('output', type=Path, help='NEW directory')
    c.set_defaults(run=_preview)
    args = parser.parse_args()
    args.run(args)
