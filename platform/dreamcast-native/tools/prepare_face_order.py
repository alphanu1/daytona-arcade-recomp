"""Dreamcast draw order for a prepared DNM2/4/5 pack: writes DNM6.

python3 platform/dreamcast-native/tools/prepare_face_order.py IN.dnm NEW_OUT.dnm \
    [--intro-header intro_drive_data.h | --part-lists LISTS.json] \
    [--materials PACK.dnt] [--order-map NEW.json]

The arcade has no depth buffer and draws polygons in submission order; the
packs carry that order. On the PowerVR the order of depth-tested opaque and
punch-through polygons only matters where faces are coplanar, and keeping it
costs a 32-byte header whenever consecutive faces change material and blocks
strips. This pass, run once offline:

1. Layers. Every drawable face that lies in the plane of, and overlaps, a
   face the arcade drew before it gets a layer above that face: 0 base, 1 on
   top, 2 on top of that. "Before" is the source face order inside a mesh and
   the part order between meshes that one car draws with the same matrix
   (--intro-header / --part-lists). The renderer multiplies the written depth
   by 1+layer*step (NM_LAYER_DEPTH_STEP), so the result no longer depends on
   which face is submitted last. Faces that can never be seen together
   (one-sided, opposite normals) are not layered. "In the plane" means every
   corner within PLANE of the other face's plane, or the two faces share at
   least three corners: a decal drawn on the corners of a slightly warped
   quad is that quad to the arcade, which rasterises quads whole. Parallel
   faces further apart are left to the depth buffer.
   Known limit, reported as "same_corner_dips": such a decal may be split on
   the other diagonal than its base, so in the middle it lies below the base
   by the quad's warp. The relative step lifts it only where step*distance
   exceeds that; nearer than that the base shows through, as it did before.
   --match-decal-diagonals removes the cause: such a decal quad is rotated
   by one corner so it uses its base's diagonal and their triangles
   coincide. The quad, its corners, UVs, winding, normal and material are
   unchanged; its two triangles are the other pair, so the submitted
   triangle set differs for exactly those faces. Off by default.
2. Order. Faces of each mesh are sorted by PVR list (opaque, then
   punch-through; screen-checker faces the renderer never draws go last),
   then material, then layer, then supplied normal: equal normals light alike
   and share a material binding, and the normal key puts the dominant
   (vertical) light term first so a material's texture variants form runs.
   With --materials, materials whose bindings mostly use the same texture
   resource (hence the same PVR header) are placed next to each other.
3. Strips. Inside one (material, layer, normal) run, edge-adjacent faces are
   chained into the longest strips the DNM5 strip record can express (whole
   faces, original fan diagonal, triangle order and winding).

Vertices, materials, instances, position references and face-group records
are untouched: groups are selected by face.group, which travels with the
face. Input must be a pack the C validator accepts; DNM6 input is refused
because its arcade order is gone. ROM-derived output stays under build/.

Static scenery (--static-light X Y Z, the scene's world light): the world does
not move relative to its light, so the luma of every face is a constant of
the pack. It is computed here with the reference's single-precision
arithmetic (lighting.h, nm_face_lighting_eval, supplied normals) and written
into the face: face.reserved is the luma of the side the supplied normal
points to ("front", facing >= 0), face.padding the luma of the other side of
a two-sided face. The side away from the light gets the material's ambient
only. A face whose luma depends on the view (computed normals, or an
effective specular term) cannot be baked and is refused. Header flag
NM_PACK_BAKED_LIGHT (header.padding[3] bit 0) tells the renderer to use them.
With --live-specular the faces of an effective specular term (mode 1, control
and scale non-zero, ambient below 255: Expert's glass and metal) are accepted:
their lumas are baked without the specular term and the renderer adds it every
frame from the camera's turn (meshpack.h NM_PACK_LIVE_SPECULAR, bit 4). They
never join a flattened stack (its composite is keyed by constant lumas).

Scenery classes (--scenery-classes track_surface.c): each material gets the
stage that first draws it and its depth class in material.padding[0]
(meshpack.h, NM_SCENERY_*); header flag NM_PACK_SCENERY_CLASSES (bit 1).
Stage 0 is the audited reduced view (the 40 headers of track_surface.c),
1 other opaque faces, 2 opaque ground (every face of the material points
road-up), 3 punch-through cutouts, 4 the screen-checker panels. The road
surface bit keeps the renderer's geometric rule (every face of the material
points road-up) for the reduced view's materials.
Road decals: a later-stage material whose faces all point road-up and lie
over road-surface faces (in plan, within ROAD_DECAL_REACH above or below
them) is paint or wear on the road, e.g. the light streaks of the racing
line. The arcade's polygon sort shows it over the asphalt although its faces
float up to a unit off the coarse road mesh, where no coplanar layer can be
found. It takes the road-surface class, so cars and their shadows treat it as
road, and every one of its faces is lifted by ROAD_DECAL_LAYER layers so it
wins over the road it lies on (written depth times 1+layer*step).
--drape-road-decals (road_drape.py) instead raises the corners of the decal
faces that dip under the road triangles, so that every decal lies on or above
the road, and lifts the decals one layer above the road surface (1 + the
largest layer of the road-surface faces): their written depth stays their
true one, and the cars' shadow volumes fall on them at any distance. Decal and
other faces are then not layered against each other (find_layers, unpaired).
Road apron: a stage-2 ground material that is level (every normal within 8
degrees of road-up) and joins the road edge to edge (most of its faces share
two corners with a road-surface face) is driven on like the road and takes
the road-surface class too. Other ground keeps true depth: with the
road-surface class, scenery standing behind a rise of the ground within
twice its distance would show through it (--surface-ground forces the class
on all stage-2 ground for comparison: cars are then never cut by grass).

Flattened stacks (--flatten-stacks, --composites NEW.json): the arcade builds
a picture such as the grandstand crowd from an opaque face and punch-through
faces drawn on the very same polygon (course 0: seats, then three layers of
spectators, each texture tinted by one colour: four polygons and four times
the fill for one surface). Every such stack becomes one opaque face whose
texture is the layers laid over each other, see flatten_stacks. The removed
faces are listed in the report ("flattened_stacks"/"removed", source face
indices) so a stack can be compared or restored one by one.

Screen-checker panels (--stipple-panels): the arcade draws these polygons on
every other pixel of a screen checkerboard (course 0: the catch fence above
the wall, a dark net with the crowd, the stand and the sky seen through it
from 50 units and more: nothing lies close enough behind a panel to carry
its shade). On the PowerVR the half coverage is a texture: the material
becomes a punch-through one whose texture is a checkerboard of the panel's
colour and holes, every mip level again a checkerboard, so the PVR's level
selection keeps the holes near one pixel at any distance. The faces go to
the punch-through list like any cutout; the translucent list stays empty.
With --lod-groups a panel may have a level with a sparser stipple (a second
material, the variant number in bits 8..9 of header word 1; lod_groups.py).

Split cutouts (--split-cutouts MODE --split-video DUMP, split_cutouts.py): a
punch-through face is cut, in its own plane, into parts that are opaque
throughout (drawn on the opaque list, where hidden-surface removal makes
overdraw nearly free), parts that contain transparent texels (they stay
punch-through) and parts that are transparent throughout (not drawn). The
opaque parts use a new material: the source's record under split_header(),
with the source's scenery stage and depth class plus NM_SCENERY_POINT (point
sampled, as the punch-through list is). prepare_palette_materials.py
--split-mesh gives it the source's texture, palette bank and colour. The
pieces replace the face in the source order, so layers, baked light, order
and strips follow as for any face; all pieces of one face get one layer.
The reduced view's own punch-through materials are left alone unless
--split-reduced is given.
An added face costs the SH4 in every frame it is drawn, so the mode to use is
auto: --split-weights names the simulator's per-face fill of recorded runs
with the unsplit pack, and each face takes the plan (or none) that removes
the most punch-through pixel samples after paying --split-pixel-price
samples per added face and frame. The other modes apply one recipe to every
face that loses --split-least-gain of its punch-through area; they are for
comparison (course 0: outline and both grids exceed the renderer's triangle
capacity in the densest frames).

Opaque cores (--opaque-cores SPEC.json, opaque_cores.py; not together with
--split-cutouts): per texture word of the specification, opaque polygons
designed once in texture space are clipped to every punch-through face of
that texture and drawn with the split material of --split-cutouts (same
texture, palette, light; NM_SCENERY_POINT). Kind overlay keeps the
punch-through face and puts the cores one layer above it, so its fragments
fail the depth test there; kinds outline and rects remove the face. The
specification may also name punch-through textures whose adjacent faces get
texture coordinates that agree along shared edges (so they chain into
strips) and whose coplanar quads are joined. The same file goes to
prepare_palette_materials.py --texture-fill for the texels it changes.

Punch-through to opaque (--pt-to-opaque, pt_to_opaque.py): a punch-through
face whose bilinear footprint (half a texel around its UV polygon, wrap and
mirror applied) holds no transparent texel moves to the opaque list; one that
reaches transparent texels only beyond a hole-free rectangle of the opaque
texels it touches is cut to that rectangle first (exact UV-to-position map,
no opaque texel lost; --pt-to-opaque-trim no). Same texture, palette, colour
and light: the source's record under a converted header (CONVERTED_MARK),
one per source material, with the source's scenery class. Runs after
--bake-decals and before every cut; texture words of --lod-groups and
--opaque-cores, and the reduced view, are left alone. Report and order map
key "pt_to_opaque". prepare_palette_materials.py --split-mesh gives the
converted headers the source bindings.

Level of detail (--lod-groups LOD.json, lod_groups.py; not together with
--opaque-cores or --split-cutouts): every punch-through polygon a rule of
LOD.json names becomes a group of up to three alternative shapes, each what
an opaque-cores specification makes of it (or the polygon itself, or
nothing), with the view depths at which the renderer changes from one to the
next (given, or derived per polygon from an on-screen size). In the pack
(meshpack.h, NM_PACK_LOD_GROUPS): the faces of a group are consecutive, shape
after shape, behind the mesh's other faces; strips stay inside one shape; the
shapes of one group are not layered against each other (they are never drawn
together), each is layered on its own against everything else; one record per
group in a table behind the strips. LOD.json also goes to
prepare_palette_materials.py --texture-fill: a shape whose texel changes
differ from the first shape's gets a second copy of the texture.
"""
import argparse
import hashlib
import itertools
import json
import math
from collections import Counter, defaultdict
from pathlib import Path
import re
import struct

import prepare_models as pm

LAYERED = 0x364d4e44
FACE6 = struct.Struct('<5HBBI3fHBB')
STRIP = struct.Struct('<IHHI5I')
# Geometric tolerances, in model units (a car is about 4.5 long; world
# coordinates reach a few thousand, where one float32 step is 2.4e-4).
# Measured on the course 0 packs (report "plane_gap": every overlapping
# parallel pair up to 0.25 apart). Cars: faces without shared corners are
# exactly in plane, or stacked 1e-3 apart with the later one in front (one
# model), or at least 7e-3 apart. World, reduced view: the two layered faces
# are exact. The full scenery also has large (20..40 unit) slightly warped
# faces whose gaps run from 5e-4 through this limit to the real offsets
# (0.05, 0.2): audit them when the scenery is drawn again.
PARALLEL = 1e-4      # 1 - |cos| between the geometric normals
SHARED_PARALLEL = 1e-2  # same, for faces that share three or four corners
PLANE = 2e-3         # distance of every vertex from the other face's plane
MARGIN = 1e-4        # overlap must exceed this: shared edges do not count


BAKED_LIGHT, SCENERY_CLASSES = 1, 2
LIVE_SPECULAR = 16      # meshpack.h NM_PACK_LIVE_SPECULAR
# --live-specular: faces with an effective specular term are baked without it (module text).
ACCEPT_SPECULAR = False


def live_specular(mode, material):
    """The face's light has an effective specular term (lighting.h mode 1): it follows the view."""
    _, _, _, _, diffuse, ambient, scale, control, _, _ = material
    return bool(mode & 1 and control and scale and ambient < 255)
SCENERY_POINT = 0x200   # meshpack.h NM_SCENERY_POINT: opaque pieces of split cutouts
STAGE_REDUCED, STAGE_OPAQUE, STAGE_GROUND, STAGE_CUTOUT, STAGE_CHECKER = range(5)
SCENERY_SURFACE = 0x100
# Road decals (module text). The lift is 1+240/1024 = 1.234 in written depth:
# a decal wins over the road while it lies less than 0.23 camera heights
# below it (the largest measured need on course 0 is 0.16), and a car still
# wins over the decal unless it sinks 0.62 camera heights below it.
ROAD_DECAL_REACH = 2.0
ROAD_DECAL_LAYER = 240


def static_lumas(pack, light):
    """(front, back) luma per face in source order: see the module text.

    Single precision in the C expression order of nm_face_lighting_eval:
    dotl=(n0*l0+n1*l1)+n2*l2; luminance=|dotl|*diffuse+ambient, saturated at
    255, truncated. Each product or sum of two float32 values is exact in a
    double or far from a float32 rounding boundary, so rounding it once to
    float32 is the float32 operation. back is 0 for one-sided faces.
    """
    def f32(value):
        return struct.unpack('<f', struct.pack('<f', value))[0]
    light = [f32(v) for v in light]
    materials = [pm.MATERIAL.unpack_from(pack['materials'], m*pm.MATERIAL.size) for m in range(pack['header'][5])]
    result = []
    for mesh in pack['meshes']:
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = pack['faces'][f]
            _, _, _, _, diffuse, ambient, scale, control, _, _ = materials[face[4]]
            if mesh[10] > 1:
                raise ValueError(f'face {f}: computed normals light per view; it cannot be baked')
            if mesh[10] & 1 and control and ambient < 255 and not (ACCEPT_SPECULAR and scale):
                raise ValueError(f'face {f}: specular below saturated ambient depends on the view; it cannot be baked'
                                 ' (--live-specular: the renderer adds it)')
            n = [f32(v) for v in face[8:11]]
            dotl = f32(f32(f32(n[0]*light[0])+f32(n[1]*light[1]))+f32(n[2]*light[2]))
            lit = f32(f32(abs(dotl)*f32(diffuse))+f32(ambient))
            dark = f32(ambient)
            lit, dark = (int(min(max(v, 0.0), 255.0)) for v in (lit, dark))
            front, back = (lit if dotl > 0 else dark), (lit if dotl < 0 else dark)
            result.append((front, back if face[7] & (1 << 17) else 0))
    return result


SCENERY_STAGE_OBJECTS = 5   # meshpack.h
WORLD_OBJECTS = 8           # NM_PACK_WORLD_OBJECTS
OBJECT_INSTANCE = 0x80000000


OBJECT_MESHES = 0x800   # meshpack.h, NM_OBJECT_MESHES: the instance stands for variant+1 consecutive meshes
OBJECT_PLACED = 0x200   # meshpack.h, NM_OBJECT_PLACED: the mesh is in model coordinates


def object_meshes(pack):
    """Meshes drawn by world-object instances (instance.padding[0]), every mesh of a MESHES run included."""
    count = len(pack['instances'])//pm.INSTANCE.size
    rows = [pm.INSTANCE.unpack_from(pack['instances'], i*pm.INSTANCE.size) for i in range(count)]
    out = set()
    for row in rows:
        word = row[-4]
        if word & OBJECT_INSTANCE:
            out.update(range(row[0], row[0]+(((word >> 16) & 0xff)+1 if word & OBJECT_MESHES else 1)))
    return out


def object_materials(pack):
    """(materials of world-object faces, materials of every other face)."""
    objects = object_meshes(pack)
    inside, outside = set(), set()
    for at, mesh in enumerate(pack['meshes']):
        for f in range(mesh[2], mesh[2]+mesh[3]):
            (inside if at in objects else outside).add(pack['faces'][f][4])
    return inside, outside


def scenery_classes(pack, reduced_headers, surface_ground=False):
    """Class word per material, the materials that are road decals (see the
    module text) and a report.

    The opaque material of a split cutout (pack['split_materials']) is its
    source material here: its faces count as the source's and it takes the
    source's class plus SCENERY_POINT. A material that draws a punch-through
    material with another copy of its texture (pack['variant_materials'],
    lod_groups.py) is its source material too and takes its class as it is."""
    headers = [struct.unpack_from('<4H', pack['materials'], m*pm.MATERIAL.size) for m in range(pack['header'][5])]
    split = pack.get('split_materials', {})
    variant = pack.get('variant_materials', {})
    converted = pack.get('converted_materials', {})
    if split or variant or converted:
        stands_for = {**split, **variant, **converted}
        faces = [face[:4]+(stands_for.get(face[4], face[4]),)+face[5:] for face in pack['faces']]
        pack = dict(pack, faces=faces)
    reduced = set()
    for wanted in reduced_headers:
        found = [m for m, header in enumerate(headers) if header == tuple(wanted)]
        if len(found) != 1:
            raise ValueError(f'reduced-view header {wanted} matches {len(found)} materials')
        reduced.add(found[0])
    up = defaultdict(int)   # bit 0: a road-up face, bit 1: any other face
    for mesh in pack['meshes']:
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = pack['faces'][f]
            n = struct.unpack('<3f', struct.pack('<3f', *face[8:11]))
            squared = _dot(n, n)
            # Same rule as nm_render_world_attach's geometric surface class.
            is_up = mesh[10] < 2 and squared > 0 and n[1] < 0 and n[1]*n[1] >= 0.5625*squared
            up[face[4]] |= 1 if is_up else 2
    classes, report = [], Counter()
    for m, header in enumerate(headers):
        cutout = ((header[0] >> 13) & 3) == 3
        if header[0] & 0x8000:   # Also the one the reduced list names: never drawn there.
            stage = STAGE_CHECKER
        elif m in reduced:
            stage = STAGE_REDUCED
        elif cutout:
            stage = STAGE_CUTOUT
        else:
            stage = STAGE_GROUND if up[m] == 1 else STAGE_OPAQUE
        # Reduced view: exactly the renderer's geometric rule, whatever the
        # list (the finish-line overlay is a punch-through decal on the road).
        surface = up[m] == 1 and (stage == STAGE_REDUCED or (stage == STAGE_GROUND and surface_ground))
        classes.append(stage | (SCENERY_SURFACE if surface else 0))
    positions = [v[:3] for v in pm.VERTEX.iter_unpack(pack['vertices'])]
    # Road apron: stage-2 ground that is level and joins the road surface edge
    # to edge (most of its faces share two corners with a road-surface face).
    road_corners = set()
    for mesh in pack['meshes']:
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = pack['faces'][f]
            if classes[face[4]] == (STAGE_REDUCED | SCENERY_SURFACE):
                road_corners.update(positions[mesh[0]+i] for i in face[:face[5]])
    joined, level = defaultdict(lambda: [0, 0]), defaultdict(lambda: True)
    for mesh in pack['meshes']:
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = pack['faces'][f]
            if classes[face[4]] != STAGE_GROUND:
                continue
            n = face[8:11]
            level[face[4]] &= n[1]*n[1] >= 0.98*_dot(n, n)
            joined[face[4]][0] += 1
            joined[face[4]][1] += sum(1 for i in face[:face[5]] if positions[mesh[0]+i] in road_corners) >= 2
    for m, (count, edges) in joined.items():
        if level[m] and 2*edges > count:
            classes[m] |= SCENERY_SURFACE
            report['road_apron_materials'] += 1
            report['road_apron_faces'] += count
    # Road decals: ground-lying faces of later stages over the road surface.
    road, candidates = [], defaultdict(list)
    for mesh_index, mesh in enumerate(pack['meshes']):
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = pack['faces'][f]
            m = face[4]
            is_road = classes[m] == (STAGE_REDUCED | SCENERY_SURFACE)
            if not is_road and not (up[m] == 1 and classes[m] & 255 in (STAGE_OPAQUE, STAGE_GROUND, STAGE_CUTOUT) and
                                    not classes[m] & SCENERY_SURFACE):
                continue
            p = [positions[mesh[0]+i] for i in face[:face[5]]]
            box = (min(q[0] for q in p), max(q[0] for q in p), min(q[2] for q in p), max(q[2] for q in p))
            if is_road:
                n = face[8:11]
                road.append((mesh_index, box, n, _dot(n, p[0])))
            else:
                candidates[m].append((mesh_index, box, [sum(q[a] for q in p)/len(p) for a in range(3)]))
    decals = set()
    for m, members in candidates.items():
        on_road = 0
        for mesh_index, box, centre in members:
            for other, rbox, n, offset in road:
                if other != mesh_index or rbox[0] > box[1] or rbox[1] < box[0] or rbox[2] > box[3] or rbox[3] < box[2]:
                    continue
                if not rbox[0] <= centre[0] <= rbox[1] or not rbox[2] <= centre[2] <= rbox[3]:
                    continue
                # Height of the road face's plane under the decal's centre.
                if abs(n[1]) > 1e-6 and abs(centre[1]-(offset-n[0]*centre[0]-n[2]*centre[2])/n[1]) <= ROAD_DECAL_REACH:
                    on_road += 1
                    break
        if 2*on_road > len(members):
            decals.add(m)
            classes[m] |= SCENERY_SURFACE
            report['road_decal_materials'] += 1
            report['road_decal_faces'] += len(members)
    # Bindings are keyed by header: materials sharing one must agree.
    by_header = defaultdict(list)
    for m, header in enumerate(headers):
        by_header[header].append(m)
    for members in by_header.values():
        stage = min(classes[m] & 255 for m in members)
        surface = all(classes[m] & SCENERY_SURFACE for m in members)
        for m in members:
            classes[m] = stage | (SCENERY_SURFACE if surface else 0)
        if not all(m in decals for m in members):
            decals.difference_update(members)
    # World objects (meshpack.h, NM_PACK_WORLD_OBJECTS): a stage of their own,
    # true depth, never a road decal. No material serves a cell and an object.
    inside, outside = object_materials(pack)
    if inside & outside:
        raise ValueError('a material is used by a scenery cell and by a world object')
    for m in inside:
        classes[m] = SCENERY_STAGE_OBJECTS
    decals.difference_update(inside)
    for new, old in split.items():
        classes[new] = classes[old] | SCENERY_POINT
        if old in decals:
            decals.add(new)
    for new, old in variant.items():
        classes[new] = classes[old]
        if old in decals:
            decals.add(new)
    for new, old in converted.items():   # pt_to_opaque.py: the source's class as it is
        classes[new] = classes[old]
        if old in decals:
            decals.add(new)
    for word in classes:
        report[f'stage{word & 255}'+('_surface' if word & SCENERY_SURFACE else '') +
               ('_split_opaque' if word & SCENERY_POINT else '')] += 1
    return classes, decals, dict(sorted(report.items()))


# Screen-checker panels as punch-through stipple (module text). The header is
# synthetic: textured punch-through, 128x128, with a marker in the luma-base
# word that no arcade header carries; word 3 keeps the panel's colour word.
STIPPLE_TEXELS_PER_UNIT = 32
STIPPLE_SIZE_BITS = 2 | 2 << 3      # 32<<2 = 128 texels each way
STIPPLE_MARK = 0x8000


def stipple_header(header):
    """Bits 8..9 of word 1 stay: the number of a sparser stipple texture
    (lod_groups.py, stipple variants); zero for the checkerboard."""
    return (0x6000 | STIPPLE_SIZE_BITS, STIPPLE_MARK | header[1] & 0x0300, header[2], header[3])


def stipple_panels(pack):
    """Turn every screen-checker material into a punch-through stipple one.

    In place: the material header becomes stipple_header() and the panels'
    vertices get texture coordinates (1/8 texel units, as every DNM vertex).
    A panel quad has corners (top A, bottom A, bottom B, top B): u is constant
    on the edges 0-1 and 2-3, v on 1-2 and 3-0. Panels of a fence share vertex
    records at their posts, so u and v are solved per vertex class: u runs
    along the fence by each panel's length and turns back at a post whenever
    it can (the stipple is its own mirror image), which keeps it inside the 16
    bits of a coordinate however long the fence is.
    """
    count = pack['header'][5]
    headers = [struct.unpack_from('<4H', pack['materials'], m*pm.MATERIAL.size) for m in range(count)]
    checker = {m for m, header in enumerate(headers) if header[0] & 0x8000}
    if not checker:
        return dict(panels=0)
    positions = [v[:3] for v in pm.VERTEX.iter_unpack(pack['vertices'])]
    vertices = bytearray(pack['vertices'])
    panels, mismatch, highest = 0, 0.0, 0
    for mesh in pack['meshes']:
        members = [f for f in range(mesh[2], mesh[2]+mesh[3]) if pack['faces'][f][4] in checker]
        if not members:
            continue
        own = {i for f in members for i in pack['faces'][f][:pack['faces'][f][5]]}
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = pack['faces'][f]
            if face[4] not in checker and own & set(face[:face[5]]):
                raise ValueError(f'face {f} shares a vertex record with a checker panel')
        parent = {}

        def find(key):
            while parent.setdefault(key, key) != key:
                parent[key] = parent[parent[key]]
                key = parent[key]
            return key

        def distance(a, b):
            return math.dist(positions[mesh[0]+a], positions[mesh[0]+b])
        for f in members:
            face = pack['faces'][f]
            if face[5] != 4:
                raise ValueError(f'checker panel {f} is not a quad')
            i = face[:4]
            for axis, pairs in (('u', ((i[0], i[1]), (i[2], i[3]))), ('v', ((i[1], i[2]), (i[3], i[0])))):
                for a, b in pairs:
                    parent[find((axis, a))] = find((axis, b))
        links = defaultdict(list)
        for f in members:
            i = pack['faces'][f][:4]
            for axis, a, b, length in (('u', i[0], i[2], 0.5*(distance(i[1], i[2])+distance(i[0], i[3]))),
                                       ('v', i[1], i[0], 0.5*(distance(i[0], i[1])+distance(i[2], i[3])))):
                a, b = find((axis, a)), find((axis, b))
                if a == b:
                    raise ValueError(f'checker panel {f}: corner order differs from its neighbours')
                links[a].append((b, length))
                links[b].append((a, length))
        value = {}
        for start in sorted(links):
            if start in value:
                continue
            value[start] = 0.0
            queue = [start]
            while queue:
                at = queue.pop()
                for other, length in links[at]:
                    if other in value:
                        mismatch = max(mismatch, abs(abs(value[other]-value[at])-length))
                        continue
                    value[other] = value[at]-length if value[at] >= length else value[at]+length
                    queue.append(other)
        for i in own:
            u, v = (int(round(value[find((axis, i))]*STIPPLE_TEXELS_PER_UNIT*8)) for axis in 'uv')
            if not 0 <= u <= 65535 or not 0 <= v <= 65535:
                raise ValueError('stipple coordinate out of range')
            highest = max(highest, u, v)
            struct.pack_into('<2H', vertices, (mesh[0]+i)*pm.VERTEX.size+12, u, v)
        # A level-of-detail group draws a panel with more than one material: one panel.
        panels += len({pack['faces'][f][:4] for f in members})
    materials = bytearray(pack['materials'])
    for m in checker:
        struct.pack_into('<4H', materials, m*pm.MATERIAL.size, *stipple_header(headers[m]))
    if len({bytes(materials[m*pm.MATERIAL.size:m*pm.MATERIAL.size+8]) for m in range(count)}) != len(set(headers)):
        raise ValueError('stipple header collides with a material of the pack')
    pack['vertices'], pack['materials'] = bytes(vertices), bytes(materials)
    return dict(panels=panels, materials=len(checker), texels_per_unit=STIPPLE_TEXELS_PER_UNIT,
                largest_coordinate=highest, largest_length_mismatch=mismatch)


# Flattened stacks (module text). Synthetic opaque header: the base's size and
# flip bits, a marker plus the composite's number in the luma-base word.
COMPOSITE_MARK = 0x4000


def flatten_stacks(pack, lumas, limit=None):
    """Replace each stack of one opaque face and punch-through faces drawn on
    exactly the same polygon by ONE opaque face of a composite material.

    A stack is a set of faces of one mesh with the same vertex records
    (position and texture coordinate of every corner) and supplied normal,
    all one-sided and drawable: in the arcade's order an opaque textured
    face first, then punch-through faces only, every texture of the same
    size and wrap. The punch-through faces cover nothing but that polygon,
    from every direction, so the picture is the base's texture with the
    others laid over it texel by texel: prepare_palette_materials.py
    --composites builds that texture (4-bit, one palette of its own). The
    base face takes the composite material; the others leave the pack.

    A composite needs a palette bank of its own. limit: flatten only the
    `limit` composites that remove the most face area (the rest are left as
    they are and reported as candidates).

    In place: faces, mesh ranges, header counts, materials; pack['origin']
    maps the remaining faces to source face indices. `lumas` is the baked
    (front, back) table of the source faces. Returns the composite
    descriptions for the material packer and a report.
    """
    size = pm.MATERIAL.size
    headers = [struct.unpack_from('<4H', pack['materials'], m*size) for m in range(pack['header'][5])]
    faces = pack['faces']
    origin = pack.setdefault('origin', list(range(len(faces))))
    positions = [v[:3] for v in pm.VERTEX.iter_unpack(pack['vertices'])]
    found, variants = [], defaultdict(list)
    report = Counter()
    for mesh in pack['meshes']:
        by_polygon = defaultdict(list)
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = faces[f]
            records = tuple(sorted(pack['vertices'][(mesh[0]+i)*pm.VERTEX.size:(mesh[0]+i+1)*pm.VERTEX.size]
                                   for i in face[:face[5]]))
            by_polygon[records, struct.pack('<3f', *face[8:11])].append(f)
        for members in by_polygon.values():
            if len(members) < 2 or any(faces[f][7] & (1 << 17) or not faces[f][7] & (3 << 8) for f in members):
                continue
            if any(live_specular(mesh[10], pm.MATERIAL.unpack_from(pack['materials'], faces[f][4]*size)) for f in members):
                report['stacks_left_live_specular'] += 1
                continue
            layers = [headers[faces[f][4]] for f in members]
            if (layers[0][0] >> 13) & 7 != 2 or any((h[0] >> 13) & 7 != 3 for h in layers[1:]):
                continue   # opaque textured base, punch-through on top; never a screen-checker face
            if len({h[0] & 0x33f for h in layers}) != 1 or any(h[1] & 0xff00 for h in layers):
                report['stacks_left_size_or_wrap_differs'] += 1
                continue
            light = tuple(lumas[origin[f]][0] for f in members)
            key = tuple(layers)
            for index in variants[key]:
                if found[index]['lumas'].setdefault(light[0], light) == light:
                    break
            else:
                index = len(found)
                found.append(dict(layers=key, lumas={light[0]: light}, stacks=[], area=0.0))
                variants[key].append(index)
            p = [positions[mesh[0]+i] for i in faces[members[0]][:faces[members[0]][5]]]
            area = 0.0
            for i in range(1, len(p)-1):
                a, b = [p[i][k]-p[0][k] for k in range(3)], [p[i+1][k]-p[0][k] for k in range(3)]
                area += 0.5*math.sqrt((a[1]*b[2]-a[2]*b[1])**2+(a[2]*b[0]-a[0]*b[2])**2+(a[0]*b[1]-a[1]*b[0])**2)
            found[index]['stacks'].append(members)
            found[index]['area'] += area*(len(members)-1)
    ranked = sorted(range(len(found)), key=lambda i: (-found[i]['area'], i))
    chosen = sorted(ranked if limit is None else ranked[:limit])
    report['composite_candidates_left'] = len(found)-len(chosen)
    candidates = [dict(layers=[list(h) for h in found[i]['layers']], stacks=len(found[i]['stacks']),
                       faces_removable=sum(len(m)-1 for m in found[i]['stacks']), area_removable=round(found[i]['area'], 1))
                  for i in ranked if i not in chosen]
    if len(chosen) > 0x100:
        raise ValueError('more than 256 composite materials')
    materials = bytearray(pack['materials'])
    composites, drop, removed = [], set(), []
    for number, index in enumerate(chosen):
        stack = found[index]
        base = faces[stack['stacks'][0][0]][4]
        first = stack['layers'][0]
        header = (0x4000 | first[0] & 0x33f, COMPOSITE_MARK | number, first[2], first[3])
        if header in headers:
            raise ValueError('composite header collides with a material of the pack')
        record = bytearray(materials[base*size:(base+1)*size])
        struct.pack_into('<4H', record, 0, *header)
        material = len(materials)//size
        materials += record
        for members in stack['stacks']:
            if materials[faces[members[0]][4]*size+8:(faces[members[0]][4]+1)*size] != record[8:]:
                raise ValueError('bases of one composite differ in their light terms')
            face = faces[members[0]]
            faces[members[0]] = face[:4]+(material,)+face[5:]
            drop.update(members[1:])
            removed += [dict(face=origin[f], under=origin[members[0]], header=list(headers[faces[f][4]]),
                             triangles=faces[f][5]-2) for f in members[1:]]
        composites.append(dict(header=list(header), layers=[list(h) for h in stack['layers']],
                               faces=len(stack['stacks']), area_removed=round(stack['area'], 1),
                               lumas={str(k): list(v) for k, v in sorted(stack['lumas'].items())}))
        report['stacks'] += len(stack['stacks'])
    if not drop:
        return [], dict(report, candidates=candidates)
    kept, meshes = [], []
    for mesh in pack['meshes']:
        mesh = list(mesh)
        members = [f for f in range(mesh[2], mesh[2]+mesh[3]) if f not in drop]
        mesh[2], mesh[3] = len(kept), len(members)
        kept += members
        meshes.append(mesh)
    pack['faces'] = [faces[f] for f in kept]
    pack['origin'] = [origin[f] for f in kept]
    pack['meshes'] = meshes
    pack['materials'] = bytes(materials)
    pack['header'][4], pack['header'][5] = len(kept), len(materials)//size
    report.update(faces_removed=len(drop), triangles_removed=sum(r['triangles'] for r in removed),
                  composite_materials=len(composites))
    return composites, dict(report, candidates=candidates, removed=removed)


def read_pack(data):
    """Tables of a validated legacy or aligned DNM2/4/5 pack."""
    h = list(pm.HEADER.unpack_from(data))
    magic, aligned = h[0] & ~pm.ALIGNED, bool(h[0] & pm.ALIGNED)
    if magic == LAYERED:
        raise ValueError('pack is already DNM6: its source face order is gone')
    if magic not in (0x324d4e44, 0x344d4e44, 0x354d4e44) or h[1] != len(data):
        raise ValueError('face ordering requires a prepared DNM2, DNM4 or DNM5 pack')
    offset = pm.HEADER.size
    tables = []

    def take(count, stride, legacy_alignment=1):
        nonlocal offset
        offset = (offset+(31 if aligned else legacy_alignment-1)) & -(32 if aligned else legacy_alignment)
        tables.append(data[offset:offset+count*stride])
        if len(tables[-1]) != count*stride:
            raise ValueError('truncated DNM table')
        offset += count*stride
    for count, record in zip(h[2:7], (pm.MESH, pm.VERTEX, pm.FACE, pm.MATERIAL, pm.INSTANCE)):
        take(count, record.size)
    take(h[3], 2)
    groups = b''
    if magic != 0x324d4e44:
        take(h[7], pm.GROUP.size, 4)
        groups = tables.pop()
    meshes, vertices, faces, materials, instances, refs = tables
    return dict(
        header=h, aligned=aligned,
        meshes=[list(pm.MESH.unpack_from(meshes, i*pm.MESH.size)) for i in range(h[2])],
        vertices=vertices, materials=materials, instances=instances, refs=refs, groups=groups,
        faces=[pm.FACE.unpack_from(faces, i*pm.FACE.size) for i in range(h[4])])


def intro_part_lists(header_text):
    """Part lists that one car draws with ONE matrix, in the arcade's order.

    Source of the rules: nm_practice_render_parts and nm_player_parts
    (code/render/scene/practice.c, include/player_parts.h). An opponent at
    detail level 4/3 draws body, window, combined wheels (phase model); at
    level 2/1 the crash attachment precedes the body. The player draws body
    95ff9d, glass 13b, cutout shell 961bbf; its wheels and every shadow have
    their own pose and cannot be coplanar with the body by construction.
    """
    def table(name):
        match = re.search(name+r'\[\]\s*=\s*\{([^}]*)\}', header_text)
        if not match:
            raise ValueError(f'generated header lacks {name}')
        return [int(v) for v in match.group(1).split(',')]
    ids = table('nm_intro_model_ids')
    windows, phases, crash = (table('nm_intro_'+n) for n in ('window_models', 'wheel_phase_models', 'crash_models'))
    body = re.search(r'nm_intro_body_models\[\]\[8\]\s*=\s*\{(.*?)\};', header_text, re.S)
    special = re.search(r'nm_intro_special_model\s*=\s*(\d+)', header_text)
    if not body or not special or len(windows) != 2 or len(phases) != 16 or len(crash) != 2:
        raise ValueError('generated header does not describe the car part tables')
    rows = [[int(v) for v in row.split(',')] for row in re.findall(r'\{([^{}]*)\}', body.group(1))]
    if not rows or any(len(row) != 8 for row in rows):
        raise ValueError('unexpected body model table')
    lists = set()
    # The special actor (kind 2) uses its own body binding at any level.
    for row in rows+[[int(special.group(1))]*8]:
        for level in (4, 3):
            for phase in phases[(4-level)*8:(4-level)*8+8]:
                lists.add((row[6-level], windows[4-level], phase))
        for level in (2, 1):
            lists.add((crash[level-1], row[6-level]))
    result = [[ids[b] for b in part_list] for part_list in sorted(lists)]
    result.append([0x95ff9d, 0x13b, 0x961bbf])
    return result


def _dot(a, b):
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]


def _polygons(pack):
    """Per face: corner tuples (float32 values), unit geometric normal or None."""
    positions = [v[:3] for v in pm.VERTEX.iter_unpack(pack['vertices'])]
    polygons, normals = [], []
    for mesh in pack['meshes']:
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = pack['faces'][f]
            p = [positions[mesh[0]+i] for i in face[:face[5]]]
            # Newell: also right for quads whose first corner is reflex.
            n = [0.0, 0.0, 0.0]
            for a, b in zip(p, p[1:]+p[:1]):
                n[0] += a[1]*b[2]-a[2]*b[1]
                n[1] += a[2]*b[0]-a[0]*b[2]
                n[2] += a[0]*b[1]-a[1]*b[0]
            length = math.sqrt(_dot(n, n))
            polygons.append(p)
            normals.append(tuple(v/length for v in n) if length > 1e-12 else None)
    return polygons, normals


def _overlap(a, b):
    """Convex polygons in 2D share more than an edge (separating axes)."""
    for p, q in ((a, b), (b, a)):
        for i in range(len(p)):
            ex, ey = p[(i+1) % len(p)][0]-p[i][0], p[(i+1) % len(p)][1]-p[i][1]
            length = math.hypot(ex, ey)
            if length < 1e-12:
                continue
            ax, ay = -ey/length, ex/length
            pa = [v[0]*ax+v[1]*ay for v in p]
            pb = [v[0]*ax+v[1]*ay for v in q]
            if max(pa) <= min(pb)+MARGIN or max(pb) <= min(pa)+MARGIN:
                return False
    return True


def find_layers(pack, part_lists=(), cross_mesh_by_area=False, unpaired=None):
    """Layer per face, a report, and the evidence: [under, over, plane gap,
    same-corner dip] per ordered face pair (source face indices).

    Level-of-detail groups (pack['lod_face'], lod_groups.py): the shapes of
    one group lie in one plane on top of each other but are never drawn
    together, so two faces of different levels of one group are no pair.
    Every shape is paired with the faces outside its group like any face, so
    whichever shape is drawn lies above what the polygon lay on and below
    what lay on it.

    cross_mesh_by_area (static scenery, every mesh under one view matrix):
    faces of two different meshes that lie in one plane and overlap have no
    draw order to go by (the arcade's cell order changes with the view), so
    the smaller face is taken as the detail lying on the larger one. Two
    variants of one selection run of world objects (meshpack.h,
    NM_OBJECT_RUN: same cell, near flag and kind, a selector kind, different
    variants: the die's 256 poses, the leader board's numbers) are never
    drawn together and are no pair either; nor is a PLACED object mesh (model
    coordinates: its world position is the instance's) with any other mesh.

    unpaired (--drape-road-decals): source faces of the road decals
    (road_drape.py); no such face is paired with a face outside the set:
    their lift above the road is set after the classes (sort_pack)."""
    polygons, normals = _polygons(pack)
    faces, meshes = pack['faces'], pack['meshes']
    checker = {m for m in range(pack['header'][5])
               if struct.unpack_from('<H', pack['materials'], m*pm.MATERIAL.size)[0] & 0x8000}
    drawable = [normals[f] is not None and faces[f][4] not in checker for f in range(len(faces))]
    supplied = [face[8:11] for face in faces]
    one_sided = [not face[7] & (1 << 17) for face in faces]
    low = [tuple(min(q[a] for q in p)-PLANE for a in range(3)) for p in polygons]
    high = [tuple(max(q[a] for q in p)+PLANE for a in range(3)) for p in polygons]
    offset = [_dot(normals[f], polygons[f][0]) if normals[f] else 0.0 for f in range(len(faces))]
    gaps, dips, detail = Counter(), [], {}
    corners = [set(p) for p in polygons]
    lod = pack.get('lod_face') or [None]*len(faces)
    # "Drawn before" inside a mesh: source order. A face's source face (pack['origin'], several pieces of one
    # source face in their order); a multi-face level-of-detail group (lod_groups.py, supersample runs) moves
    # its faces next to each other, and this keeps their relations to the faces in between. Wherever faces
    # keep their source order (every pack without such groups) this is the face order itself.
    origin = pack.get('origin') or range(len(faces))
    rank = {f: (origin[f], f) for f in range(len(faces))}

    def pairs(under, over, same_mesh):
        """Faces of `over` lying on a face of `under` drawn before them."""
        found = []
        under = [f for f in under if drawable[f]]
        if same_mesh:
            under.sort(key=rank.__getitem__)
        for j in over:
            if not drawable[j]:
                continue
            lj, hj, nj = low[j], high[j], normals[j]
            for i in under:
                if same_mesh and rank[i] >= rank[j]:
                    break
                if lod[j] is not None and lod[i] is not None and lod[i][0] == lod[j][0] and lod[i][1] != lod[j][1]:
                    continue
                if unpaired is not None and (origin[i] in unpaired) != (origin[j] in unpaired):
                    continue
                li, hi = low[i], high[i]
                # Bounds already carry PLANE of slack on each side.
                if (li[0] > hj[0] or hi[0] < lj[0] or li[1] > hj[1] or hi[1] < lj[1] or
                        li[2] > hj[2] or hi[2] < lj[2]):
                    continue
                ni = normals[i]
                cos = abs(_dot(ni, nj))
                if cos < 1-SHARED_PARALLEL:
                    continue
                # Never both visible: one-sided faces whose supplied normals oppose.
                if one_sided[i] and one_sided[j] and _dot(supplied[i], supplied[j]) < 0:
                    continue
                shared = len(corners[i] & corners[j])
                if cos < 1-PARALLEL and shared < 3:
                    continue
                gap = max(max(abs(_dot(q, ni)-offset[i]) for q in polygons[j]),
                          max(abs(_dot(q, nj)-offset[j]) for q in polygons[i]))
                if gap > 0.25:
                    continue
                axis = max(range(3), key=lambda a: abs(ni[a]))
                u, v = [a for a in range(3) if a != axis]
                if not _overlap([(q[u], q[v]) for q in polygons[i]], [(q[u], q[v]) for q in polygons[j]]):
                    continue
                gaps[('shared ' if shared >= 3 else 'apart ')+'%.0e' % max(gap, 1e-7)] += 1
                if gap > PLANE and shared < 3:
                    continue
                dip = 0.0
                if shared == 4 and len(polygons[i]) == 4 and len(polygons[j]) == 4:
                    # Same quad on the other fan diagonal: the two surfaces
                    # part by the distance between the diagonals' midpoints.
                    a, b = polygons[i], polygons[j]
                    normal = supplied[i]
                    across = [b[0][k]+b[2][k]-a[0][k]-a[2][k] for k in range(3)]
                    lift = _dot(across, normal)*-0.5/max(math.sqrt(_dot(normal, normal)), 1e-12)
                    if lift < -1e-5:   # toward the visible side is -normal
                        dip = -lift
                        dips.append(dip)
                found.append((i, j))
                detail[i, j] = (gap, dip)
        return found

    edges = []
    for mesh in meshes:
        members = range(mesh[2], mesh[2]+mesh[3])
        edges += pairs(members, members, True)
    within = len(edges)
    owner = {}
    for i in range(pack['header'][6]):
        inst = pm.INSTANCE.unpack_from(pack['instances'], i*pm.INSTANCE.size)
        if inst[-4] & OBJECT_INSTANCE:
            continue   # a world-object mesh may hold several actors of one model; never a car part
        if owner.setdefault(inst[1], inst[0]) != inst[0]:
            raise ValueError('one source model is bound to two meshes')
    done = set()
    for part_list in part_lists:
        missing = [a for a in part_list if a not in owner]
        if missing:
            raise ValueError('part list names models absent from the pack: '+', '.join(map(hex, missing)))
        for under, over in itertools.combinations([owner[a] for a in part_list], 2):
            if under == over or (under, over) in done:
                continue
            if (over, under) in done:
                raise ValueError('two part lists draw the same meshes in opposite order')
            done.add((under, over))
            a, b = meshes[under], meshes[over]
            edges += pairs(range(a[2], a[2]+a[3]), range(b[2], b[2]+b[3]), False)
    across = 0
    if cross_mesh_by_area:
        exclusive = {}   # mesh -> (run, variant) for the variants of selector kinds and the meshes of MESHES runs
        placed_meshes = set()
        for i in range(pack['header'][6]):
            inst = pm.INSTANCE.unpack_from(pack['instances'], i*pm.INSTANCE.size)
            word = inst[-4]
            if word & OBJECT_INSTANCE and word & OBJECT_MESHES:
                for k in range(((word >> 16) & 0xff)+1):
                    exclusive[inst[0]+k] = (('meshes', inst[0]), k)
            elif word & OBJECT_INSTANCE and (word >> 12) & 15:
                exclusive[inst[0]] = (word & 0xf1ff, word)
            if word & OBJECT_INSTANCE and word & OBJECT_PLACED:   # model coordinates: no world-space neighbours
                placed_meshes.update(range(inst[0], inst[0]+(((word >> 16) & 0xff)+1 if word & OBJECT_MESHES else 1)))

        def area(f):
            p = polygons[f]
            total = 0.0
            for i in range(1, len(p)-1):
                a = [p[i][k]-p[0][k] for k in range(3)]
                b = [p[i+1][k]-p[0][k] for k in range(3)]
                total += 0.5*math.sqrt((a[1]*b[2]-a[2]*b[1])**2+(a[2]*b[0]-a[0]*b[2])**2+(a[0]*b[1]-a[1]*b[0])**2)
            return total
        for (ia, a), (ib, b) in itertools.combinations(enumerate(meshes), 2):
            wa, wb = exclusive.get(ia), exclusive.get(ib)
            if (wa is not None and wb is not None and wa[0] == wb[0] and wa[1] != wb[1]) or ia in placed_meshes or ib in placed_meshes:
                continue
            for i, j in pairs(range(a[2], a[2]+a[3]), range(b[2], b[2]+b[3]), False):
                if area(i) < area(j):
                    detail[j, i] = detail.pop((i, j))
                    i, j = j, i
                edges.append((i, j))
                across += 1
    layer = [0]*len(faces)
    for _ in range(256):
        changed = False
        for i, j in edges:
            if layer[j] <= layer[i]:
                layer[j], changed = layer[i]+1, True
        if not changed:
            break
    else:
        raise ValueError('coplanar draw order is cyclic or deeper than 255 layers')
    dip_report = dict(count=len(dips), deepest=max(dips, default=0.0))
    for limit in (0.002, 0.005, 0.01, 0.02):
        dip_report[f'deeper_than_{limit}'] = sum(1 for d in dips if d > limit)
    return layer, dict(pairs_within_mesh=within, pairs_across_parts=len(edges)-within-across,
                       pairs_across_meshes_by_area=across,
                       mesh_pairs_tested=len(done), plane_gap=dict(sorted(gaps.items())),
                       same_corner_dips=dip_report,
                       faces_by_layer=dict(sorted(Counter(layer[f] for f in range(len(faces)) if drawable[f]).items()))), \
        [[i, j, *detail[i, j]] for i, j in edges]


def match_decal_diagonals(pack, layer, pairs):
    """Rotate dipping same-corner decal quads onto their base's fan diagonal.

    In place. The fan (0,1,2),(0,2,3) splits a quad along corners 0-2. Only
    decals that lie below their base are turned (find_layers' dip); flat
    pairs and decals that stand proud are already right. Lower layers are
    settled first; a face tied to one base is not turned again for another.
    Returns (rotated faces, conflicts left as they are).
    """
    faces = pack['faces']
    first_vertex = {}
    for mesh in pack['meshes']:
        for f in range(mesh[2], mesh[2]+mesh[3]):
            first_vertex[f] = mesh[0]

    def corner(f, i):
        at = (first_vertex[f]+faces[f][i])*pm.VERTEX.size
        return pack['vertices'][at:at+12]
    settled, rotated, conflicts = set(), set(), 0
    for under, over, _, dip in sorted(pairs, key=lambda pair: (layer[pair[1]], pair[1], layer[pair[0]], pair[0])):
        if not dip > 0 or faces[under][5] != 4 or faces[over][5] != 4:
            continue
        if {corner(under, i) for i in range(4)} != {corner(over, i) for i in range(4)}:
            continue
        if {corner(over, 0), corner(over, 2)} != {corner(under, 0), corner(under, 2)}:
            if over in settled:
                conflicts += 1
                continue
            # One corner on, or one back: both give the base's diagonal. Take
            # the turn that also puts the base's corner 2 at corner 2, the
            # point the renderer's facing test uses, so both cull alike.
            face = faces[over]
            turn = 1 if corner(over, 3) == corner(under, 2) else 3
            faces[over] = tuple(face[(i+turn) % 4] for i in range(4))+tuple(face[4:])
            rotated.add(over)
        settled.add(over)
    return sorted(rotated), conflicts


def _strip_triangles(strip, parity):
    return [pm_oriented((strip[i+1], strip[i], strip[i+2]) if (i+parity) & 1 else (strip[i], strip[i+1], strip[i+2]))
            for i in range(len(strip)-2)]


def pm_oriented(triangle):
    a, b, c = triangle
    return min((a, b, c), (b, c, a), (c, a, b))


def _options(corners):
    """Strip vertex orders of one face per start parity: fan order kept."""
    target = [pm_oriented((corners[0], corners[i], corners[i+1])) for i in range(1, len(corners)-1)]
    return {parity: [s for s in itertools.permutations(corners) if _strip_triangles(s, parity) == target]
            for parity in (0, 1)}


def chain_faces(corner_lists):
    """Greedy longest whole-face chains. corner_lists: {face: vertex ids}.

    Returns [(faces in strip order, strip vertex ids)], every face once.
    ponytail: not an optimal stripification; each round keeps the longest
    walk over all start faces and orientations.
    """
    options, starts = {}, defaultdict(list)
    for face, corners in corner_lists.items():
        options[face] = _options(corners) if len(set(corners)) == len(corners) else {0: [], 1: []}
        for parity, strips in options[face].items():
            for strip in strips:
                starts[parity, strip[0], strip[1]].append((face, strip))
    unused = set(corner_lists)
    order = sorted(corner_lists)
    chains = []
    while unused:
        best = None
        for first in order:
            if first not in unused:
                continue
            for start in options[first][0] or [None]:
                if start is None:
                    walk = ([first], None)
                else:
                    ids, strip, seen = [first], list(start), {first}
                    while len(strip) < 65000:
                        step = next(((f, s) for f, s in starts[(len(strip)-2) & 1, strip[-2], strip[-1]]
                                     if f in unused and f not in seen), None)
                        if step is None:
                            break
                        ids.append(step[0]); seen.add(step[0]); strip += step[1][2:]
                    walk = (ids, strip)
                if best is None or len(walk[0]) > len(best[0]):
                    best = walk
            if best is not None and len(best[0]) == len(unused):
                break
        unused.difference_update(best[0])
        chains.append(best)
    return chains


def material_ranks(pack, materials):
    """Sort rank per mesh material: its most used texture resource in a DNT pack.

    Bindings of one material header differ by luma and may use a few texture
    resources; the PVR header changes with the resource, not the material.
    """
    magic, size, resources, count = struct.unpack_from('<4s3I', materials)
    if magic not in (b'DNT2', b'DNT4', b'DNT6') or size != len(materials):
        raise ValueError('material ranking requires an uncompressed DNT2/DNT4/DNT6 pack')
    base = 32+16*resources
    use = defaultdict(Counter)
    for i in range(count):
        binding = struct.unpack_from('<6HI', materials, base+16*i)
        use[binding[:4]][binding[5], binding[0] & 0x300] += 1
    keys = []
    for m in range(pack['header'][5]):
        header = struct.unpack_from('<4H', pack['materials'], m*pm.MATERIAL.size)
        if m in pack.get('split_materials', ()):   # ranked with its source's texture
            header = struct.unpack_from('<4H', pack['materials'], pack['split_materials'][m]*pm.MATERIAL.size)
        if m in pack.get('variant_materials', ()):   # as its source: the copy is a neighbour in VRAM terms only
            header = struct.unpack_from('<4H', pack['materials'], pack['variant_materials'][m]*pm.MATERIAL.size)
        if m in pack.get('converted_materials', ()):   # pt_to_opaque.py: same texture resource as its source
            header = struct.unpack_from('<4H', pack['materials'], pack['converted_materials'][m]*pm.MATERIAL.size)
        keys.append(max(use[header].items(), key=lambda item: (item[1], item[0]))[0] if header in use else (1 << 20, 0))
    order = {key: rank for rank, key in enumerate(sorted(set(keys)))}
    return [order[key] for key in keys]


def _normal_key(face):
    """Vertical light term first, then azimuth: see the module text."""
    n = face[8:11]
    return (round(float(n[1]), 6), round(math.atan2(n[2], n[0]), 6), struct.pack('<3f', *n))


def order_faces(pack, layer, strip_scope='normal', material_rank=None):
    """New face order per mesh, strips, and new->old absolute face indices.

    Level-of-detail groups (pack['lod_face']): the faces outside every group
    are ordered as always; behind them come the groups, each as its levels
    in a row, a level sorted and chained into strips by the same keys on its
    own. Groups whose nearest shape uses the same materials are neighbours,
    so the queues of a list change header as seldom as the levels allow."""
    faces, meshes = pack['faces'], pack['meshes']
    lod = pack.get('lod_face') or [None]*len(faces)
    header0 = [struct.unpack_from('<H', pack['materials'], m*pm.MATERIAL.size)[0] for m in range(pack['header'][5])]
    canonical = {}
    order, strips, indices, mesh_strips = [], [], [], []
    for mesh in meshes:
        vertex_id = []
        for v in range(mesh[1]):
            raw = pack['vertices'][(mesh[0]+v)*pm.VERTEX.size:(mesh[0]+v+1)*pm.VERTEX.size]
            vertex_id.append(canonical.setdefault((mesh[0], raw), v))
        runs, grouped = defaultdict(list), defaultdict(lambda: defaultdict(list))
        for f in range(mesh[2], mesh[2]+mesh[3]):
            face = faces[f]
            th = header0[face[4]]
            rank = material_rank[face[4]] if material_rank else face[4]
            key = (bool(th & 0x8000), ((th >> 13) & 3) == 3, rank, face[4], layer[f])
            key += _normal_key(face) if strip_scope == 'normal' else ()
            if lod[f] is None:
                runs[(0, 0, 0)+key].append(f)
            else:
                grouped[lod[f][0]][lod[f][1]].append((key, f))
        # Groups by the materials of their first shape that has faces, then source order.
        def group_key(group):
            first = next((grouped[group][level] for level in sorted(grouped[group]) if grouped[group][level]), [])
            return (sorted({key[:4] for key, _ in first}), group)
        for at, group in enumerate(sorted(grouped, key=group_key)):
            for level, members in grouped[group].items():
                for key, f in members:
                    runs[(1, at, level)+key].append(f)
        first_strip = len(strips)
        for key in sorted(runs):
            members = runs[key]
            key = key[3:]
            if key[0]:  # Checker faces are never drawn: keep source order.
                order += members
                continue
            chains = chain_faces({f: tuple(vertex_id[i] for i in faces[f][:faces[f][5]]) for f in members})
            # By normal first (one normal per run in the default scope; in the
            # material scope faces that light alike, hence share a palette bank
            # or texture variant, stay together), then longest strips first;
            # singles keep source order.
            chains.sort(key=lambda chain: (_normal_key(faces[chain[0][0]]), -len(chain[0]), chain[0][0]))
            for ids, strip in chains:
                if len(ids) > 1:
                    strips.append((len(order), len(ids), len(strip), len(indices)))
                    indices += strip
                order += ids
        mesh_strips.append((first_strip, len(strips)-first_strip))
    if sorted(order) != list(range(len(faces))):
        raise AssertionError('face order is not a permutation')
    return order, strips, indices, mesh_strips


def write_pack(pack, order, layer, strips, indices, mesh_strips, lumas=None, classes=None):
    """Legacy-layout DNM6 bytes (use prepare_models.align_tables afterwards)."""
    h = list(pack['header'])
    grouped = bool(pack['groups'])
    lod_table, lod_meshes = b'', None
    if pack.get('lod'):
        import lod_groups
        lod_table, lod_meshes, pack['lod_written'] = lod_groups.table(pack, order, strips)
    meshes = []
    for at, (mesh, (first, count)) in enumerate(zip(pack['meshes'], mesh_strips)):
        mesh = list(mesh)
        mesh[13], mesh[14] = (first, count) if strips else (0, 0)
        if lod_meshes:
            mesh[15] = lod_meshes[at][0] | lod_meshes[at][1] << 16
        meshes.append(pm.MESH.pack(*mesh))
    faces = []
    for old in order:
        face = pack['faces'][old]
        if face[6] or face[11] > 65535 or (not grouped and face[11]):
            raise ValueError('unexpected face padding in source pack')
        front, back = lumas[old] if lumas else (0, 0)
        faces.append(FACE6.pack(*face[:6], front, *face[7:11], face[11], layer[old], back))
    materials = pack['materials']
    if classes is not None:
        materials = bytearray(materials)
        for m, word in enumerate(classes):
            if struct.unpack_from('<2I', materials, m*pm.MATERIAL.size+24) != (0, 0):
                raise ValueError('unexpected material padding in source pack')
            struct.pack_into('<I', materials, m*pm.MATERIAL.size+24, word)
        # Super-sample switch depths of world-object materials (lod_groups.py, "group": "instance"): word padding[1].
        for m, depth in sorted(pack.get('supersample_depths', {}).items()):
            if not depth > 0 or (classes[m] & 0xff) != SCENERY_STAGE_OBJECTS:
                raise ValueError(f'material {m}: a super-sample switch depth belongs to a world-object material')
            struct.pack_into('<f', materials, m*pm.MATERIAL.size+28, depth)
        materials = bytes(materials)
    elif pack.get('supersample_depths'):
        raise ValueError('super-sample switch depths need --scenery-classes')
    out = bytearray(pm.HEADER.size)
    out += b''.join(meshes)+pack['vertices']+b''.join(faces)+materials+pack['instances']+pack['refs']
    out += bytes((-len(out)) & 3)
    out += pack['groups']
    if strips:
        out += bytes((-len(out)) & 31)
        out += b''.join(STRIP.pack(first, count, vertices, at, 0, 0, 0, 0, 0) for first, count, vertices, at in strips)
        out += struct.pack(f'<{len(indices)}H', *indices)
    out += bytes((-len(out)) & 3)
    out += lod_table
    if len(out) > pm.MAX_BYTES:
        raise ValueError('ordered pack exceeds resident geometry ceiling; split offline')
    h[0], h[1], h[7] = LAYERED, len(out), len(pack['groups'])//pm.GROUP.size
    h[12], h[13], h[14] = len(strips), len(indices), max(layer)+1
    h[15] = (BAKED_LIGHT if lumas else 0) | (SCENERY_CLASSES if classes is not None else 0)
    if lumas and any(live_specular(mesh[10], pm.MATERIAL.unpack_from(materials, pack['faces'][f][4]*pm.MATERIAL.size))
                     for mesh in pack['meshes'] for f in range(mesh[2], mesh[2]+mesh[3])):
        h[15] |= LIVE_SPECULAR
    if object_meshes(pack):
        if not lumas or classes is None:
            raise ValueError('world objects need --static-light and --scenery-classes')
        h[15] |= WORLD_OBJECTS
    if lod_table:
        h[15] |= lod_groups.PACK_LOD_GROUPS | (len(lod_table)//lod_groups.LOD.size) << lod_groups.LOD_COUNT_SHIFT
    pm.HEADER.pack_into(out, 0, *h)
    return bytes(out)


def material_runs(pack, order):
    """Static material changes per mesh and list (checker faces excluded)."""
    header0 = [struct.unpack_from('<H', pack['materials'], m*pm.MATERIAL.size)[0] for m in range(pack['header'][5])]
    runs = floor = at = 0
    for mesh in pack['meshes']:
        members = order[at:at+mesh[3]]
        at += mesh[3]
        for cutout in (False, True):
            sequence = [pack['faces'][f][4] for f in members
                        if not header0[pack['faces'][f][4]] & 0x8000 and
                        (((header0[pack['faces'][f][4]] >> 13) & 3) == 3) == cutout]
            runs += sum(1 for i, m in enumerate(sequence) if not i or sequence[i-1] != m)
            floor += len(set(sequence))
    return runs, floor


def sort_pack(data, part_lists=(), strip_scope='normal', materials=None, match_diagonals=False,
              static_light=None, reduced_headers=None, surface_ground=False, cross_mesh_by_area=False,
              stipple=False, flatten=False, split=None, fans=None, bake=None, cores=None, lod=None, to_opaque=None,
              drape=None):
    """DNM6 bytes, report and face map for validated DNM2/4/5 bytes.

    The map holds new_to_old source face indices, the layer of each new face
    and the layering evidence of find_layers, for verification tools.
    """
    pack = read_pack(data)
    draped = drape_report = None
    if drape is not None:
        # First: every later step sees the road decals where they now lie (road_drape.py).
        import road_drape
        if reduced_headers is None:
            raise ValueError('--drape-road-decals needs --scenery-classes')
        classes, decals, _ = scenery_classes(pack, reduced_headers, surface_ground)
        surface = {m for m, word in enumerate(classes) if word & SCENERY_SURFACE and m not in decals}
        drape_report, draped, drape_lift = road_drape.drape(pack, decals, surface, object_meshes(pack), **drape)
    composites, flatten_report = [], None
    if flatten:
        if static_light is None:
            raise ValueError('--flatten-stacks needs --static-light: a composite is keyed by its baked lumas')
        # Lumas of the source faces (flatten_stacks indexes them by pack['origin']).
        composites, flatten_report = flatten_stacks(pack, static_lumas(pack if draped is None else read_pack(data), static_light),
                                                    None if flatten is True else int(flatten))
    decal_report = None
    if bake is not None:
        # Before the cuts: a baked overlay is no punch-through face any more.
        import bake_decals
        if static_light is None:
            raise ValueError('--bake-decals needs --static-light: a composite is rendered under its baked lumas')
        baked, decal_report = bake_decals.bake(pack, static_lumas(read_pack(data), static_light), bake['spec'],
                                               find_layers, len(composites), bake['only'], bake['skip'])
        composites = composites+baked
    opaque_report = opaque_log = None
    if to_opaque is not None:
        # After the bake, before every cut: hole-free punch-through faces leave the list (pt_to_opaque.py).
        import pt_to_opaque
        words = set(to_opaque.get('skip_words', ()))
        if lod is not None:
            words |= {int(w) for rule in lod['spec']['rules'] for w in rule.get('textures', ())}
            words |= {int(w) for w in (lod['spec'].get('cores') or {}).get('textures', {})}
        if cores is not None:
            words |= {int(w) for w in cores['spec'].get('textures', {})}
        opaque_report, opaque_log = pt_to_opaque.run(
            pack, to_opaque['images'], to_opaque['skip'], words, to_opaque.get('trim', True),
            to_opaque.get('strict_levels', False))
    split_report = plans = None
    if split is not None:
        import split_cutouts
        split_report, plans = split_cutouts.split_cutouts(pack, **split)
    cores_report = core_log = None
    if cores is not None:
        # Opaque cores, bars and merged runs of punch-through faces (opaque_cores.py).
        import opaque_cores
        if split is not None:
            raise ValueError('--opaque-cores and --split-cutouts both cut punch-through faces: use one')
        cores_report, core_log = opaque_cores.run(pack, light=static_light, **cores)
    lod_report = lod_log = None
    if lod is not None:
        # Level-of-detail groups: alternative shapes of the same punch-through faces (lod_groups.py).
        import lod_groups
        if split is not None or cores is not None or fans is not None:
            raise ValueError('--lod-groups cuts the punch-through faces itself: not with --split-cutouts, --opaque-cores or --rebuild-fans')
        if lod['spec'].get('cores'):
            # Cutouts with one shape for every distance, as --opaque-cores.
            import opaque_cores
            cores_report, core_log = opaque_cores.run(pack, lod['spec']['cores'], lod['images'], lod['skip'], static_light)
        lod_report, lod_log = lod_groups.build(pack, light=static_light,
                                               checker_texels_per_unit=STIPPLE_TEXELS_PER_UNIT if stipple else None, **lod)
    fan_report = None
    if fans is not None:
        # After the cuts: the layer evidence names the faces as they are now.
        import rebuild_fans
        fan_report = rebuild_fans.rebuild_fans(pack, find_layers(pack, part_lists, cross_mesh_by_area, draped)[2], **fans)
    origin = pack.get('origin', list(range(len(pack['faces']))))
    material_rank = material_ranks(pack, materials) if materials is not None else None
    layer, report, pairs = find_layers(pack, part_lists, cross_mesh_by_area, draped)
    if drape_report is not None:
        report.update(road_drape=drape_report)
    if flatten_report is not None:
        report.update(flattened_stacks=flatten_report)
    if decal_report is not None:
        report.update(baked_decals=decal_report)
    if opaque_report is not None:
        report.update(pt_to_opaque=opaque_report)
    if split_report is not None:
        report.update(split_cutouts=split_report)
    if fan_report is not None:
        report.update(rebuilt_fans=fan_report)
    if cores_report is not None:
        report.update(opaque_cores=cores_report)
    if lod_report is not None:
        report.update(lod_groups=lod_report)
    rotated = []
    if match_diagonals:
        rotated, conflicts = match_decal_diagonals(pack, layer, pairs)
        # Same corners, hence same layers; the dips are measured again.
        again, after, pairs = find_layers(pack, part_lists, cross_mesh_by_area, draped)
        if again != layer:
            raise AssertionError('rotating a quad changed its layer')
        report.update(decal_diagonals_matched=len(rotated), decal_diagonal_conflicts=conflicts,
                      same_corner_dips_before=report['same_corner_dips'],
                      same_corner_dips=after['same_corner_dips'])
    if split_report is not None:
        # One layer for all pieces of a face, and still above what they lie on.
        members = defaultdict(list)
        for f, source in enumerate(origin):
            members[source].append(f)
        changed = True
        while changed:
            changed = False
            for group in members.values():
                top = max(layer[f] for f in group)
                for f in group:
                    if layer[f] != top:
                        layer[f], changed = top, True
            for i, j, *_ in pairs:
                if layer[j] <= layer[i]:
                    layer[j], changed = layer[i]+1, True
            if max(layer) > 254:
                raise ValueError('coplanar draw order is cyclic or deeper than 255 layers')
    if pack.get('overlay'):
        # An overlay core lies above the punch-through face it was cut from,
        # and whatever lay on that face lies above the core.
        changed = True
        while changed:
            changed = False
            for piece, below in pack['overlay'].items():
                for under in below:
                    if layer[piece] <= layer[under]:
                        layer[piece], changed = layer[under]+1, True
            for i, j, *_ in pairs:
                if layer[j] <= layer[i]:
                    layer[j], changed = layer[i]+1, True
            if max(layer) > 254:
                raise ValueError('coplanar draw order is cyclic or deeper than 255 layers')
    lumas = static_lumas(pack, static_light) if static_light is not None else None
    classes = None
    if reduced_headers is not None:
        classes, decals, class_report = scenery_classes(pack, reduced_headers, surface_ground)
        if pack.get('decal_materials'):   # a composite takes the class of its base
            classes, decals, class_report = bake_decals.inherit_classes(pack, scenery_classes, reduced_headers, surface_ground)
        report.update(scenery_classes=class_report)
        lift = ROAD_DECAL_LAYER
        if draped is not None:
            # On or above the road now: one layer above every road-surface face is enough (road_drape.py).
            lift = 1+max((layer[f] for f, face in enumerate(pack['faces'])
                          if classes[face[4]] & SCENERY_SURFACE and face[4] not in decals), default=0)
            report.update(road_decal_layer=lift)
        origin = pack.get('origin', list(range(len(pack['faces']))))
        for f, face in enumerate(pack['faces']):
            if face[4] in decals:
                layer[f] += lift+(drape_lift.get(origin[f], 0) if draped is not None else 0)
        if max(layer) > 254:
            raise ValueError('road decal layers exceed the layer range')
    if stipple:
        # After the classes: the panels stay the last scenery stage. From here
        # on they are ordinary punch-through faces (order, strips, bindings).
        report.update(stipple_panels=stipple_panels(pack))
    order, strips, indices, mesh_strips = order_faces(pack, layer, strip_scope, material_rank)
    out = write_pack(pack, order, layer, strips, indices, mesh_strips, lumas, classes)
    if pack['aligned']:
        out = pm.align_tables(out)
    before, floor = material_runs(pack, list(range(len(order))))
    after, _ = material_runs(pack, order)
    report.update(
        faces=len(order), layers=max(layer)+1, material_runs_source_order=before,
        material_runs_sorted=after, material_run_floor=floor, strips=len(strips),
        strip_faces=sum(s[1] for s in strips), strip_vertices=len(indices),
        longest_strip_faces=max((s[1] for s in strips), default=0),
        bytes=len(out), sha256=hashlib.sha256(out).hexdigest())
    if lumas:
        report.update(static_light=[float(v) for v in static_light],
                      baked_front_lumas=len({v[0] for v in lumas}),
                      baked_two_sided_faces_with_two_lumas=sum(1 for f, v in enumerate(lumas)
                                                                if pack['faces'][f][7] & (1 << 17) and v[0] != v[1]))
    # Source face indices throughout: flattening removes faces before the sort.
    return out, report, dict(new_to_old=[origin[old] for old in order], layer=[layer[old] for old in order],
                             pairs=[[origin[i], origin[j], *rest] for i, j, *rest in pairs],
                             rotated=[origin[f] for f in rotated], composites=composites,
                             **(dict(split_plans=plans) if plans is not None else {}),
                             **(dict(core_pieces=core_log, merged={str(k): v for k, v in pack.get('merged', {}).items()},
                                     foreign_trunks=[order.index(f) for f in pack.get('foreign_trunks', [])])
                                if core_log is not None else {}),
                             **(dict(lod=pack.get('lod_written', []), lod_pieces=lod_log) if lod_report is not None else {}),
                             **(dict(pt_to_opaque=opaque_log) if opaque_log is not None else {}))


def reduced_headers(source):
    """The audited reduced-view material headers of track_surface.c, or of a text list (a course other than 0: one
    header "h0 h1 h2 h3" a line, # comments; it may be empty, with --surface-ground then classing the level ground)."""
    if 'int nm_track_surface_select' not in source:
        return [tuple(map(int, line.split()[:4])) for line in source.splitlines()
                if line.strip() and not line.lstrip().startswith('#')]
    rows = re.findall(r'\{(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\}', source.split('int nm_track_surface_select', 1)[0])
    if len(rows) != 40 or len(set(rows)) != 40:
        raise ValueError('expected the 40 audited headers before nm_track_surface_select')
    return [tuple(map(int, row)) for row in rows]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('source', type=Path, help='validated DNM2/4/5 pack in arcade face order')
    parser.add_argument('output', type=Path, help='NEW DNM6 pack')
    lists = parser.add_mutually_exclusive_group()
    lists.add_argument('--intro-header', type=Path,
                       help='generated intro_drive_data.h: car parts drawn with one matrix')
    lists.add_argument('--part-lists', type=Path,
                       help='JSON list of source-address lists drawn with one matrix, in draw order')
    parser.add_argument('--materials', type=Path,
                        help='DNT2/4/6 material pack of this mesh pack: keep materials of one texture adjacent')
    parser.add_argument('--order-map', type=Path, help='also write new->old face indices and layers (JSON)')
    parser.add_argument('--match-decal-diagonals', action='store_true',
                        help='split a decal quad on the same diagonal as the base whose four corners it shares '
                             '(changes the two triangles of those decals; see the module text)')
    parser.add_argument('--strip-scope', choices=('normal', 'material'), default='normal',
                        help='chain faces of one normal (default) or of one whole material/layer run')
    parser.add_argument('--static-light', type=float, nargs=3, metavar=('X', 'Y', 'Z'),
                        help='bake the constant face lumas of static scenery under this world light')
    parser.add_argument('--scenery-classes', type=Path, metavar='TRACK_SURFACE_C',
                        help='write per-material scenery stage and depth class; the reduced view is the '
                             'header list of this source file (code/render/scene/track_surface.c)')
    parser.add_argument('--cross-mesh-decals', action='store_true',
                        help='static scenery: layer coplanar overlapping faces of different meshes, smaller face on top')
    parser.add_argument('--flatten-stacks', action='store_true',
                        help='an opaque face with punch-through faces on exactly the same polygon becomes one '
                             'opaque face of a composite material; the others are removed (see --composites)')
    parser.add_argument('--flatten-limit', type=int, metavar='N',
                        help='flatten only the N composites that remove the most face area: each needs a palette bank')
    parser.add_argument('--composites', type=Path,
                        help='NEW JSON: the composite materials of --flatten-stacks, for prepare_palette_materials.py')
    parser.add_argument('--stipple-panels', action='store_true',
                        help='screen-checker panels become punch-through faces with a stipple texture '
                             '(prepare_palette_materials.py supplies it); nothing is left for the translucent list')
    parser.add_argument('--drape-road-decals', action='store_true',
                        help='raise the corners of road-decal faces that dip under the road triangles (road_drape.py) and '
                             'lift road decals one layer above the road surface instead of ROAD_DECAL_LAYER '
                             '(needs --scenery-classes)')
    parser.add_argument('--drape-corners', choices=('keep', 'snap'), default='keep',
                        help='--drape-road-decals, measurement: leave the decal corners where the arcade has them (keep, '
                             'default) or first move every corner onto the road under it (snap)')
    parser.add_argument('--drape-cut', type=float, default=math.inf, metavar='UNITS',
                        help='--drape-road-decals, measurement: cut a face that dips deeper than this along the road '
                             'creases under it (default: never)')
    parser.add_argument('--drape-uncut', choices=('raise', 'lift'), default='raise',
                        help='--drape-road-decals: an uncut face that dips under the road has its corners raised (raise, '
                             'default) or, measurement, gets extra layers for a camera --drape-lift-height above the road')
    parser.add_argument('--drape-lift-height', type=float, default=1.0, metavar='UNITS',
                        help='--drape-uncut lift: camera height above the road the extra layers are made for (default 1)')
    parser.add_argument('--live-specular', action='store_true',
                        help='with --static-light: bake faces with an effective specular term without it; the renderer '
                             'adds it every frame (NM_PACK_LIVE_SPECULAR)')
    parser.add_argument('--surface-ground', action='store_true',
                        help='stage-2 ground materials also take the road-surface depth class')
    parser.add_argument('--split-cutouts', choices=('auto', 'hull', 'lean', 'outline', 'rich', 'grid', 'tight', 'none'),
                        help='cut punch-through faces into opaque, punch-through and empty parts (module text): '
                             'convex pieces along the silhouette (hull: one tight punch-through polygon; lean, '
                             'outline, rich: plus opaque cores, in rising polygon count), a texel grid, auto: '
                             'per face the outline plan that pays best by --split-weights, or tight: a few '
                             'punch-through pieces hugging the opaque texels, nothing on the opaque list. '
                             'none: only the --cutout-* measurement aids')
    parser.add_argument('--split-tight-triangles', type=int, default=8,
                        help='tight: most triangles one source face may become (default 8)')
    parser.add_argument('--split-tight-price', type=float, default=0.02,
                        help='tight: share of a face an added triangle must stop drawing (default 0.02)')
    parser.add_argument('--cutout-drop', type=int, nargs='+', metavar='WORD', default=[],
                        help='measurement aid: remove the punch-through faces with these texture words')
    parser.add_argument('--cutout-opaque', type=int, nargs='+', metavar='WORD', default=[],
                        help='measurement aid: draw the punch-through faces with these texture words whole on '
                             'the opaque list, alpha ignored (a wrong picture)')
    parser.add_argument('--cutout-drop-faces', type=Path, metavar='JSON',
                        help='measurement aid: remove these source faces (JSON list of source face indices)')
    parser.add_argument('--split-weights', type=Path, nargs='+', metavar=('ORDER_JSON', 'FILL'),
                        help='auto: order map (--order-map) of the pack the fill was recorded with, then one or '
                             'more simulator fill files (pack face, samples, winning samples, frames seen)')
    parser.add_argument('--split-pixel-price', type=float, default=100.0,
                        help='auto: punch-through pixel samples an added face must save per frame it is drawn (default 100)')
    parser.add_argument('--split-video', type=Path, metavar='VIDEO_DUMP',
                        help='reference video dump with the textures (as for prepare_palette_materials.py)')
    parser.add_argument('--split-cell', type=int, default=16, help='grid mode: cell size in texels (default 16)')
    parser.add_argument('--split-least-gain', type=float, default=0.25,
                        help='split a face only if this share of its punch-through area goes (default 0.25)')
    parser.add_argument('--split-face-limit', type=int, default=24,
                        help='most faces one source face may become (default 24)')
    parser.add_argument('--split-budget', type=int, metavar='TRIANGLES',
                        help='most triangles added in the whole pack; best area per triangle first')
    parser.add_argument('--split-reduced', action='store_true',
                        help='also split the punch-through materials of the reduced view (stage 0); by default '
                             'only later stages change, so builds up to scenery stage 3 draw what they drew')
    parser.add_argument('--split-textures', type=int, nargs='+', metavar='WORD',
                        help='split only materials with these texture words (header word 2)')
    parser.add_argument('--bake-decals', type=Path, metavar='SPEC_JSON',
                        help='bake punch-through decals into the texture of the face they lie on (bake_decals.py): '
                             'the composites to build; they are added to --composites')
    parser.add_argument('--decal-only', nargs='+', metavar='NAME', help='bake only these composites of the specification')
    parser.add_argument('--decal-skip', nargs='+', metavar='NAME', default=[], help='leave these composites out')
    parser.add_argument('--opaque-cores', type=Path, metavar='SPEC_JSON',
                        help='opaque cores, bars and merged runs for punch-through faces: the specification '
                             'opaque_cores.py designed (needs --split-video, --static-light and --scenery-classes; '
                             'give the same file to prepare_palette_materials.py --texture-fill)')
    parser.add_argument('--lod-groups', type=Path, metavar='LOD_JSON',
                        help='level of detail for punch-through faces: per rule the alternative shapes (opaque-cores '
                             'specifications) and switch depths, see lod_groups.py (needs --split-video, --static-light '
                             'and --scenery-classes; give the same file to prepare_palette_materials.py --texture-fill)')
    parser.add_argument('--pt-to-opaque', action='store_true',
                        help='punch-through faces whose filtered texels hold no hole move to the opaque list, those '
                             'with holes only beyond one rectangle of opaque texels cut to it (pt_to_opaque.py; needs '
                             '--split-video and --scenery-classes; give the result to prepare_palette_materials.py --split-mesh)')
    parser.add_argument('--pt-to-opaque-trim', choices=('yes', 'no'), default='yes',
                        help='--pt-to-opaque: also cut faces to their rectangle of opaque texels (default yes)')
    parser.add_argument('--pt-to-opaque-strict-levels', action='store_true',
                        help='--pt-to-opaque: leave a face punch-through when a mip level of its texture has a hole within its reach')
    parser.add_argument('--rebuild-fans', action='store_true',
                        help='retriangulate flat one-material triangle fans without their hub (rebuild_fans.py)')
    parser.add_argument('--fan-min-triangles', type=int, default=3, help='least hub triangles of a fan (default 3)')
    parser.add_argument('--fan-hub-angle', type=float, default=30.0,
                        help='widest corner, in degrees, a hub triangle may have at the hub (default 30)')
    parser.add_argument('--fan-plane', type=float, default=1e-3,
                        help='a fan corner may lie this far off the fan plane, model units (default 1e-3)')
    parser.add_argument('--fan-uv', type=float, default=1.0,
                        help='largest deviation from one affine texture mapping, in 1/8 texels (default 1)')
    parser.add_argument('--fan-prefer', choices=('angle', 'span'), default='angle',
                        help='rebuild for the largest minimum angle (default) or the smallest sum of longest edges')
    parser.add_argument('--fan-steiner', type=float, default=0.0, metavar='PITCH',
                        help='add interior points on a grid of PITCH model units to rebuilt fans (default 0: none)')
    parser.add_argument('--fan-spokes', type=int, default=0, metavar='N',
                        help='cut every hub edge shared only by rebuilt fans at N points (default 0: none)')
    parser.add_argument('--fan-spoke-site', type=int, default=8, metavar='N',
                        help='--fan-spokes applies around hubs with at least N coplanar hub triangles of any material')
    args = parser.parse_args()
    if args.live_specular and not args.static_light:
        parser.error('--live-specular needs --static-light')
    ACCEPT_SPECULAR = args.live_specular
    part_lists = ()
    if args.intro_header:
        part_lists = intro_part_lists(args.intro_header.read_text())
    elif args.part_lists:
        part_lists = json.loads(args.part_lists.read_text())
    split = None
    if (args.cutout_drop or args.cutout_opaque or args.cutout_drop_faces) and not args.split_cutouts:
        parser.error('the --cutout-* measurement aids need --split-cutouts (none: nothing else changes)')
    if args.split_cutouts:
        if not args.split_video or not args.static_light or not args.scenery_classes:
            parser.error('--split-cutouts needs --split-video, --static-light and --scenery-classes')
        from prepare_palette_materials import texture_indices
        from prepare_palette_probe import video
        sheets = video(args.split_video)[2]
        split = dict(images=lambda header: texture_indices(sheets, header[2], 32 << (header[0] & 7),
                                                           32 << ((header[0] >> 3) & 7)),
                     mode=args.split_cutouts, cell=args.split_cell, least_gain=args.split_least_gain,
                     face_limit=args.split_face_limit, budget=args.split_budget,
                     only=set(args.split_textures) if args.split_textures else None,
                     tight_triangles=args.split_tight_triangles, tight_price=args.split_tight_price,
                     drop=args.cutout_drop, opaque=args.cutout_opaque,
                     drop_faces=json.loads(args.cutout_drop_faces.read_text()) if args.cutout_drop_faces else (),
                     skip=() if args.split_reduced else set(reduced_headers(args.scenery_classes.read_text())))
        if args.split_cutouts == 'auto':
            if not args.split_weights or len(args.split_weights) < 2:
                parser.error('--split-cutouts auto needs --split-weights ORDER.json FILL...')
            recorded = json.loads(args.split_weights[0].read_text())['new_to_old']
            weights = defaultdict(lambda: [0, 0])
            for path in args.split_weights[1:]:
                for line in path.read_text().splitlines()[1:]:
                    face, samples, _, frames = map(int, line.split())
                    weights[recorded[face]][0] += samples
                    weights[recorded[face]][1] += frames
            split.update(weights=dict(weights), pixel_price=args.split_pixel_price)
    cores = None
    if args.opaque_cores:
        if not args.split_video or not args.static_light or not args.scenery_classes:
            parser.error('--opaque-cores needs --split-video, --static-light and --scenery-classes')
        from prepare_palette_materials import texture_indices
        from prepare_palette_probe import video
        core_sheets = video(args.split_video)[2]
        cores = dict(spec=json.loads(args.opaque_cores.read_text()),
                     images=lambda header: texture_indices(core_sheets, header[2], 32 << (header[0] & 7),
                                                           32 << ((header[0] >> 3) & 7)),
                     skip=() if args.split_reduced else set(reduced_headers(args.scenery_classes.read_text())))
    lod = None
    if args.lod_groups:
        if not args.split_video or not args.static_light or not args.scenery_classes:
            parser.error('--lod-groups needs --split-video, --static-light and --scenery-classes')
        import lod_groups
        from prepare_palette_materials import texture_indices
        from prepare_palette_probe import video
        lod_sheets = video(args.split_video)[2]
        lod = dict(spec=lod_groups.load(args.lod_groups),
                   images=lambda header: texture_indices(lod_sheets, header[2], 32 << (header[0] & 7),
                                                         32 << ((header[0] >> 3) & 7)),
                   skip=() if args.split_reduced else set(reduced_headers(args.scenery_classes.read_text())))
    to_opaque = None
    if args.pt_to_opaque:
        if not args.split_video or not args.scenery_classes:
            parser.error('--pt-to-opaque needs --split-video and --scenery-classes')
        from prepare_palette_materials import texture_indices
        from prepare_palette_probe import video
        opaque_sheets = video(args.split_video)[2]
        to_opaque = dict(images=lambda header: texture_indices(opaque_sheets, header[2], 32 << (header[0] & 7),
                                                               32 << ((header[0] >> 3) & 7)),
                         skip=set(reduced_headers(args.scenery_classes.read_text())),
                         trim=args.pt_to_opaque_trim == 'yes', strict_levels=args.pt_to_opaque_strict_levels)
    data, report, face_map = sort_pack(args.source.read_bytes(), part_lists, args.strip_scope,
                                       args.materials.read_bytes() if args.materials else None,
                                       args.match_decal_diagonals, args.static_light,
                                       reduced_headers(args.scenery_classes.read_text()) if args.scenery_classes else None,
                                       args.surface_ground, args.cross_mesh_decals, args.stipple_panels,
                                       (True if args.flatten_limit is None else args.flatten_limit)
                                       if args.flatten_stacks else False, split,
                                       fans=dict(min_triangles=args.fan_min_triangles, hub_angle=args.fan_hub_angle,
                                                 plane=args.fan_plane, uv_tolerance=args.fan_uv, prefer=args.fan_prefer,
                                                 steiner=args.fan_steiner, spokes=args.fan_spokes,
                                                 spoke_site=args.fan_spoke_site)
                                       if args.rebuild_fans else None,
                                       bake=dict(spec=json.loads(args.bake_decals.read_text()), only=args.decal_only,
                                                 skip=args.decal_skip) if args.bake_decals else None,
                                       cores=cores, lod=lod, to_opaque=to_opaque,
                                       drape=dict(cut=args.drape_cut, uncut=args.drape_uncut, corner_mode=args.drape_corners,
                                                  lift_height=args.drape_lift_height)
                                       if args.drape_road_decals else None)
    if args.composites:
        with args.composites.open('x') as out:
            json.dump(face_map['composites'], out)
    with args.output.open('xb') as out:
        out.write(data)
    if args.order_map:
        with args.order_map.open('x') as out:
            json.dump(face_map, out)
    print(json.dumps(report, indent=2))
