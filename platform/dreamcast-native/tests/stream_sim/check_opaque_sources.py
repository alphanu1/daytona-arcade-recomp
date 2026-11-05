"""Punch-through faces moved to the opaque list (tools/pt_to_opaque.py) against their own source faces.

usage: check_opaque_sources.py REF_ASSETS NEW_ASSETS REF_ORDER.json[,LOD_ORDER.json] NEW_ORDER.json[,LOD_ORDER.json]
  ASSETS: world.dnm and world.dnt of each set (the shipped files only; none of the packers' code or textures).
  ORDER: prepare_face_order.py --order-map of the pack, or "STRIPS,LOD" (prepare_restrip.py's map of world.dnm onto
  the level-of-detail pack, then that pack's map onto source faces).

Per source face of REF_ASSETS:
- unchanged: the same corners, material header, class, attributes and lumas in NEW_ASSETS;
- converted: every face NEW_ASSETS makes of it has the converted header of the source's (pt_to_opaque.converted_header:
  opaque list, CONVERTED_MARK, same texture word and colour word), the source's scenery class, normal, group,
  sidedness and baked lumas; every corner lies where the source fan triangle's own UV-to-position map puts its texture
  coordinate (largest deviation reported); for each luma it shows, its binding draws the source binding's texture
  (texel for texel), palette and vertex colour; the texels its polygon covers hold no transparent entry; a face
  that is the source polygon unchanged reaches none either with the filter's half texel (pt_to_opaque.reach); a
  face cut smaller covers every opaque texel the source covered (lost = 0);
- anything else (another option, e.g. a baked decal) is counted per texture word under "other" and is not judged.
Exit status 0 and a last line "PASS ..." only if no converted face fails.
"""
from collections import Counter, defaultdict
import json
from pathlib import Path
import struct
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'tools'))
from check_cutout_sources import read_mesh, read_materials   # noqa: E402
import pt_to_opaque as po                                    # noqa: E402


def order_of(argument):
    paths = argument.split(',')
    maps = [json.loads(Path(p).read_text())['new_to_old'] for p in paths]
    out = maps[0]
    for inner in maps[1:]:
        out = [inner[f] for f in out]
    return out


def main(ref_assets, new_assets, ref_order, new_order):
    R, S = read_mesh(Path(ref_assets)/'world.dnm'), read_mesh(Path(new_assets)/'world.dnm')
    RB, RP, RI = read_materials(Path(ref_assets)/'world.dnt')
    SB, SP, SI = read_materials(Path(new_assets)/'world.dnt')
    ref_map, new_map = order_of(ref_order), order_of(new_order)
    ref_faces, new_faces = defaultdict(list), defaultdict(list)
    for f, source in enumerate(ref_map):
        ref_faces[source].append(f)
    for f, source in enumerate(new_map):
        new_faces[source].append(f)
    problems, stats, other = [], Counter(), Counter()
    worst = 0.0

    def corners(pack, f):
        face = pack['faces'][f]
        return [struct.unpack('<3f2H', pack['vertices'][pack['first'][f]+i]) for i in face[:face[5]]]

    def same(fa, fb):
        a, b = R['faces'][fa], S['faces'][fb]
        return corners(R, fa) == corners(S, fb) and R['headers'][a[4]] == S['headers'][b[4]] and \
            R['classes'][a[4]] == S['classes'][b[4]] and a[5:12] == b[5:12] and a[13] == b[13]

    for source, made_ref in ref_faces.items():
        made_new = new_faces.get(source, [])
        if len(made_ref) == len(made_new) and all(same(a, b) for a, b in zip(made_ref, made_new)):
            stats['unchanged_source_faces'] += 1
            continue
        rf = made_ref[0]
        rface, rh = R['faces'][rf], R['headers'][R['faces'][rf][4]]
        if not made_new or any(po.converted_base(S['headers'][S['faces'][f][4]]) != rh for f in made_new) or len(made_ref) != 1:
            other[rh[2]] += 1
            continue
        stats['converted_source_faces'] += 1
        stats['converted_faces'] += len(made_new)
        rc = corners(R, rf)
        width, height = 32 << (rh[0] & 7), 32 << ((rh[0] >> 3) & 7)
        flip = (rh[0] >> 8) & 3
        source_polys = []
        for fan in po.fans_of(rc):
            order, turn = po._ccw(rc, fan)
            source_polys.append((order, [(rc[k][3], rc[k][4]) for k in order]))
        lumas = {rface[6]} | ({rface[13]} if rface[7] & (1 << 17) else set())
        for f in made_new:
            face = S['faces'][f]
            if S['classes'][face[4]] != R['classes'][rface[4]] or face[6] != rface[6] or face[13] != rface[13] or \
                    face[8:11] != rface[8:11] or face[11] != rface[11] or (face[7] ^ rface[7]) & ~3:
                problems.append(f'source face {source}: class, luma, normal, group or attributes changed')
            for luma in lumas:
                old = RB.get((rh, luma))
                new = SB.get((S['headers'][face[4]], luma))
                if old is None or new is None:
                    problems.append(f'source face {source}: no binding for luma {luma}')
                    continue
                if not np.array_equal(RI[old[0]], SI[new[0]]) or old[1] != new[1] or \
                        not np.array_equal(RP[old[2]], SP[new[2]]):
                    problems.append(f'source face {source}: luma {luma} draws another texture, colour or palette')
            # Corners on the source's own UV-to-position map.
            for c in corners(S, f):
                for order, poly in source_polys:
                    if po._point_in(poly, (c[3], c[4])):
                        tri = [rc[k] for k in order]
                        det = (tri[1][3]-tri[0][3])*(tri[2][4]-tri[0][4])-(tri[1][4]-tri[0][4])*(tri[2][3]-tri[0][3])
                        best = None
                        for second in range(1, len(order)-1):
                            t = [rc[order[0]], rc[order[second]], rc[order[second+1]]]
                            d = (t[1][3]-t[0][3])*(t[2][4]-t[0][4])-(t[1][4]-t[0][4])*(t[2][3]-t[0][3])
                            w1 = ((c[3]-t[0][3])*(t[2][4]-t[0][4])-(c[4]-t[0][4])*(t[2][3]-t[0][3]))/d
                            w2 = ((t[1][3]-t[0][3])*(c[4]-t[0][4])-(t[1][4]-t[0][4])*(c[3]-t[0][3]))/d
                            if w1 >= -1e-9 and w2 >= -1e-9 and w1+w2 <= 1+1e-9:
                                best = [t[0][k]+w1*(t[1][k]-t[0][k])+w2*(t[2][k]-t[0][k]) for k in range(3)]
                                break
                        if best is not None:
                            deviation = max(abs(best[k]-c[k]) for k in range(3))
                            worst = max(worst, deviation)
                            if deviation > 1e-3:
                                problems.append(f'source face {source}: a corner is {deviation:.4g} units off the source map')
                        break
                else:
                    problems.append(f'source face {source}: a corner UV lies outside the source polygon')
        # Texels: covered by the new faces (no filter) never transparent; uncut: none within the filter's reach;
        # cut: every opaque texel the source covered still covered.
        image = SI[SB[(S['headers'][S['faces'][made_new[0]][4]], rface[6])][0]] if \
            (S['headers'][S['faces'][made_new[0]][4]], rface[6]) in SB else None
        if image is None:
            continue
        hole = image == 15
        new_polys = []
        for f in made_new:
            nc = corners(S, f)
            for fan in po.fans_of(nc):
                order, turn = po._ccw(nc, fan)
                new_polys.append([(nc[k][3], nc[k][4]) for k in order])
        covered_new, covered_old = set(), set()
        for poly in new_polys:
            xs, ys = po.touched(poly)
            covered_new |= set(zip(po._wrap(xs, width, flip & 1).tolist(), po._wrap(ys, height, flip & 2).tolist()))
        for _, poly in source_polys:
            xs, ys = po.touched(poly)
            covered_old |= set(zip(po._wrap(xs, width, flip & 1).tolist(), po._wrap(ys, height, flip & 2).tolist()))
        if any(hole[y, x] for x, y in covered_new):
            problems.append(f'source face {source} (texture {rh[2]}): an opaque face covers a transparent texel')
        lost = sum(1 for x, y in covered_old-covered_new if not hole[y, x])
        stats['lost_opaque_texels'] += lost
        if lost:
            problems.append(f'source face {source}: {lost} opaque texels no longer drawn')
        uncut = sorted(sorted(p) for p in new_polys) == sorted(sorted(p) for _, p in source_polys)
        stats['uncut' if uncut else 'cut'] += 1
        if uncut:
            reached = 0
            for poly in new_polys:
                xs, ys = po.reach(poly)
                reached += int(hole[po._wrap(ys, height, flip & 2), po._wrap(xs, width, flip & 1)].sum())
            if reached:
                problems.append(f'source face {source} (texture {rh[2]}): the filter reaches {reached} transparent texels')
    for problem in problems[:40]:
        print('FAIL', problem)
    print(json.dumps(dict(stats=dict(stats), other_by_texture_word={str(k): v for k, v in sorted(other.items())},
                          largest_corner_deviation_units=worst, problems=len(problems))))
    if problems:
        return 1
    print('PASS converted faces: %d source faces (%d faces), same texels, bindings and lumas; %d unchanged; %d other' % (
        stats['converted_source_faces'], stats['converted_faces'], stats['unchanged_source_faces'], sum(other.values())))
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    sys.exit(main(*sys.argv[1:]))
