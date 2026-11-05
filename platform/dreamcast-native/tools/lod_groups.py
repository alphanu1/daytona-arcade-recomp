"""Level-of-detail groups for scenery cutouts (meshpack.h, NM_PACK_LOD_GROUPS).

Used by  prepare_face_order.py --lod-groups LOD.json        (the mesh pack)
and by   prepare_palette_materials.py --texture-fill LOD.json  (its textures).

A group is one source polygon drawn as one of up to three alternative shapes
("levels", nearest first). The renderer draws exactly one of them per frame,
chosen by the view-space depth of the polygon's centre against the group's
switch depths, with hysteresis. Each level is what an opaque-cores
specification (opaque_cores.py) makes of the source polygon, the source
polygon itself, or nothing.

LOD.json:
  {"reference_focus": 280,          optional, must be 280 (meshpack.h)
   "expected_focus": 207,           optional: the vertical focus the scene really
                                    uses; only "reach" needs it (below)
   "cores": {"spec": "FILE.json", "textures": [3924]},
                                    optional: an opaque_cores specification (or
                                    inline) applied as --opaque-cores would be, to
                                    the listed textures, none of them in a rule:
                                    cutouts that get one shape for every distance;
                                    or a list of such entries, their textures
                                    merged (other keys of the files must agree)
   "rules": [
     {"name": "trees",
      "textures": [3600, 2600],     punch-through materials with these texture
                                    words (material header word 2), or
      "checker": true,              the screen-checker panels (the catch fence
                                    that --stipple-panels turns into stipple)
      "levels": [LEVEL, LEVEL, ...],    one to three
      "switch": [SWITCH, ...]}]}        one less than levels
  LEVEL:  {"spec": "FILE.json"}     an opaque_cores specification (path relative
                                    to LOD.json) or {"spec": {...}} inline; only
                                    its entries for the rule's textures are used
          "source"                  the source polygon as it is
          "none"                    nothing is drawn at this level
          {"stipple": CELL}         checker panels only: the panel with a sparser
                                    stipple (see below). CELL is "quarter",
                                    "eighth" or a list of equal strings, "X" an
                                    opaque texel and "." a hole
  SWITCH: {"depth": D}              view-space depth at reference focus 280
          {"pixels": P, "of": "height"}   the depth at which the source polygon
                                    is P pixels of a 640x480 picture: tall
                                    ("height": its vertical extent, the default),
                                    wide ("width": horizontal extent) or long
                                    ("longest": its largest corner distance).
                                    Derived per polygon: trees differ in size.
          {"texel_pixels": P}       the depth at which one texel of the polygon's
                                    texture is P pixels (texels per unit from the
                                    polygon's own mapping, or "texels_per_unit")
          any of them with "reach": "radius": the polygon's largest corner
                                    distance from its centre is added, so the
                                    nearer level is still drawn while ANY part
                                    of the polygon is nearer than the depth
                                    (long fence panels seen along the fence).
                                    The renderer scales a switch depth by
                                    focus/280; the radius is a length, so it is
                                    stored times 280/expected_focus.
A picture of 640x480 shows the 496x384 logical screen: a length h at depth z
covers h*focus*(480/384)/z pixels vertically.

Texture variants. A level's specification may change texels (fill, gutter).
The texture of level 0 is the texture every user of that texture word sees,
as with a plain --opaque-cores pack. A further level whose texel changes
differ gets a copy of the texture: its materials carry the copy's number
(1..3) in bits 8..9 of header word 1 (VARIANT_MASK), next to SPLIT_MARK for
opaque pieces. The material packer gives such a header a second PAL4 texture
under the palette, bank and vertex colour of the header without the number:
no new palette, no new bank.

Stipple variants. The catch fence (--stipple-panels) is a punch-through panel
whose texture is a checkerboard: half the texels are holes. A {"stipple": CELL}
level draws the same quad, with the same texture coordinates, with another
synthetic texture of the same size and the same palette entry: CELL repeated
over the texture. A cell may only be opaque where the checkerboard is (x+y
even), so the net of the nearer level thins in place: every texel the sparser
level shows, the full one shows at the same place on the panel. "quarter" keeps
one texel of each 2x2 cell (the checkerboard's opaque texels on every other
row: a square net of twice the pitch), "eighth" every other one of those (the
checkerboard again at twice its size, 2 texels of 16). The level's material is
the checker material with the variant number (1..3) in bits 8..9 of header
word 1, which stipple_header() keeps next to the stipple mark; the material
packer gives it the texture and the binding of the plain stipple header (its
palette, bank and vertex colour): no new palette, no new bank.

Catch-fence sweep (renderer, make NATIVE_FENCE_SWEEP=1; the default is 0 since
15 October 2026, evening, when the recommended set became ssfar4f, which has no
checker rule: the plain fence, every panel at full density at every distance;
include/render/models/model_submission.h, sq_sweep_select). The checker rule's
groups (every level the same quad, stipple or "none") are not switched panel by
panel there: switch n of the rule becomes one plane of view-space depth for the
whole fence, the least switch depth n of its groups (so with "reach": "radius"
the texel depth plus the smallest panel's radius), and a panel across a plane
is drawn clipped: its part in front of plane n at level n, the part beyond at
level n+1 (nothing beyond the last plane when the last level is "none"). The
boundary therefore moves continuously with the camera. Nothing changes in the
pack: the same file serves a build with NATIVE_FENCE_SWEEP=0 (the default), which switches
whole panels by their centres with hysteresis as before. Switches of a checker
rule must increase by more than a quarter unit (four of the renderer's
margins) or the renderer keeps whole-panel switching.

Super-sampled far levels. A rule with "supersample" names opaque or
punch-through textured faces of any stage (the road surface included: these
rules ignore the reduced-view exclusion) and has the levels
["source", "supersample"]: level 0 is the face as it is, level 1 the same
face under a material variant whose header word 1 carries SUPERSAMPLE_MARK
0x1000 (no other mark uses bit 12 of word 1: 0x8000 stipple, 0x4000
composite, 0x2000 split, 0x0300 variant numbers). The renderer sets the TSP
super-sample bit for such a header (model_renderer.c, compile_draw_header,
NM_SUPERSAMPLE_FAR); the material packer gives it the binding of the header
without the mark (same texture, palette, bank and colour), and the scenery
class of its source material (prepare_face_order.py scenery_classes). Only
the header word differs, so where the PVR draws level 1 the picture is level
0's, sampled four times a pixel.
  {"name": "asphalt",
   "textures": [7168, 6656],        textured faces with these texture words
   "supersample": {"road_up": true, Optional: only faces whose supplied normal
                                    points road-up (the renderer's surface
                                    test), for a word also used on walls
                   "group": "face"  one group per face (default), or
                            {"run": N, "radius": R}: the faces of one mesh,
                                    one face group (bounds) and one material,
                                    in source order, N at most, every face
                                    centre within R of the first one's: one
                                    group, one centre, one decision, or
                            "instance": no group and no face added (below)
                  },
   "levels": ["source", "supersample"],
   "switch": [SWITCH]}
  SWITCH may also be {"grazing_texel_pixels": P, "camera_height": H}: the
  depth at which one texel of the face, seen along the ground from an eye H
  above it, is P pixels tall on a 640x480 picture at expected_focus: a ground
  length a at depth z covers focus*(480/384)*H*a/z^2 rows, so
  z = sqrt(focus*(480/384)*H*a/P), stored times 280/expected_focus. a is the
  face's shorter texel (world units per texel along texture u or v, from an
  affine fit of its corners' u, v; the finer axis, whichever way the face
  recedes). A multi-face group takes its members' smallest depth.
The faces of a multi-face group are moved next to each other; layers follow
the source order (prepare_face_order.find_layers orders a mesh's faces by
their source face, pack['origin']), so the layering is that of the faces in
place.

"group": "instance" (the slot machine's reels: 64 meshes of 3 faces, of which
each of three PLACED world-object instances draws one a frame). Nothing is
duplicated: the rule's materials carry the switch depth in material word
padding[1] (meshpack.h, nm_material_supersample_depth), and the renderer sets
the TSP super-sample bit on their headers while it draws an instance whose
model origin is beyond it (model_submission.h, sq_ss_select; the hysteresis of
the groups). The decision point is the model origin, so each face's switch is
computed with the centre (0, 0, 0) (its "reach" the largest corner distance
from the origin); the rule takes the least of its faces', one depth for all its
materials (pack['supersample_depths'], written by prepare_face_order.write_pack
after the scenery classes). Every face of the
rule must lie in a mesh drawn by PLACED world-object instances only, every
face of such a mesh must be in the rule (one depth a mesh), and every face of
a rule material in the rule; no "road_up".

build() is called where prepare_face_order.py would apply --opaque-cores.
"""
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import struct

import prepare_models as pm

REFERENCE_FOCUS = 280.0
PIXELS_PER_UNIT = 480.0/384.0      # output pixels per logical screen unit, vertical
LEVELS = 3
VARIANT_SHIFT, VARIANT_MASK = 8, 0x0300
SPLIT_MARK = 0x2000                # split_cutouts.py, split_header
STIPPLE_MARK = 0x8000              # prepare_face_order.py, stipple_header
SUPERSAMPLE_MARK = 0x1000          # header word 1: the far level of a "supersample" rule (module text)
OBJECT_PLACED, OBJECT_MESHES = 0x200, 0x800   # meshpack.h, NM_OBJECT_PLACED, NM_OBJECT_MESHES
STIPPLE_CELLS = {'quarter': ('X.', '..'), 'eighth': ('X...', '....', '..X.', '....')}
LOD = struct.Struct('<I3HBB3f2f')  # nm_lod_group_t
PACK_LOD_GROUPS, LOD_COUNT_SHIFT = 4, 16


def load(path):
    """LOD.json with every level specification read in."""
    path = Path(path)
    spec = json.loads(path.read_text())
    if float(spec.get('reference_focus', REFERENCE_FOCUS)) != REFERENCE_FOCUS:
        raise ValueError(f'level-of-detail depths are authored for focus {REFERENCE_FOCUS:g}')
    if not spec.get('rules'):
        raise ValueError('a level-of-detail specification needs rules')
    seen = set()
    if spec.get('cores'):
        merged = {'textures': {}}
        for entry in spec['cores'] if isinstance(spec['cores'], list) else [spec['cores']]:
            body = entry['spec']
            if not isinstance(body, dict):
                body = json.loads((path.parent/body).read_text())
            words = [str(int(w)) for w in entry.get('textures', body.get('textures', {}))]
            if body.get('unwrap') or body.get('merge') or any(w not in body.get('textures', {}) for w in words):
                raise ValueError('the plain cores of a level-of-detail specification: textures of the file, no unwrap or merge')
            if any(w in merged['textures'] for w in words):
                raise ValueError('a texture in two cores entries')
            for key, value in body.items():
                if key in ('textures', 'trunk_variation'):
                    continue
                if key in merged and merged[key] != value:
                    raise ValueError(f'cores entries disagree on {key}')
                merged[key] = value
            merged['textures'].update({w: body['textures'][w] for w in words})
            seen.update(int(w) for w in words)
        spec['cores'] = merged
    for rule in spec['rules']:
        levels = rule.get('levels', [])
        if not 1 <= len(levels) <= LEVELS or len(rule.get('switch', [])) != len(levels)-1:
            raise ValueError('a rule has one to three levels and one switch between each two')
        if bool(rule.get('textures')) == bool(rule.get('checker')):
            raise ValueError('a rule names textures or the checker panels')
        if 'supersample' in rule:
            options = rule['supersample']
            if not isinstance(options, dict) or set(options)-{'road_up', 'group'}:
                raise ValueError('"supersample" takes the options road_up and group')
            if levels != ['source', 'supersample']:
                raise ValueError('a supersample rule has the levels ["source", "supersample"]')
            group = options.get('group', 'face')
            if group not in ('face', 'instance') and not (isinstance(group, dict) and set(group) == {'run', 'radius'} and
                                                          int(group['run']) >= 1 and float(group['radius']) >= 0):
                raise ValueError('a supersample group is "face", "instance" or {"run": N, "radius": R}')
            if group == 'instance' and options.get('road_up'):
                raise ValueError('a supersample rule by instance takes no road_up')
        elif 'supersample' in levels:
            raise ValueError('the level "supersample" belongs to a supersample rule')
        for word in rule.get('textures', []):
            if int(word) in seen:
                raise ValueError(f'texture {word} is in two rules')
            seen.add(int(word))
        loaded = []
        for level in levels:
            if isinstance(level, dict) and 'stipple' in level:
                if not rule.get('checker') or set(level) != {'stipple'}:
                    raise ValueError('a stipple level belongs to the checker panels and has no other key')
                loaded.append(dict(stipple=stipple_cell(level['stipple'])))
            elif isinstance(level, dict):
                body = level['spec']
                if not isinstance(body, dict):
                    body = json.loads((path.parent/body).read_text())
                if rule.get('checker'):
                    raise ValueError('checker panels have no texture to design a shape from')
                if body.get('unwrap') or body.get('merge'):
                    raise ValueError('a level specification must not unwrap or merge: those change other faces')
                words = {str(int(w)) for w in rule['textures']}
                missing = words-set(body.get('textures', {}))
                if missing:
                    raise ValueError(f'a level specification lacks texture {sorted(missing)[0]}')
                loaded.append(dict(spec=dict(body, textures={w: body['textures'][w] for w in sorted(words)})))
            elif level in ('source', 'none', 'supersample'):
                loaded.append(level)
            else:
                raise ValueError(f'unknown level {level!r}')
        rule['levels'] = loaded
    stipple_variants(spec)
    return spec


def stipple_cell(cell):
    """The rows of a stipple cell ("X" opaque, "." hole), checked: powers of
    two each way, at least one opaque texel, none where the checkerboard of
    the full stipple has a hole (x+y odd)."""
    if isinstance(cell, str):
        if cell not in STIPPLE_CELLS:
            raise ValueError(f'unknown stipple cell {cell!r}')
        cell = STIPPLE_CELLS[cell]
    rows = tuple(cell)
    if not rows or any(not isinstance(row, str) or len(row) != len(rows[0]) or set(row)-set('X.') for row in rows):
        raise ValueError('a stipple cell is a list of equal strings of "X" and "."')
    if any(n < 2 or n & (n-1) for n in (len(rows), len(rows[0]))):
        raise ValueError('a stipple cell is 2, 4, 8, ... texels each way')
    if not any('X' in row for row in rows):
        raise ValueError('a stipple cell without an opaque texel: use the level "none"')
    if any(c == 'X' and (x+y) & 1 for y, row in enumerate(rows) for x, c in enumerate(row)):
        raise ValueError('a stipple cell may only be opaque where the checkerboard is (x+y even): the net thins in place')
    return rows


def stipple_variants(spec):
    """({rule index: [variant number per level]}, {variant number: cell}) of
    the checker rules; 0 is the checkerboard itself ("source" and "none")."""
    numbers, cells = {}, {}
    for index, rule in enumerate(spec['rules']):
        if not rule.get('checker'):
            continue
        row = []
        for level in rule['levels']:
            if isinstance(level, dict):
                cell = tuple(level['stipple'])
                number = next((n for n, known in cells.items() if known == cell), len(cells)+1)
                cells[number] = cell
                row.append(number)
            else:
                row.append(0)
        numbers[index] = row
    if len(cells) > VARIANT_MASK >> VARIANT_SHIFT:
        raise ValueError('more than three stipple variants')
    return numbers, cells


def stipple_image(cell, size, entry, hole=15):
    """`cell` repeated over a size x size texture as rows of palette indices
    (nested lists): `entry` where the cell is opaque, else `hole`. A level too
    small for the cell is all holes."""
    if size < len(cell) or size < len(cell[0]):
        return [[hole]*size for _ in range(size)]
    return [[entry if cell[y % len(cell)][x % len(cell[0])] == 'X' else hole for x in range(size)] for y in range(size)]


def stipple_base(header):
    """(the plain stipple header, variant number) of a stipple header that
    carries a variant number, else None."""
    number = (header[1] & VARIANT_MASK) >> VARIANT_SHIFT
    if not number or header[1] & ~VARIANT_MASK != STIPPLE_MARK or (header[0] >> 13) & 7 != 3:
        return None
    return (header[0], STIPPLE_MARK, header[2], header[3]), number


def _changes(level, word):
    """Texel changes of one level for one texture word, as a set."""
    if not isinstance(level, dict):
        return frozenset()
    entry = level['spec']['textures'][str(word)]
    return frozenset(tuple(t) for t in entry.get('fill', [])+entry.get('gutter', []))


def variants(spec):
    """{word: [variant number per level]} and {(word, variant): sorted texel
    changes}; variant 0 is the texture itself (level 0's changes)."""
    numbers, texels = {}, {}
    for rule in spec['rules']:
        if 'supersample' in rule:
            continue   # every level samples the texture itself
        for word in rule.get('textures', []):
            word = int(word)
            known = [_changes(rule['levels'][0], word)]
            numbers[word] = []
            for level in rule['levels']:
                # "source" and "none" sample nothing new: the texture itself.
                changes = _changes(level, word) if isinstance(level, dict) else known[0]
                if changes not in known:
                    known.append(changes)
                numbers[word].append(known.index(changes))
            if len(known) > 4:
                raise ValueError('more than three texture variants')
            for number, changes in enumerate(known):
                texels[word, number] = sorted(changes)
    return numbers, texels


def texture_fill(spec):
    """For prepare_palette_materials: ({word: changes of the texture itself},
    {(word, variant): changes of a copy})."""
    _, texels = variants(spec)
    plain = {int(word): entry.get('fill', [])+entry.get('gutter', []) for word, entry in (spec.get('cores') or {}).get('textures', {}).items()}
    return ({**plain, **{word: list(map(list, changes)) for (word, number), changes in texels.items() if not number and changes}},
            {key: list(map(list, changes)) for key, changes in texels.items() if key[1]})


def variant_header(header, number):
    return (header[0], header[1] | number << VARIANT_SHIFT, header[2], header[3])


def supersample_header(header):
    """The far level's header of a supersample rule: a textured header without a mark, plus SUPERSAMPLE_MARK."""
    if not header[0] & 0x4000 or header[1] & 0xff00:
        raise ValueError(f'header {header}: only an unmarked textured header gets a super-sampled variant')
    return (header[0], header[1] | SUPERSAMPLE_MARK, header[2], header[3])


def supersample_base(header):
    """The header a super-sampled variant stands for, or None for any other header."""
    if header[1] & 0xff00 != SUPERSAMPLE_MARK or not header[0] & 0x4000:
        return None
    return (header[0], header[1] & 0xff, header[2], header[3])


def texel_length(corners, uvs):
    """World units per texel along texture u and along v of a polygon (affine fit of its corners; u, v in
    1/8 texel as stored): the shorter of the two, or None without a usable mapping."""
    import numpy as np
    if len(corners) < 3:
        return None
    uv = np.array([[u/8.0, v/8.0, 1.0] for u, v in uvs])
    if np.linalg.matrix_rank(uv) < 3:
        return None
    fit = np.linalg.lstsq(uv, np.array(corners, dtype=float), rcond=None)[0]
    lengths = [float(np.linalg.norm(fit[0])), float(np.linalg.norm(fit[1]))]
    return min(lengths) if min(lengths) > 0 else None


def base_header(header):
    """(the punch-through header a variant or split header stands for, variant
    number, is an opaque split piece), or None for any other header."""
    mark = header[1] & 0xff00
    if not mark or mark & ~(SPLIT_MARK | VARIANT_MASK):
        return None
    split = bool(mark & SPLIT_MARK)
    if split != ((header[0] >> 13) & 7 == 2):
        return None
    return (header[0] | 0x2000, header[1] & 0xff, header[2], header[3]), (mark & VARIANT_MASK) >> VARIANT_SHIFT, split


def _switch_depth(entry, corners, uvs, centre, texels_per_unit, expected_focus):
    if 'depth' in entry:
        depth = float(entry['depth'])
    elif 'pixels' in entry:
        of = entry.get('of', 'height')
        if of == 'height':
            size = max(p[1] for p in corners)-min(p[1] for p in corners)
        elif of == 'width':
            size = math.hypot(max(p[0] for p in corners)-min(p[0] for p in corners),
                              max(p[2] for p in corners)-min(p[2] for p in corners))
        elif of == 'longest':
            size = max(math.dist(a, b) for a in corners for b in corners)
        else:
            raise ValueError(f'unknown size {of!r}')
        depth = size*REFERENCE_FOCUS*PIXELS_PER_UNIT/float(entry['pixels'])
    elif 'texel_pixels' in entry:
        density = entry.get('texels_per_unit', texels_per_unit)
        if density is None:
            density = max((math.dist(uvs[i], uvs[i-1])/8.0/math.dist(corners[i], corners[i-1])
                           for i in range(len(corners)) if math.dist(corners[i], corners[i-1]) > 1e-6), default=0.0)
        if not density > 0:
            raise ValueError('a polygon without a texture mapping cannot switch by texel size')
        depth = REFERENCE_FOCUS*PIXELS_PER_UNIT/(float(density)*float(entry['texel_pixels']))
    elif 'grazing_texel_pixels' in entry:
        # Ground seen along itself (module text): rows = focus*PIXELS_PER_UNIT*height*texel/z^2.
        texel = texel_length(corners, uvs)
        height, pixels = float(entry.get('camera_height', 0)), float(entry['grazing_texel_pixels'])
        if texel is None or not height > 0 or not pixels > 0:
            raise ValueError('a grazing switch needs a texture mapping, camera_height and pixels above zero')
        depth = math.sqrt(expected_focus*PIXELS_PER_UNIT*height*texel/pixels)*REFERENCE_FOCUS/expected_focus
    else:
        raise ValueError('a switch needs depth, pixels, texel_pixels or grazing_texel_pixels')
    if entry.get('reach') == 'radius':
        depth += max(math.dist(p, centre) for p in corners)*REFERENCE_FOCUS/expected_focus
    elif entry.get('reach') not in (None, 'centre'):
        raise ValueError(f'unknown reach {entry["reach"]!r}')
    if not depth > 0 or not math.isfinite(depth):
        raise ValueError('a switch depth must be positive')
    return struct.unpack('<f', struct.pack('<f', depth))[0]


def build(pack, spec, images, skip=(), light=None, checker_texels_per_unit=None):
    """Replace every polygon a rule names by its levels, in place
    (prepare_face_order.read_pack tables).

    Each level's specification is applied by opaque_cores.run to a copy of
    the pack; the faces it makes of a source polygon become that polygon's
    range of the level. Afterwards the faces of a group are consecutive in
    pack['faces'] (level 0 first) and
      pack['lod_face']   per face None or (group, level),
      pack['lod']        per group dict(mesh, origin, faces per level,
                         centre, depths, rule),
      pack['split_materials'] / pack['variant_materials']  new material ->
                         the punch-through material whose class, palette and
                         colour it takes (an opaque piece / the same list),
      pack['overlay']    as opaque_cores.apply, within a level.
    order_faces keeps the ranges together; write_pack writes the table.
    Returns (report, log of the pieces per level).
    """
    import opaque_cores
    size = pm.MATERIAL.size
    base_faces, base_materials = pack['faces'], pack['materials']
    origin = pack.setdefault('origin', list(range(len(base_faces))))
    headers = [struct.unpack_from('<4H', base_materials, m*size) for m in range(pack['header'][5])]
    numbers, _ = variants(spec)
    stipple_numbers, _ = stipple_variants(spec)
    expected_focus = float(spec.get('expected_focus', REFERENCE_FOCUS))
    if not expected_focus > 0:
        raise ValueError('expected_focus must be positive')
    # Which rule takes which source face.
    rule_of = {}
    for index, rule in enumerate(spec['rules']):
        words = {int(w) for w in rule.get('textures', [])}
        for f, face in enumerate(base_faces):
            header = headers[face[4]]
            if rule.get('checker'):
                chosen = bool(header[0] & 0x8000)
            elif 'supersample' in rule:
                # Textured, unmarked, any list and stage; road_up: the renderer's surface test on the supplied normal.
                n = struct.unpack('<3f', struct.pack('<3f', *face[8:11]))
                squared = n[0]*n[0]+n[1]*n[1]+n[2]*n[2]
                chosen = (header[2] in words and bool(header[0] & 0x4000) and not header[0] & 0x8000 and
                          not header[1] & 0xff00 and
                          (not rule['supersample'].get('road_up') or (squared > 0 and n[1] < 0 and n[1]*n[1] >= 0.5625*squared)))
            else:   # the faces opaque_cores.apply would cut
                chosen = (header[2] in words and (header[0] >> 13) & 7 == 3 and not header[1] & 0xff00 and
                          header not in skip)
            if chosen:
                if f in rule_of:
                    raise ValueError('a face is in two rules')
                rule_of[f] = index
    report = dict(rules=[])
    if not rule_of:
        raise ValueError('no face matches a level-of-detail rule')
    # A level cut by an opaque-cores specification finds its faces by source face: one face per source face
    # there. Rules whose levels keep the face (source, supersample, stipple, none) take faces as they are, so
    # also the pieces of a road decal laid onto the road (road_drape.py).
    cut = {f for f, index in rule_of.items()
           if any(isinstance(level, dict) and 'spec' in level for level in spec['rules'][index]['levels'])}
    named = [origin[f] for f in cut]
    if len(set(named)) != len(named) or set(named) & {o for f, o in enumerate(origin) if f not in cut}:
        raise ValueError('a face of a level-of-detail rule was cut before: one face per source face')
    refs = struct.unpack(f'<{pack["header"][3]}H', pack['refs'])
    del refs
    records = [pack['vertices'][i*pm.VERTEX.size:(i+1)*pm.VERTEX.size] for i in range(pack['header'][3])]
    mesh_of = {}
    for index, mesh in enumerate(pack['meshes']):
        for f in range(mesh[2], mesh[2]+mesh[3]):
            mesh_of[f] = index

    def with_records(table, mesh, face):
        return tuple(table[mesh[0]+i] for i in face[:face[5]])+tuple([b'']*(4-face[5]))+tuple(face[4:])

    # Supersample rules by instance (module text): a switch depth on the rule's materials, no group, no face added.
    instance_rules = {index for index, rule in enumerate(spec['rules'])
                      if rule.get('supersample', {}).get('group') == 'instance'}
    instance_reports = {}
    if instance_rules:
        drawers = defaultdict(set)   # mesh -> instance words that draw it
        for i in range(len(pack['instances'])//pm.INSTANCE.size):
            row = pm.INSTANCE.unpack_from(pack['instances'], i*pm.INSTANCE.size)
            word = row[-4]
            count = ((word >> 16) & 0xff)+1 if word & pm.OBJECT_INSTANCE and word & OBJECT_MESHES else 1
            for m in range(row[0], row[0]+count):
                drawers[m].add(word)
        depths = pack.setdefault('supersample_depths', {})
        for index in sorted(instance_rules):
            rule = spec['rules'][index]
            members = [f for f in sorted(rule_of) if rule_of[f] == index]
            name = rule.get('name', str(index))
            if not members:
                raise ValueError(f'rule {name}: no face')
            materials_of = {base_faces[f][4] for f in members}
            meshes = {mesh_of[f] for f in members}
            for m in meshes:
                if not drawers[m] or any(not word & pm.OBJECT_INSTANCE or not word & OBJECT_PLACED for word in drawers[m]):
                    raise ValueError(f'rule {name}: mesh {m} is not drawn by placed world-object instances only')
                mesh = pack['meshes'][m]
                if any(rule_of.get(f) != index for f in range(mesh[2], mesh[2]+mesh[3])):
                    raise ValueError(f'rule {name}: mesh {m} has faces outside the rule (one depth a mesh)')
            if any(face[4] in materials_of and rule_of.get(f) != index for f, face in enumerate(base_faces)):
                raise ValueError(f'rule {name}: a face outside the rule shares a material of the rule')
            values = []
            for f in members:
                mesh = pack['meshes'][mesh_of[f]]
                corners = [struct.unpack_from('<3f2H', records[mesh[0]+i]) for i in base_faces[f][:base_faces[f][5]]]
                values.append([_switch_depth(entry, [p[:3] for p in corners], [p[3:5] for p in corners], (0.0, 0.0, 0.0),
                                             None, expected_focus) for entry in rule['switch']][0])
            depth = min(values)
            for m in materials_of:
                depths[m] = depth
            for f in members:
                del rule_of[f]
            instance_reports[index] = dict(name=name, faces=len(members), levels=[], by_instance=dict(
                materials=len(materials_of), meshes=len(meshes), switch_depth_at_focus_280=depth,
                face_switch_depths=dict(least=min(values), most=max(values))))

    # Per rule and level: {source face: [(face with corner records, material record, source material or None, below)]}
    materials = bytearray(base_materials)
    # Keyed by the whole record (header, diffuse, ambient, specular, words): two source materials with one header
    # and different lighting keep their own variants, so a level carries its source's baked light.
    material_index = {bytes(materials[m*size:(m+1)*size]): m for m in range(len(materials)//size)}
    split_materials, variant_materials = {}, {}
    groups_out = bytearray(pack['groups'])
    made, logs = defaultdict(dict), []

    def material_for(record, source_material, split, number, supersample=False):
        """Final material index of a level's material record."""
        header = struct.unpack_from('<4H', record)
        if number:
            header = variant_header(header, number)
        if supersample:
            header = supersample_header(header)
        key = struct.pack('<4H', *header)+bytes(record[8:size])
        if key not in material_index:
            if not split and not number and not supersample:
                raise AssertionError('an unknown punch-through material')
            material_index[key] = len(materials)//size
            materials.extend(key)
            (split_materials if split else variant_materials)[material_index[key]] = source_material
        return material_index[key]

    for index, rule in enumerate(spec['rules']):
        if index in instance_reports:
            report['rules'].append(instance_reports[index])
            continue
        members = [f for f in sorted(rule_of) if rule_of[f] == index]
        entry = dict(name=rule.get('name', str(index)), faces=len(members), levels=[])
        for number, level in enumerate(rule['levels']):
            stats = Counter()
            if level == 'none':
                for f in members:
                    made[f][number] = []
            elif level == 'source':
                for f in members:
                    mesh = pack['meshes'][mesh_of[f]]
                    face = base_faces[f]
                    made[f][number] = [(with_records(records, mesh, face), face[4], [])]
                    stats['faces'] += 1
                    stats['triangles'] += face[5]-2
            elif level == 'supersample':
                # The same face under its material with SUPERSAMPLE_MARK.
                for f in members:
                    mesh = pack['meshes'][mesh_of[f]]
                    face = base_faces[f]
                    final = material_for(base_materials[face[4]*size:(face[4]+1)*size], face[4], False, 0, True)
                    own = with_records(records, mesh, face)
                    made[f][number] = [(own[:4]+(final,)+own[5:], final, [])]
                    stats['faces'] += 1
                    stats['triangles'] += face[5]-2
                stats['materials'] = len({base_faces[f][4] for f in members})
            elif 'stipple' in level:
                # The same quad under the checker material with a variant number.
                variant = stipple_numbers[index][number]
                for f in members:
                    mesh = pack['meshes'][mesh_of[f]]
                    face = base_faces[f]
                    if headers[face[4]][1] & 0xff00:
                        raise ValueError('a checker material with a marked header')
                    final = material_for(base_materials[face[4]*size:(face[4]+1)*size], face[4], False, variant)
                    own = with_records(records, mesh, face)
                    made[f][number] = [(own[:4]+(final,)+own[5:], final, [])]
                    stats['faces'] += 1
                    stats['triangles'] += face[5]-2
                stats['stipple_variant'] = variant
                stats['stipple_cell'] = list(level['stipple'])
                stats['stipple_coverage'] = sum(row.count('X') for row in level['stipple'])/(len(level['stipple'])*len(level['stipple'][0]))
            else:
                copy = dict(pack, header=list(pack['header']), faces=list(base_faces), origin=list(origin),
                            meshes=[list(mesh) for mesh in pack['meshes']])
                for key in ('split_materials', 'overlay', 'foreign_trunks', 'merged'):
                    copy.pop(key, None)
                level_report, log = opaque_cores.run(copy, level['spec'], images, skip, light)
                logs.append(dict(rule=entry['name'], level=number, pieces=log))
                table = [copy['vertices'][i*pm.VERTEX.size:(i+1)*pm.VERTEX.size] for i in range(copy['header'][3])]
                split = copy.get('split_materials', {})
                by_origin, local = defaultdict(list), {}
                wanted = {origin[f]: f for f in members}
                for mesh in copy['meshes']:
                    for g in range(mesh[2], mesh[2]+mesh[3]):
                        f = wanted.get(copy['origin'][g])
                        if f is None:
                            continue
                        face = copy['faces'][g]
                        record = copy['materials'][face[4]*size:(face[4]+1)*size]
                        source = split.get(face[4], face[4])
                        if source >= len(headers):
                            raise AssertionError('a split material of a split material')
                        word = headers[source][2]
                        variant = numbers.get(word, [0]*LEVELS)[number]
                        final = material_for(record, source, face[4] in split, variant)
                        local[g] = (f, len(by_origin[f]))
                        by_origin[f].append([with_records(table, mesh, face)[:4]+(final,)+tuple(face[5:]), final, []])
                        stats['faces'] += 1
                        stats['triangles'] += face[5]-2
                        stats['opaque_triangles' if face[4] in split else 'punch_through_triangles'] += face[5]-2
                for piece, below in copy.get('overlay', {}).items():
                    if piece not in local:
                        continue
                    f, at = local[piece]
                    for under in below:
                        if under in local and local[under][0] == f:
                            by_origin[f][at][2].append(local[under][1])
                for f in members:
                    made[f][number] = [tuple(item) for item in by_origin.get(f, [])]
                # Facing spheres the level enlarged (opaque_cores.apply): keep the largest.
                if copy['groups'] != pack['groups'] and pm.GROUP.size == 64:
                    for g in range(len(groups_out)//64):
                        radius = pm.GROUP.unpack_from(copy['groups'], g*64)[13]
                        values = list(pm.GROUP.unpack_from(groups_out, g*64))
                        if radius > values[13]:
                            values[13] = radius
                            pm.GROUP.pack_into(groups_out, g*64, *values)
                stats['texture_variants'] = sorted({numbers[int(w)][number] for w in rule['textures']})
                entry.setdefault('cores', []).append(level_report.get('cores', {}).get('textures'))
            entry['levels'].append(dict(stats))
        report['rules'].append(entry)

    # Supersample rules: the source faces of each group (module text), in source order.
    positions = [struct.unpack_from('<3f2H', r) for r in records]

    def face_centre(f):
        mesh = pack['meshes'][mesh_of[f]]
        corners = [positions[mesh[0]+i] for i in base_faces[f][:base_faces[f][5]]]
        return [sum(p[a] for p in corners)/len(corners) for a in range(3)]
    joined = {}   # first source face of a multi-face group -> its source faces; other members -> None
    for index, rule in enumerate(spec['rules']):
        group = rule.get('supersample', {}).get('group', 'face') if 'supersample' in rule else 'face'
        if group in ('face', 'instance'):
            continue
        limit, radius = int(group['run']), float(group['radius'])
        for mesh_index, mesh in enumerate(pack['meshes']):
            open_runs = {}
            for f in range(mesh[2], mesh[2]+mesh[3]):
                if rule_of.get(f) != index:
                    continue
                key = (base_faces[f][11], base_faces[f][4])   # face group (bounds), material
                run = open_runs.get(key)
                centre = face_centre(f)
                if run is None or len(run[1]) >= limit or math.dist(run[0], centre) > radius:
                    run = open_runs[key] = (centre, [])
                    joined[f] = run[1]
                else:
                    joined[f] = None
                run[1].append(f)

    # Compose: per mesh the faces in source order, a group's levels in a row (a multi-face group at its first face).
    mesh_faces, lod_face, lod, overlay, moved = [], [], [], {}, {}
    plain = Counter()
    for mesh_index, mesh in enumerate(pack['meshes']):
        members = []
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = base_faces[f]
            if f not in rule_of:
                moved[f] = len(lod_face)
                members.append((with_records(records, mesh, face), origin[f]))
                lod_face.append(None)
                continue
            rule = spec['rules'][rule_of[f]]
            sources = joined.get(f, [f])
            if sources is None:
                continue   # placed with the first face of its group
            levels = [[(item, g) for g in sources for item in made[g][n]] for n in range(len(rule['levels']))]
            own = with_records(records, mesh, face)
            if len(sources) == 1 and all(len(level) == 1 and level[0][0][0] == own for level in levels):
                # Every level is the source face (e.g. an undrawable face): no group.
                members.append((own, origin[f]))
                lod_face.append(None)
                plain[rule.get('name', str(rule_of[f]))] += 1
                continue
            centres, depth_rows = [], []
            for g in sources:
                corners = [positions[mesh[0]+i] for i in base_faces[g][:base_faces[g][5]]]
                centre = [struct.unpack('<f', struct.pack('<f', sum(p[a] for p in corners)/len(corners)))[0] for a in range(3)]
                centres.append(centre)
                try:
                    depth_rows.append([_switch_depth(entry, [p[:3] for p in corners], [p[3:5] for p in corners], centre,
                                                     checker_texels_per_unit if rule.get('checker') else None, expected_focus)
                                       for entry in rule['switch']])
                except ValueError:
                    # A member of a multi-face group without a texture mapping of its own (a sliver of a road decal
                    # laid onto the road, road_drape.py, whose texture coordinates round to one line): the others decide.
                    if len(sources) == 1:
                        raise
            if not depth_rows:
                raise ValueError(f'source face {origin[f]}: no member of the group has a texture mapping')
            if len(sources) == 1:
                centre, depths = centres[0], depth_rows[0]
            else:
                centre = [struct.unpack('<f', struct.pack('<f', sum(c[a] for c in centres)/len(centres)))[0] for a in range(3)]
                depths = [min(row[n] for row in depth_rows) for n in range(len(rule['switch']))]
            if any(b < a for a, b in zip(depths, depths[1:])):
                raise ValueError(f'source face {origin[f]}: switch depths must not decrease')
            group = len(lod)
            first = sum(len(m) for m in mesh_faces)+len(members)
            ranges = []
            for number, level in enumerate(levels):
                start = sum(len(m) for m in mesh_faces)+len(members)
                for (item, _, below), g in level:
                    if below:
                        overlay[sum(len(m) for m in mesh_faces)+len(members)] = [start+b for b in below]
                    members.append((item, origin[g]))
                    lod_face.append((group, number))
                ranges.append(len(level))
            if len({item[11] for level in levels for (item, _, _), _ in level}) > 1:
                raise ValueError('the shapes of one group must share its face group')
            lod.append(dict(mesh=mesh_index, origin=origin[f], first=first, counts=ranges, centre=centre, depths=depths,
                            rule=rule.get('name', str(rule_of[f])), sources=len(sources)))
        mesh_faces.append(members)
    pack['materials'] = bytes(materials)
    pack['header'][5] = len(materials)//size
    pack['groups'] = bytes(groups_out)
    opaque_cores._rewrite(pack, mesh_faces)
    if len(pack['faces']) != len(lod_face):
        raise AssertionError('face bookkeeping')
    pack['lod_face'], pack['lod'] = lod_face, lod
    pack['split_materials'] = {**pack.get('split_materials', {}), **split_materials}
    pack['variant_materials'] = variant_materials
    # Faces cut before (the specification's plain cores) keep their relations under their new indices.
    pack['overlay'] = {**{moved[piece]: [moved[b] for b in below] for piece, below in pack.get('overlay', {}).items()}, **overlay}
    if pack.get('foreign_trunks'):
        pack['foreign_trunks'] = [moved[f] for f in pack['foreign_trunks']]
    for entry in report['rules']:
        mine = [g for g in lod if g['rule'] == entry['name']]
        entry['groups'] = len(mine)
        if any(g['sources'] > 1 for g in mine):
            entry['source_faces_per_group'] = dict(sorted(Counter(g['sources'] for g in mine).items()))
        entry['faces_left_plain'] = plain[entry['name']]
        for n in range(max((len(g['depths']) for g in mine), default=0)):
            values = sorted(g['depths'][n] for g in mine)
            entry.setdefault('switch_depth_at_focus_280', []).append(
                dict(least=values[0], median=values[len(values)//2], most=values[-1]))
    report.update(groups=len(lod), faces=len(pack['faces']), split_materials=len(split_materials),
                  variant_materials=len(variant_materials),
                  texture_variants={str(word): row for word, row in sorted(numbers.items())})
    if any(any(row) for row in stipple_numbers.values()):
        report.update(stipple_variants={spec['rules'][index].get('name', str(index)): row for index, row in stipple_numbers.items()})
    return report, logs


def table(pack, order, strips):
    """(nm_lod_group_t bytes, (first, count) per mesh) for the sorted order.

    `order` is new->old face indices; a group's faces must have come out
    consecutive, level after level (order_faces sees to it)."""
    new = {old: at for at, old in enumerate(order)}
    lod_face, groups = pack['lod_face'], pack['lod']
    members = defaultdict(lambda: [[] for _ in range(LEVELS)])
    for old, key in enumerate(lod_face):
        if key is not None:
            members[key[0]][key[1]].append(new[old])
    rows = []
    for group, entry in enumerate(groups):
        levels = members[group]
        every = [f for level in levels for f in level]
        if not every:
            raise ValueError(f'source face {entry["origin"]}: no level has a face')
        first = min(every)
        at = first
        for number, level in enumerate(levels):
            if sorted(level) != list(range(at, at+len(level))):
                raise AssertionError('the ranges of a group are not consecutive')
            at += len(level)
        count = len(entry['counts'])
        rows.append((entry['mesh'], first, [len(level) for level in levels], count, entry))
    rows.sort(key=lambda row: (row[0], row[1]))
    # A strip lies inside one range or outside every group.
    owner = {}
    for mesh, first, counts, _, _ in rows:
        at = first
        for number, n in enumerate(counts):
            for f in range(at, at+n):
                owner[f] = (first, number)
            at += n
    for first, count, _, _ in strips:
        if len({owner.get(f) for f in range(first, first+count)}) != 1:
            raise AssertionError('a strip crosses a level-of-detail range')
    out, per_mesh, entries = bytearray(), [], []
    for index in range(len(pack['meshes'])):
        mine = [row for row in rows if row[0] == index]
        per_mesh.append((len(entries) if mine else 0, len(mine)))
        for _, first, counts, count, entry in mine:
            depths = list(entry['depths'])+[0.0]*(LEVELS-1-len(entry['depths']))
            out += LOD.pack(first, *counts, count, 0, *entry['centre'], *depths)
            entries.append(dict(origin=entry['origin'], rule=entry['rule'], mesh=index, first=first, counts=counts[:count],
                                centre=entry['centre'], depths=entry['depths']))
    if len(entries) > 65535:
        raise ValueError('more than 65535 level-of-detail groups')
    return bytes(out), per_mesh, entries
