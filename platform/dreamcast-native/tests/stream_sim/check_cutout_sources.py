"""Converted cutouts against their own source faces: covered texels and colours.

The stream reference (reference_stream.h) checks that the renderer submits the
pack it was given. This check is the other half for packs whose punch-through
faces were cut offline (tools/opaque_cores.py, tools/split_cutouts.py): it
compares the cut pack with the uncut one, face by face, from the shipped
files only (world.dnm and world.dnt of both asset directories; none of the
packers' code or textures).

usage: check_cutout_sources.py REF_ASSETS NEW_ASSETS REF_ORDER.json NEW_ORDER.json [SPEC.json]
  ORDER.json: prepare_face_order.py --order-map of each world.dnm (new_to_old
  source face indices; "merged": source faces joined into another's face).
  SPEC.json: the opaque_cores.py specification, if one was applied.

Per source face that is not byte for byte what it was:
- every face made from it keeps normal, group, sidedness, baked front and
  back luma; an opaque piece has the source's material record under the
  split header and the source's scenery class plus NM_SCENERY_POINT, and its
  binding the source binding's texture, palette and vertex colour for both
  lumas; opaque pieces lie in a higher layer than the punch-through faces
  left of the same source; a face that replaces several joined source faces
  ("merged") is checked against each of them and may reach beyond each;
- every vertex of a piece lies where the source triangle's own UV-to-position
  map puts its texture coordinate (largest deviation reported, in texels and
  model units), up to a whole number of texture periods per piece;
- 9 sample points per texel of the source's footprint (never nearer than 1/8
  texel to a texel edge), per fan triangle of the source: what the uncut pack
  draws there (texel of the old texture, hole or colour = palette entry x
  vertex colour) against what the cut pack draws (an opaque piece: the texel
  of the new texture whatever its entry; else a punch-through piece: that
  texel unless it is the hole). Counted per texture word:
    same        both nothing, or the same colour
    filled      nothing before, a colour now
    lost        a colour before, nothing now
    recoloured  two different colours                        (never allowed)
    exposed     an opaque piece shows a hole texel (entry 15) (never allowed)
  filled is allowed only on texels the specification fills, lost only for
  textures whose specification kind removes the punch-through face (outline,
  rects, bands: what lies beyond the outer lines); without a specification
  neither is allowed. Where samples are lost is listed per source footprint
  ("lost: ..."; a texel's 9 samples count once per distinct face shape).
- trunk variation (specification key trunk_variation): the faces the order
  map lists as varied trunks may show another trunk's texels at another
  width; they must lie in the source's plane, below the punch-through pieces
  of their tree, with an existing opaque binding, and the samples they cover
  are left out of the comparison (counted with the faces, not as lost).
- the texels of the two material packs differ exactly where the
  specification says (fill and gutter), each a former hole that now has an
  entry below 15; every other texture is identical.
Exit status 0 and a last line "PASS ..." only if all of that holds.
"""
from collections import Counter, defaultdict
import json
from pathlib import Path
import struct
import sys

import numpy as np


def read_mesh(path):
    """Tables of an aligned DNM6 pack, located as code/meshpack.c does."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'tools'))
    import prepare_models as pm
    d = Path(path).read_bytes()
    h = pm.HEADER.unpack_from(d)
    b = pm.table_offsets(h)
    faces = [struct.unpack_from('<5HBBI3fHBB', d, b[2]+f*32) for f in range(h[4])]
    meshes = [pm.MESH.unpack_from(d, b[0]+i*pm.MESH.size) for i in range(h[2])]
    first = {}
    for mesh in meshes:
        for f in range(mesh[2], mesh[2]+mesh[3]):
            first[f] = mesh[0]
    return dict(headers=[struct.unpack_from('<4H', d, b[3]+m*32) for m in range(h[5])],
                records=[d[b[3]+m*32+8:b[3]+m*32+24] for m in range(h[5])],
                classes=[struct.unpack_from('<I', d, b[3]+m*32+24)[0] for m in range(h[5])],
                faces=faces, first=first, vertices=[d[b[1]+i*16:b[1]+i*16+16] for i in range(h[3])])


def read_materials(path):
    """DNT6: bindings {(header, luma): (resource, argb, palette)}, palettes, index images."""
    d = Path(path).read_bytes()
    magic, size, resources, count, _, _, palettes, flags = struct.unpack_from('<4s7I', d)
    assert magic == b'DNT6' and size == len(d), 'expected a DNT6 pack'
    base = 32+16*resources
    table = base+20*count+((count+3) & ~3)
    bindings = {}
    for i in range(count):
        row = struct.unpack_from('<6HI', d, base+16*i)
        bindings[row[:4], row[4]] = (row[5], struct.unpack_from('<I', d, base+16*count+4*i)[0], d[base+20*count+i])
    pal = np.frombuffer(d, '<u4', 16*palettes, table).reshape(palettes, 16)
    images = []
    for r in range(resources):
        offset, length, width, height, _ = struct.unpack_from('<IIHHI', d, 32+16*r)
        mode = struct.unpack_from('<I', d, offset+16)[0]
        assert (mode >> 27) & 7 == 5, 'expected PAL4 textures'
        side = min(width, height)
        ys, xs = np.mgrid[0:height, 0:width]
        index = (xs//side+ys//side)*side*side
        bit = 0
        while (1 << bit) < side:
            index |= ((ys >> bit) & 1) << (2*bit) | ((xs >> bit) & 1) << (2*bit+1)
            bit += 1
        start = offset+32
        if mode >> 31:                      # a square mip chain ends with the base level
            start += 2+sum(level*level//2 for level in (2 << k for k in range(16)) if level < width)
        data = np.frombuffer(d, np.uint8, width*height//2, start)
        images.append(((data[index//2] >> np.where(index & 1, 4, 0)) & 15).astype(np.uint8))
    return bindings, pal, images


def colours(pal, palette, argb):
    """RGB of the 16 entries of a binding, as the PVR multiplies them."""
    out = np.zeros((16, 3), np.int64)
    for k, shift in enumerate((16, 8, 0)):
        out[:, k] = ((pal[palette] >> shift) & 255).astype(np.int64)*((argb >> shift) & 255)//255
    return out


def main(ref_assets, new_assets, ref_order, new_order, spec_path=None):
    R, S = read_mesh(Path(ref_assets)/'world.dnm'), read_mesh(Path(new_assets)/'world.dnm')
    RB, RP, RI = read_materials(Path(ref_assets)/'world.dnt')
    SB, SP, SI = read_materials(Path(new_assets)/'world.dnt')
    ref_map, new_map = json.loads(Path(ref_order).read_text()), json.loads(Path(new_order).read_text())
    spec = json.loads(Path(spec_path).read_text()) if spec_path else {}
    spec_textures = {int(word): entry for word, entry in spec.get('textures', {}).items()}
    problems = []
    # Textures: equal except the specification's texels.
    changed_texels = {}
    texture_of = {}
    for (header, luma), (resource, _, _) in RB.items():
        if resource != 65535:
            texture_of.setdefault(resource, set()).add(header[2])
    for resource, words in texture_of.items():
        new_resource = {SB[key][0] for key in RB if RB[key][0] == resource and key in SB}
        if len(new_resource) != 1:
            problems.append(f'texture resource {resource} is not one resource in the new pack')
            continue
        a, b = RI[resource], SI[new_resource.pop()]
        if a.shape != b.shape:
            problems.append(f'texture {sorted(words)} changed size')
            continue
        ys, xs = np.nonzero(a != b)
        if not len(ys):
            continue
        word = sorted(words)[0]
        changed_texels[word] = {(int(x), int(y)) for x, y in zip(xs, ys)}
        entry = spec_textures.get(word)
        listed = {(x, y): value for x, y, value in (entry.get('fill', [])+entry.get('gutter', []) if entry else [])}
        if len(words) != 1 or set(listed) != changed_texels[word] or any(a[y, x] != 15 or b[y, x] != v for (x, y), v in listed.items()):
            problems.append(f'texture {sorted(words)}: {len(ys)} texels differ, not exactly the {len(listed)} holes the specification fills')
    for word, entry in spec_textures.items():
        if entry.get('fill') or entry.get('gutter'):
            if word not in changed_texels:
                problems.append(f'texture {word}: the specification changes texels, the packs agree')
    ref_face = {origin: f for f, origin in enumerate(ref_map['new_to_old'])}
    pieces = defaultdict(list)
    for f, origin in enumerate(new_map['new_to_old']):
        pieces[origin].append(f)
    joined = set()
    for keeper, absorbed in new_map.get('merged', {}).items():
        joined.add(int(keeper))
        for origin in absorbed:
            pieces[origin] = pieces[int(keeper)]
            joined.add(origin)
    stats, memo = defaultdict(Counter), {}
    lost_where = defaultdict(lambda: defaultdict(Counter))    # word -> source footprint -> texel -> samples lost (per face shape)
    varied_faces = set(new_map.get('foreign_trunks', []))
    worst = dict(texels=0.0, units=0.0)

    def corners(pack, f):
        face = pack['faces'][f]
        return [struct.unpack('<3f2H', pack['vertices'][pack['first'][f]+i]) for i in face[:face[5]]]

    for origin, rf in ref_face.items():
        rface, rh = R['faces'][rf], R['headers'][R['faces'][rf][4]]
        rc = corners(R, rf)
        made = pieces.get(origin, [])
        word = rh[2]
        if len(made) == 1 and corners(S, made[0]) == rc:
            sface = S['faces'][made[0]]
            if S['headers'][sface[4]] != rh or sface[5:12] != rface[5:12] or sface[13] != rface[13] or \
                    S['classes'][sface[4]] != R['classes'][rface[4]]:
                problems.append(f'source face {origin} kept its corners but not its material or attributes')
            stats[word]['faces_unchanged'] += 1
            continue
        if (rh[0] >> 13) & 7 != 3:
            problems.append(f'source face {origin} is not punch-through and changed')
            continue
        if not rface[7] & (3 << 8):
            if made:
                problems.append(f'source face {origin} is never drawn and changed')
            continue
        stats[word]['faces_changed'] += 1
        width, height = 32 << (rh[0] & 7), 32 << ((rh[0] >> 3) & 7)
        period = (8*width, 8*height)
        fans = [(0, i, i+1) for i in range(1, rface[5]-1)]
        uv = np.array([c[3:] for c in rc], float)
        pos = np.array([c[:3] for c in rc], float)
        shapes, layers = [], defaultdict(list)
        for f in made:
            sface, sh = S['faces'][f], S['headers'][S['faces'][f][4]]
            sc = corners(S, f)
            opaque = sh != rh
            varied = f in varied_faces
            layers['varied' if varied else opaque].append(sface[12])
            if sface[6] != rface[6] or (sface[7] & ~3) != (rface[7] & ~3) or sface[8:12] != rface[8:12] or sface[13] != rface[13]:
                problems.append(f'source face {origin}: a piece differs in luma, sidedness, normal or group')
            if varied:
                # A varied trunk: another trunk's texels or another width, by the specification. Its binding must exist.
                if (sh[0] >> 13) & 7 != 2 or sh[1] & 0xff00 != 0x2000 or any((sh, luma) not in SB for luma in {rface[6], rface[13]}):
                    problems.append(f'source face {origin}: a varied trunk has no opaque split binding')
                if not spec.get('trunk_variation'):
                    problems.append(f'source face {origin}: a varied trunk without trunk variation in the specification')
            elif opaque:
                if sh != (rh[0] & ~0x2000, 0x2000 | rh[1], rh[2], rh[3]) or S['records'][sface[4]] != R['records'][rface[4]] or \
                        S['classes'][sface[4]] != (R['classes'][rface[4]] | 0x200):
                    problems.append(f'source face {origin}: opaque piece material is not the split image of {rh}')
                for luma in {rface[6]} | ({rface[13]} if rface[7] & (1 << 17) else set()):
                    old, new = RB.get((rh, luma)), SB.get((sh, luma))
                    if not old or not new or old[1] != new[1] or (RP[old[2]] != SP[new[2]]).any() or \
                            texture_of.get(old[0]) != {rh[2]}:
                        problems.append(f'source face {origin}: binding of the opaque piece differs from the source at luma {luma}')
            elif S['classes'][sface[4]] != R['classes'][rface[4]]:
                problems.append(f'source face {origin}: punch-through piece changed class')
            # Where the piece is, in the source's texture coordinates: through the source triangle that holds it.
            p = np.array([c[:3] for c in sc], float)
            t = np.array([c[3:] for c in sc], float)
            best = None
            for fan in fans:
                a, e1, e2 = pos[fan[0]], pos[fan[1]]-pos[fan[0]], pos[fan[2]]-pos[fan[0]]
                g = np.array([[e1@e1, e1@e2], [e1@e2, e2@e2]])
                w = np.linalg.solve(g, np.array([(p-a)@e1, (p-a)@e2]))            # 2 x n barycentric weights
                off = np.abs((a+np.outer(w[0], e1)+np.outer(w[1], e2))-p).max()
                outside = max(0.0, float(-(w.min())), float((w.sum(0)-1).max()))
                where = uv[fan[0]]+np.outer(w[0], uv[fan[1]]-uv[fan[0]])+np.outer(w[1], uv[fan[2]]-uv[fan[0]])
                # A piece belongs to the source triangle whose plane and mapping it follows (the two
                # triangles of an affine quad have the same); it must lie inside the source polygon.
                shift = np.round((t-where).mean(0)/period)*period
                turn = np.sign(sum(uv[i-1][0]*uv[i][1]-uv[i][0]*uv[i-1][1] for i in range(len(uv))))
                beyond = 0.0
                for i in range(len(uv)):
                    a2, b2 = uv[i-1], uv[i]
                    length = max(float(np.hypot(*(b2-a2))), 1e-9)
                    beyond = max(beyond, float((-turn*((b2[0]-a2[0])*(where[:, 1]-a2[1])-(b2[1]-a2[1])*(where[:, 0]-a2[0]))/length).max()))
                if origin in joined:
                    beyond = 0.0        # a face joined with its neighbours reaches beyond this source, in its plane and mapping
                rank = (beyond > 0.51 or off > 1e-2, float(np.abs(t-shift-where).max()), outside, off)
                if best is None or rank < best[0]:
                    best = (rank, off, where, shift)
            moved = t-uv if len(sc) == len(rc) and [c[:3] for c in sc] == [c[:3] for c in rc] else None
            if moved is not None and (moved == moved[0]).all() and not (moved[0] % period).any():
                # The source face itself (kept under opaque pieces, or its texture coordinates
                # moved by whole periods): its own corners and triangles.
                best = ((False, 0.0, 0.0, 0.0), 0.0, uv, moved[0])
                stats[word]['faces_moved_by_whole_periods'] += bool(moved[0].any())
            shift = best[3]
            if varied:
                shapes.append(('varied', tuple(map(tuple, np.round(best[2]).astype(int))), ()))
                stats[word]['varied_trunk_faces'] += 1
                stats[word]['opaque_triangles'] += sface[5]-2
                if best[0][0]:
                    problems.pop()
                if best[1] > 1e-2:
                    problems.append(f'source face {origin}: a varied trunk is off its source plane')
                continue
            if best[0][1]/8 > worst['texels']:
                worst['face'] = origin
            worst['texels'] = max(worst['texels'], best[0][1]/8)
            worst['units'] = max(worst['units'], float(best[1]))
            if best[0][0]:
                problems.append(f'source face {origin}: a piece does not lie in its source polygon')
            shapes.append((opaque, tuple(map(tuple, (t-shift).astype(int))), tuple(map(tuple, t.astype(int)))))
            stats[word]['opaque_triangles' if opaque else 'punch_through_triangles'] += sface[5]-2
            stats[word]['opaque_faces' if opaque else 'punch_through_faces'] += 1
        if layers['varied'] and layers[False] and max(layers['varied']) >= min(layers[False]):
            problems.append(f'source face {origin}: a varied trunk is not below the punch-through pieces')
        if layers[True] and layers[False] and min(layers[True]) <= max(layers[False]):
            problems.append(f'source face {origin}: an opaque piece is not above the punch-through faces under it')
        key = (rh[0] & 0x33f, word, tuple(map(tuple, uv.astype(int))), tuple(sorted(shapes, key=repr)), rface[6],
               rface[13] if rface[7] & (1 << 17) else None)
        if key not in memo:
            old_binding = RB[rh, rface[6]]
            new_pt = SB.get((rh, rface[6]))
            new_op = SB.get(((rh[0] & ~0x2000, 0x2000 | rh[1], rh[2], rh[3]), rface[6]))
            old_image = RI[old_binding[0]]
            new_image = SI[(new_op or new_pt or old_binding)[0]]
            old_rgb = colours(RP, old_binding[2], old_binding[1])
            new_rgb = {True: colours(SP, new_op[2], new_op[1]) if new_op else None,
                       False: colours(SP, new_pt[2], new_pt[1]) if new_pt else None}
            x0, x1 = int(uv[:, 0].min()//8), int(-(-uv[:, 0].max()//8))
            y0, y1 = int(uv[:, 1].min()//8), int(-(-uv[:, 1].max()//8))
            sub = np.array([1, 4, 7])
            X, Y = np.meshgrid((8*np.arange(x0, x1)[:, None]+sub).reshape(-1), (8*np.arange(y0, y1)[:, None]+sub).reshape(-1))

            def inside(tri, strict):
                area = (tri[1][0]-tri[0][0])*(tri[2][1]-tri[0][1])-(tri[1][1]-tri[0][1])*(tri[2][0]-tri[0][0])
                if not area:
                    return np.zeros(X.shape, bool)
                mask = np.ones(X.shape, bool)
                for i in range(3):
                    a, b = tri[i], tri[(i+1) % 3]
                    edge = ((b[0]-a[0])*(Y-a[1])-(b[1]-a[1])*(X-a[0]))*(1 if area > 0 else -1)
                    mask &= edge > 0 if strict else edge >= 0
                return mask
            foot = np.zeros(X.shape, bool)
            for fan in fans:
                foot |= inside([uv[k] for k in fan], True)
            flip = (rh[0] >> 8) & 3

            def texel(image, U, V):
                tx, ty = np.floor(U/8).astype(int), np.floor(V/8).astype(int)
                if flip & 1:
                    tx = tx % (2*width)
                    tx = np.where(tx >= width, 2*width-1-tx, tx)
                if flip & 2:
                    ty = ty % (2*height)
                    ty = np.where(ty >= height, 2*height-1-ty, ty)
                return image[ty % height, tx % width]
            before = texel(old_image, X, Y)
            now = np.full(X.shape, -1)          # palette entry drawn, -1 nothing
            by_opaque = np.zeros(X.shape, bool)
            off_texel = 0
            varied_mask = np.zeros(X.shape, bool)
            for kind, where, own in shapes:
                if kind == 'varied':
                    for i in range(1, len(where)-1):
                        varied_mask |= inside([where[0], where[i], where[i+1]], False)
            for opaque in (False, True):        # opaque pieces lie on top
                for kind, where, own in shapes:
                    if kind != opaque:
                        continue
                    for i in range(1, len(where)-1):
                        tri = [where[0], where[i], where[i+1]]
                        cover = inside(tri, False) & foot
                        if not cover.any():
                            continue
                        # The piece's own texture coordinates at the samples (affine in its triangle).
                        A = np.linalg.solve(np.array([[tri[0][0], tri[0][1], 1.0], [tri[1][0], tri[1][1], 1.0], [tri[2][0], tri[2][1], 1.0]]),
                                            np.array([own[0], own[i], own[i+1]], float))
                        U = X*A[0, 0]+Y*A[1, 0]+A[2, 0]
                        V = X*A[0, 1]+Y*A[1, 1]+A[2, 1]
                        entry = texel(new_image, U, V)
                        # ... must be the source's texel there, up to whole periods.
                        off_texel += int((cover & ((np.floor(U/8) % width != np.floor(X/8) % width) |
                                                   (np.floor(V/8) % height != np.floor(Y/8) % height))).sum())
                        if opaque:
                            now = np.where(cover, entry, now)
                            by_opaque |= cover
                        else:
                            now = np.where(cover & (entry != 15) & ~by_opaque, entry, now)
            foot = foot & ~varied_mask        # under a varied trunk nothing is compared: it is meant to differ
            drawn_before = foot & (before != 15)
            drawn_now = foot & (now >= 0)
            exposed = foot & by_opaque & (now == 15)
            both = drawn_before & drawn_now & ~exposed
            recoloured = both
            if both.any():
                rgb_now = np.where(by_opaque[..., None], (new_rgb[True] if new_op else old_rgb)[np.clip(now, 0, 15)],
                                   (new_rgb[False] if new_pt else old_rgb)[np.clip(now, 0, 15)])
                recoloured = both & (old_rgb[before] != rgb_now).any(-1)
            filled = foot & ~drawn_before & drawn_now & ~exposed
            texels = {(int(x) % width, int(y) % height) for x, y in zip(X[filled]//8, Y[filled]//8)}
            memo[key] = Counter(samples=int(foot.sum()), drawn_before=int(drawn_before.sum()),
                                same=int((foot & ~drawn_before & ~drawn_now).sum()+(both & ~recoloured).sum()),
                                filled=int(filled.sum()), lost=int((drawn_before & ~drawn_now).sum()),
                                recoloured=int(recoloured.sum()), exposed=int(exposed.sum()), other_texel=off_texel,
                                on_opaque_list=int((foot & by_opaque).sum())), texels, \
                Counter((int(x) % width, int(y) % height) for x, y in zip(X[drawn_before & ~drawn_now]//8, Y[drawn_before & ~drawn_now]//8))
        counts, texels, gone = memo[key]
        stats[word].update(counts)
        if gone:
            lost_where[word][tuple(map(tuple, uv.astype(int)))].update(gone)
        if texels-changed_texels.get(word, set()):
            problems.append(f'source face {origin}: filled samples on texels the material packs do not change')
    never = ('recoloured', 'exposed', 'other_texel')
    print('texture   faces changed | samples   drawn before      same    filled      lost recoloured exposed | now opaque list  PT tris  OP tris')
    total = Counter()
    for word, row in sorted(stats.items(), key=lambda item: -item[1]['samples']):
        total.update(row)
        if not row['faces_changed']:
            continue
        kind = spec_textures.get(word, {}).get('kind')
        print('%7d %6d %7d | %9d %9d %9d %9d %9d %7d %7d | %5.1f%% of drawn %8d %8d' % (
            word, row['faces_changed']+row['faces_unchanged'], row['faces_changed'], row['samples'], row['drawn_before'], row['same'],
            row['filled'], row['lost'], row['recoloured'], row['exposed'],
            100*row['on_opaque_list']/max(row['drawn_before']-row['lost']+row['filled'], 1), row['punch_through_triangles'], row['opaque_triangles']))
        for name in never:
            if row[name]:
                problems.append(f'texture {word}: {row[name]} samples {name}')
        if row['lost'] and kind not in ('outline', 'rects', 'bands'):
            problems.append(f'texture {word}: {row["lost"]} samples lost, and its punch-through face was to stay')
        if row['filled'] and word not in spec_textures:
            problems.append(f'texture {word}: {row["filled"]} samples filled without a specification')
    for word, places in sorted(lost_where.items()):
        for footprint, texels in sorted(places.items()):
            print('lost: texture %d, source footprint %s: %d texels, samples per texel (x, y): %s' % (
                word, list(footprint), len(texels), ', '.join('(%d, %d) %d' % (x, y, n) for (x, y), n in sorted(texels.items())[:24])))
    print('faces unchanged %d, changed %d; largest distance of a piece vertex from its place on the source face: %.4f texels, %.2e model units'
          % (total['faces_unchanged'], total['faces_changed'], worst['texels'], worst['units']))
    if worst['texels'] > 0.07 or worst['units'] > 0.02:
        problems.append(f'a piece vertex is off the source face\'s texture mapping (worst at source face {worst.get("face")})')
    for line in problems[:20]:
        print('FAIL:', line)
    if problems:
        return 1
    print('PASS: every changed face draws its source\'s texels in its source\'s colours (filled %d, lost %d of %d samples)'
          % (total['filled'], total['lost'], total['samples']))
    return 0


if __name__ == '__main__':
    if len(sys.argv) not in (5, 6):
        raise SystemExit(__doc__)
    sys.exit(main(*sys.argv[1:]))
