"""DNT6: palette-shaded material pack (one PAL4 texture per arcade texture).

python3 platform/dreamcast-native/tools/prepare_palette_materials.py SOURCE.dnt VIDEO_DUMP NEW.dnt

SOURCE.dnt is a validated DNT4 pack; only its binding keys (texture header,
luma) and its solid bindings are used. VIDEO_DUMP holds the reference video
tables (palram/colorxlat/lumaram/tex0/tex1 .bin). Each arcade texture is stored
once as a twiddled 4-bit index image. A binding carries the index of one of a
few colour-independent gain-bucket palettes, keyed by (bucket R, bucket G,
bucket B, lumabase), and a vertex colour; the PVR multiplies the two. Bucket 0
is the linear ramp, buckets 1..N cover the saturating range of a channel.

Layout (little endian, 32-byte header as DNT1..4):
  "DNT6", file size, resources, bindings, VRAM payload bytes, largest blob,
  palettes, flags (bit 0: separate palettes for the opaque and punch-through
  lists; bits 8..15: saturating buckets per channel; bit 16: mip chains)
  resources x (offset, size, width u16, height u16, 0)       as DNT4
  bindings  x (header u16[4], luma u16, resource u16, crop)  as DNT4, sorted
  bindings  x vertex ARGB u32                                as DNT4
  bindings  x palette index u8 (255 for solid), zero padded to 4 bytes
  palettes  x 16 ARGB8888
  palettes  x (bucket R, bucket G, bucket B, lumabase | list<<7) u8
  zero padding to 32 bytes, then one DcTx PAL4 blob per resource.
Entry 15 keeps its ramp colour; its alpha is 0 (list bit 1) unless the palette
is one of the opaque-only ones of a split pack (list bit 0), which exist only
for opaque bindings whose texture contains entry 15.
Translucent class: a textured header with texel 15 transparent (word 0 bits
13-14 = 3) and the screen-checker bit 15 (the cars' smoke, e409) is the
arcade's screen-door half coverage. Its binding is made as a punch-through
one (palette with entry 15 alpha 0) with vertex alpha TRANSLUCENT_ALPHA
(0x80) instead of 0xff: the renderer draws it on the translucent list with
the texture alpha modulated by the vertex alpha (model_renderer.c), so
entry 15 stays a hole and every other texel covers half. Report
"translucent_bindings".
Output is ROM-derived: write it under ignored build/ only.

Static scenery (world packs):
  --mesh WORLD.dnm     a DNM6 pack with baked light (prepare_face_order.py
                       --static-light): only the (texture header, luma) pairs
                       its faces can show are kept, weighted by their faces.
  --share CAR.dnt      the scene's car pack. Palette RAM is shared: a world
                       palette equal to a car palette uses the car's bank.
  --palette-banks N    banks of the scene for cars and world together
                       (NM_CAR_PALETTE_BANKS). The world may add N minus the
                       car palettes; every other binding takes the best
                       palette of the allowed set, at the error cost reported
                       under "palette_budget".
  --mipmaps-for opaque|all   mip chains for square textures: opaque users
                       only, or punch-through textures too (--mipmaps = all).
  --mip-filter index|colour|arcade   how the levels are made (default index:
                       2x2 means of ramp positions, see reduce_indices).
                       colour: box means of the colours the texture shows
                       (every palette and vertex colour it is drawn with,
                       face weighted) in linear light (--mip-gamma), mapped to
                       the nearest entry without dithering; a punch-through
                       level texel is a hole when less than --mip-coverage of
                       its area is opaque, else the mean of its opaque texels
                       (the hole's RGB never enters). Textures of split
                       cutouts (both lists) keep no chain, so --mipmaps-for
                       all is allowed with --split-mesh. arcade: the levels
                       the Model 2 keeps in its texture sheets (colour for
                       textures this tool changed). Only square textures can
                       carry a PVR mip chain.
  --mip-skip-surface   no chain for textures of road-surface class materials
                       of --mesh (seen at grazing angles; the PVR picks one
                       level from the larger screen axis, which blurs them).
  --mip-skip-composites   no chain for the flattened stacks (grandstands):
                       seen along the track, the isotropic level smears them.
  --mip-skip-textures W1,W2,...   no chain for the textures with these
                       texture words (header word 2): ground lying flat
                       outside the road-surface class, e.g. the forest floor
                       (6428), which the isotropic level blurs as it does the
                       road. Report "mip_skipped_textures".
  --hole-colour       entry 15 of a punch-through palette: the mean colour of
                       the opaque texels next to holes, where no opaque
                       polygon shows entry 15 (report "hole_colour"; the opaque
                       piece of a split cutout does not count: it shows no
                       hole texel). --hole-colour-scene OTHER.dnt: palettes
                       an opaque binding of OTHER.dnt shows entry 15 with are
                       kept too (a car pack whose banks the world shares).
                       With --share, a palette equal to a shared one apart
                       from entry 15 takes the shared pack's words.
  --vram-limit BYTES   resident texture budget of the pack's loader.
  --palette-bits 8|5   5: every ramp entry is a value a 5-bit channel shows
                       on the PVR (bit-repeat expansion, q<<3|q>>2), so an
                       ARGB1555 palette RAM (make NATIVE_PAL16=1555,
                       include/render/materials/palette_ram.h) loads the
                       pack without loss, and the vertex colours are fitted
                       to those entries instead of the 8-bit ones. The file
                       format is unchanged (ARGB8888 entries). Composite
                       palettes are snapped too; baked decal palettes are not
                       (the loader converts them like any other entry). The
                       shared car pack (--share) must be built with the same
                       value. The report's model_error_as_1555 is the error of
                       the pack's own palettes read through ARGB1555 with its
                       own vertex colours (the plain NATIVE_PAL16 conversion).
  --split-mesh SPLIT.dnm   the same scenery with split cutouts
                       (prepare_face_order.py --split-cutouts); --mesh is the
                       pack without the split. Everything is decided from
                       --mesh, so textures, palettes and existing bindings
                       are those of the unsplit pack; each opaque split
                       header then gets a binding per baked luma with the
                       texture, palette and vertex colour of the
                       punch-through header it came from. The result serves
                       both meshes. Cutout textures must stay without mip
                       chains (not with --mipmaps-for all): the opaque
                       pieces are cut on the full-size texels.
  --extra-keys FILE    (texture header, luma) pairs the renderer switches faces of
                       --mesh to while the scene runs ("h0 h1 h2 h3 luma" a
                       line: the frames of the frame-counter animations,
                       prepare_world_objects.py clock_keys.txt): bindings are
                       made for them as for the mesh's own (face weight 1).
  --chase-variants N   texture headers whose luma table (lumabase, header word
                       1) is not monotonic: the colour-cycling ramps of
                       Advanced's animations (lumabases 2..65, a brightness
                       wave that travels along the texture's indices as the
                       frame counter steps the lumabase). A 4-bit palette
                       cannot follow a wave through the gain buckets, and the
                       scene has no palette bank to spare per lumabase, so
                       the wave is baked into the texels: N copies of each
                       such texture (the lumabases in N runs of equal length,
                       a copy for each run's mean ramp) whose texels index
                       one palette of 15 brightness levels (entry 15 as
                       before: transparent in punch-through textures), and
                       per lumabase a binding with its copy and its own
                       vertex colour, fitted to the lumabase's own colours.
                       No mip chain on the copies. Report "chase".
  --texture-fill SPEC.json   texel changes of opaque_cores.py, or the
                       level-of-detail specification of lod_groups.py
                       (prepare_face_order.py --lod-groups). With the latter
                       a texture may be needed twice: the shape of one level
                       fills texels the shape of another must leave
                       transparent. A header of --split-mesh that carries a
                       variant number (header word 1 bits 8..9) gets a copy of
                       the texture with that variant's texels: a further 4-bit
                       resource of the same size, drawn with the palette,
                       bank and vertex colour of the header without the
                       number. No palette and no bank is added.
                       The same for a stipple header with a variant number (a
                       {"stipple": CELL} level of the checker panels): a
                       further synthetic stipple texture of the same size, the
                       cell repeated in the stipple's palette entry, with the
                       palette, bank and vertex colour of the plain stipple.
"""
import argparse
import collections
import math
import hashlib
import json
from pathlib import Path
import struct

import numpy as np

from prepare_material_crops import block_index
from prepare_palette_probe import video, ramp

HEADER = struct.Struct('<4s7I')
RESOURCE = struct.Struct('<IIHHI')
BINDING = struct.Struct('<6HI')
SOLID = 65535
TRANSLUCENT_ALPHA = 0x80   # vertex alpha of the translucent class (module text; material_pack.c accepts it there only)


def translucent(header):
    """The translucent class: textured, texel 15 transparent, screen checker."""
    return bool(header[0] & 0x4000) and bool(header[0] & 0x8000) and (header[0] >> 13) & 3 == 3
# Loader limits (include/material_pack.h, code/material_pack.c).
PALETTES_MAX = 64
RESOURCES_MAX = 4096
BINDINGS_MAX = 65534
FILE_MAX = 10*1024*1024
VRAM_MAX = 2*544*1024  # NM_MATERIAL_CACHE_BANKS * NM_MATERIAL_CACHE_BYTES


def read_pack(path):
    data = path.read_bytes()
    magic, size, resources, count, *_ = HEADER.unpack_from(data)
    if magic != b'DNT4' or size != len(data):
        raise ValueError('expected a DNT4 material pack')
    base = 32+16*resources
    rows = []
    for i in range(count):
        row = BINDING.unpack_from(data, base+16*i)
        rows.append((row[:4], row[4], row[5], row[6], struct.unpack_from('<I', data, base+16*count+4*i)[0]))
    return rows, resources


def expand5(q):
    return (q << 3) | (q >> 2)


# 8-bit channel value -> the nearest value a 5-bit palette channel shows on the
# PVR (palette_ram.h, nm_palette_narrow; lower on a tie).
SNAP5 = np.array([min((expand5(q) for q in range(32)), key=lambda e: (abs(e-v), e)) for v in range(256)])


def palette_level(values, bits):
    """Channel values as an entry of `bits` bits a channel reads on the PVR."""
    if bits == 8:
        return values
    return SNAP5[np.clip(np.rint(np.asarray(values)), 0, 255).astype(np.int64)]


def gain_ramps(luma, curve, header, buckets, bits=8):
    """Bucket 0: the largest gain with at most one saturated entry; then
    `buckets` gains spread evenly up to full colour at full light. bits=5:
    each entry snapped to a value a 5-bit palette channel shows."""
    return gain_ramps_base(ramp(luma, header), curve, buckets, bits)


def gain_ramps_base(base, curve, buckets, bits=8):
    """gain_ramps for the 16 luma levels `base` (0..63) of a palette."""
    def channel(gain):
        return palette_level(curve[31][np.minimum(63, base*gain//(31*256))], bits)
    linear = 0
    for gain in range(1, 31*255+1, 8):
        if (channel(gain) >= 255).sum() <= 1:
            linear = gain
    gains = [linear]+[int(linear+(31*255-linear)*(i+1)/buckets) for i in range(buckets)]
    return [channel(g).astype(float) for g in gains], gains


def fit_buckets(target, ramps):
    """[(max error, bucket, vertex value)] of one channel, per bucket."""
    fits = []
    for bucket, g in enumerate(ramps):
        v = min(255, max(0, int(round(float((g*target).sum()/max((g*g).sum(), 1e-9))*255))))
        fits.append((float(np.abs(np.round(g*v/255)-target).max()), bucket, v))
    return fits


def fit_channel(target, ramps):
    """(max error, bucket, vertex value) of the best bucket for one channel."""
    best = None
    for fit in fit_buckets(target, ramps):
        if best is None or fit[0] < best[0]:
            best = fit
    return best


def mesh_keys(path):
    """{(texture header, luma): faces} a baked DNM6 scenery pack can show."""
    import prepare_models as pm
    data = path.read_bytes()
    h = pm.HEADER.unpack_from(data)
    if (h[0] & ~pm.ALIGNED) != 0x364d4e44 or h[1] != len(data) or not h[15] & 1:
        raise ValueError('--mesh needs a DNM6 pack with baked light')
    bases = pm.table_offsets(h)
    headers = [struct.unpack_from('<4H', data, bases[3]+m*pm.MATERIAL.size) for m in range(h[5])]
    keys = collections.Counter()
    for f in range(h[4]):
        face = struct.unpack_from('<5HBBI3fHBB', data, bases[2]+f*32)
        keys[headers[face[4]], face[6]] += 1
        if face[7] & (1 << 17):
            keys[headers[face[4]], face[13]] += 1
    if h[15] & 16:
        # Live specular (meshpack.h NM_PACK_LIVE_SPECULAR): every luma the renderer's term can reach (model_submission.h
        # sq_live_prepare: from the baked level to it plus scale |R|^k, R = 2 (n.l) n - l), one more each way.
        light = (-0.45, -0.89, -0.45)
        for m in range(h[2]):
            mesh = pm.MESH.unpack_from(data, bases[0]+m*pm.MESH.size)
            if not mesh[10] & 1:
                continue
            for f in range(mesh[2], mesh[2]+mesh[3]):
                face = struct.unpack_from('<5HBBI3fHBB', data, bases[2]+f*32)
                row = pm.MATERIAL.unpack_from(data, bases[3]+face[4]*pm.MATERIAL.size)
                ambient, scale, control = row[5], row[6], row[7]
                if not (control and scale and ambient < 255):
                    continue
                n = face[8:11]
                dot = sum(a*b for a, b in zip(n, light))
                term = math.sqrt(sum((2*dot*n[a]-light[a])**2 for a in range(3)))*1.0001
                for bit in (control >> 1, control >> 2, (control+1) >> 3):
                    if bit: term *= term
                reach = int(min(256.0, term*scale))+2
                for base in ((face[6], face[13]) if face[7] & (1 << 17) else (face[6],)):
                    for luma in range(max(0, base-1), min(255, base+reach)+1):
                        keys[headers[face[4]], luma] += 0 if (headers[face[4]], luma) in keys else 1
    return keys


def surface_words(path):
    """Texture words (header word 2) of the materials a DNM6 pack with scenery
    classes marks road-surface class (meshpack.h, NM_SCENERY_SURFACE)."""
    import prepare_models as pm
    data = path.read_bytes()
    h = pm.HEADER.unpack_from(data)
    bases = pm.table_offsets(h)
    words = set()
    for m in range(h[5]):
        row = pm.MATERIAL.unpack_from(data, bases[3]+m*pm.MATERIAL.size)
        if row[8] & 0x100:
            words.add(row[2])
    return words


def opaque_entry15_palettes(path):
    """Palettes (16 words each) of a DNT6 pack that an opaque binding shows
    entry 15 with: its texture contains index 15 and the binding is not the
    opaque piece of a split cutout (lod_groups.base_header). Read from the
    full-size level (the payload's last width x height / 2 bytes)."""
    import lod_groups
    data = path.read_bytes()
    magic, size, resources, count, _, _, palettes, flags = HEADER.unpack_from(data)
    if magic != b'DNT6' or size != len(data):
        raise ValueError('--hole-colour-scene needs a DNT6 pack')
    has15 = []
    for r in range(resources):
        offset, length, width, height, _ = RESOURCE.unpack_from(data, 32+16*r)
        top = np.frombuffer(data, np.uint8, width*height//2, offset+length-width*height//2) if \
            struct.unpack_from('<I', data, offset+16)[0] >> 31 else np.frombuffer(data, np.uint8, width*height//2, offset+32)
        has15.append(bool(((top & 15) == 15).any() or ((top >> 4) == 15).any()))
    base = 32+16*resources
    table = base+20*count+((count+3) & ~3)
    words = [struct.unpack_from('<16I', data, table+64*i) for i in range(palettes)]
    shown = set()
    for i in range(count):
        row = BINDING.unpack_from(data, base+16*i)
        if row[5] == SOLID or (row[0] >> 13) & 3 == 3 or not has15[row[5]] or \
                lod_groups.base_header(tuple(row[:4])) is not None:
            continue
        shown.add(words[data[base+20*count+i]])
    return shown


def shared_keys(path, buckets):
    """Palette keys and tables of a DNT6 pack whose banks this pack may use."""
    data = path.read_bytes()
    magic, size, resources, count, _, _, palettes, flags = HEADER.unpack_from(data)
    if magic != b'DNT6' or size != len(data) or flags & 1 or (flags >> 8) & 255 != buckets:
        raise ValueError('--share needs a shared-list DNT6 pack with the same saturating buckets')
    table = 32+16*resources+20*count+((count+3) & ~3)
    keys = [tuple(data[table+64*palettes+4*i:table+64*palettes+4*i+4]) for i in range(palettes)]
    return {(k[0], k[1], k[2], k[3] & 127): struct.unpack_from('<16I', data, table+64*i) for i, k in enumerate(keys)}


def texture_indices(sheets, h2, width, height):
    """Index image of one texture: reference get_texel addressing."""
    sheet = sheets[1 if h2 & 0x1000 else 0]
    u, v = np.meshgrid(np.arange(width), np.arange(height))
    x, y = (h2 & 63)*32+u, ((h2 >> 6) & 31)*32+v
    fold = np.where(x >= 1024, 512, 0)
    x = np.where(x >= 1024, x-1024, x)
    off = (((y >> 1) ^ fold) << 9)+(x >> 1)
    bit = ((off & 1) << 4) | ((1 ^ (v & 1)) << 3) | ((1 ^ (u & 1)) << 2)
    return ((sheet[(off >> 1) & 0x7ffff].astype(np.int64) >> bit) & 15).astype(np.uint8)


def twiddle_order(width, height):
    order = np.zeros((height, width), np.int64)
    for y in range(height):
        for x in range(width):
            order[y, x] = block_index(x, y, width, height)
    return order


def pal4_level(index):
    height, width = index.shape
    out = np.zeros(max(width*height, 2), np.uint8)
    out[twiddle_order(width, height).reshape(-1)] = index.reshape(-1)
    return (out[0::2] | out[1::2] << 4).astype(np.uint8).tobytes()


def reduce_indices(index, inverted, cutout):
    """Next mip level. An index is a position on a monotonic luma ramp, so the
    2x2 mean of ramp positions is the mean brightness. Punch-through keeps
    entry 15 (transparent) when at least half of the block is transparent and
    never produces it from opaque texels."""
    position = (15-index.astype(np.int64)) if inverted else index.astype(np.int64)
    blocks = np.stack([position[0::2, 0::2], position[0::2, 1::2], position[1::2, 0::2], position[1::2, 1::2]])
    raw = np.stack([index[0::2, 0::2], index[0::2, 1::2], index[1::2, 0::2], index[1::2, 1::2]])
    if cutout:
        hole = raw == 15
        weight = (~hole).sum(0)
        mean = np.where(weight > 0, (blocks*~hole).sum(0)/np.maximum(weight, 1), 0)
        value = np.rint(mean).astype(np.int64)
        value = np.where(inverted, 15-value, value)
        value = np.where(value == 15, 14, value)  # ramp neighbour of the hole
        return np.where(hole.sum(0) >= 2, 15, value).astype(np.uint8)
    value = np.rint(blocks.mean(0)).astype(np.int64)
    return (15-value if inverted else value).astype(np.uint8)


def colour_levels(index, users, cutout, gamma=2.2, coverage=0.5):
    """Mip levels (largest first, down to 1x1) of a square 4-bit texture made
    from its colours (--mip-filter colour): each level texel is the mean, in
    linear light (`gamma`), of the full-size texels it covers, under every
    palette and vertex colour the texture is drawn with (`users`: (16 x 3
    displayed entry colours, weight)); its index is the entry whose colours
    are nearest to those means, summed over the users with their weights (in
    the gamma-encoded values the screen shows; no error diffusion). A
    punch-through texture: a level texel is the transparent entry 15 when less
    than `coverage` of the texels it covers are opaque, otherwise the mean of
    its opaque texels only (the hole colour never enters) mapped to entries
    0..14. Entry 15 is a colour like the others for an opaque texture."""
    side = index.shape[0]
    hole = (index == 15) if cutout else np.zeros(index.shape, bool)
    solid = (~hole).astype(float)
    weights = np.array([w for _, w in users], float)
    weights = weights/weights.sum()
    shown = [np.asarray(entries, float) for entries, _ in users]
    linear = [((s/255.0)**gamma)[index]*solid[..., None] for s in shown]
    candidates = np.arange(15 if cutout else 16)
    levels = [index]
    size = side//2
    while size >= 1:
        step = side//size
        cover = solid.reshape(size, step, size, step).mean((1, 3))
        cost = np.zeros((size, size, len(candidates)))
        for s, lin, w in zip(shown, linear, weights):
            mean = lin.reshape(size, step, size, step, 3).sum((1, 3))/np.maximum(cover*step*step, 1e-9)[..., None]
            target = 255.0*np.clip(mean, 0, 1)**(1/gamma)
            cost += w*((target[:, :, None, :]-s[candidates][None, None, :, :])**2).sum(-1)
        level = candidates[np.argmin(cost, -1)].astype(np.uint8)
        if cutout:
            level = np.where(cover < coverage, 15, level).astype(np.uint8)
        levels.append(level)
        size //= 2
    return levels


def arcade_levels(sheets, h2, side):
    """The arcade's own mip levels of a square texture (--mip-filter arcade):
    the Model 2 keeps them in the texture sheets, level k at
    ((x-2048)>>k)&2047, ((y-1024)>>k)&1023 of the other sheet on odd k (the
    reference rasteriser, src/runtime/raster.cpp); index images on the same
    luma ramp as the full-size texture."""
    levels, k = [], 0
    while side >> k >= 1:
        sheet = sheets[(1 if h2 & 0x1000 else 0) ^ (k & 1)]
        bx, by = ((32*(h2 & 63)-2048) >> k) & 2047, ((32*((h2 >> 6) & 31)-1024) >> k) & 1023
        u, v = np.meshgrid(np.arange(side >> k), np.arange(side >> k))
        x, y = bx+u, by+v
        fold = x >= 1024
        x, y = np.where(fold, x-1024, x), np.where(fold, y ^ 1024, y)
        off = (y >> 1)*512+(x >> 1)
        shift = np.where(off & 1, 16, 0)+np.where(v & 1, 0, 8)+np.where(u & 1, 0, 4)
        levels.append(((sheet[(off >> 1) & 0x7ffff].astype(np.int64) >> shift) & 15).astype(np.uint8))
        k += 1
    return levels


def border_mean(index, entries, gamma=2.2):
    """(sum of linear-light colour, count) of the opaque texels next to a hole
    (8-neighbourhood, wrapping like the texture) of a punch-through texture."""
    hole = index == 15
    near = np.zeros_like(hole)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dy or dx:
                near |= np.roll(np.roll(hole, dy, 0), dx, 1)
    border = ~hole & near
    colours = (np.asarray(entries, float)[index[border]]/255.0)**gamma
    return colours.sum(0), int(border.sum())


STIPPLE_MARK = 0x8000  # prepare_face_order.py, stipple_header


def stipple_levels(size, entry):
    """Mip levels of the screen-checker stipple, largest first: each level is
    a checkerboard of palette entry `entry` and the transparent entry 15 at
    its own resolution, so the level the PVR selects always has holes of
    about one pixel. The 1x1 level is a hole: a fence too far to resolve
    disappears instead of turning solid."""
    levels = []
    while size >= 1:
        y, x = np.mgrid[0:size, 0:size]
        levels.append(np.where((x+y) & 1, 15, entry).astype(np.uint8) if size > 1 else np.full((1, 1), 15, np.uint8))
        size //= 2
    return levels


COMPOSITE_MARK = 0x4000  # prepare_face_order.py, flatten_stacks
SPLIT_MARK = 0x2000      # split_cutouts.py, split_header
# --chase-variants: the palette key's lumabase field (7 bits) of the baked colour-cycling textures' palette; the arcade's
# luma tables in use stop far below (course 2: lumabases 0..65).
CHASE_LUMABASE = 127
CHASE_LEVELS = 15        # palette entries 0..14 hold the brightness levels; 15 stays the transparent entry


def monotonic(position):
    return bool((np.diff(position) > 0).all() or (np.diff(position) < 0).all())


def chase_plan(luma, rows, variants):
    """The colour-cycling lumabases of the textured rows: (variant of each lumabase, the mean ramp of each variant, the
    palette's 15 levels). The lumabases are taken in ascending order in `variants` runs of equal length (the arcade
    steps them by one or two a frame, so a run is a few consecutive frames of the wave)."""
    chase = sorted({h[1] & 255 for h, light, resource, *_ in rows
                    if resource != SOLID and h[0] & 0x4000 and not monotonic(ramp(luma, h))})
    if not chase:
        return None
    if len(chase) % variants:
        raise ValueError(f'{len(chase)} colour-cycling lumabases do not split into {variants} variants')
    run = len(chase)//variants
    variant = {lb: i//run for i, lb in enumerate(chase)}
    means = [np.mean([ramp(luma, (0, lb, 0, 0)) for lb in chase[j*run:(j+1)*run]], axis=0) for j in range(variants)]
    lights = {light for h, light, resource, *_ in rows if resource != SOLID and (h[1] & 255) in variant}
    shown = [min(63, int(r)*light//256) for lb in chase for r in ramp(luma, (0, lb, 0, 0)) for light in lights]
    low, high = min(shown), max(shown)
    levels = np.array([int(round(low+(high-low)*e/(CHASE_LEVELS-1))) for e in range(CHASE_LEVELS)]+[high], np.int64)
    return dict(lumabases=chase, variant=variant, means=means, levels=levels, lights=sorted(lights))


def chase_entries(plan, j, light, cutout):
    """Palette entry of each texel index in variant j at `light`: the level of the variant's mean ramp nearest to an
    entry's (entry 15 for index 15 of a punch-through texture)."""
    target = np.minimum(63.0, plan['means'][j]*light/256.0)
    entries = np.array([int(np.argmin(np.abs(plan['levels'][:CHASE_LEVELS]-t))) for t in target], np.uint8)
    if cutout:
        entries[15] = 15
    return entries


def quantize(pixels, colours=16, rounds=40):
    """Palette (colours x 3, float) and index per pixel for an N x 3 array:
    weighted k-means over the distinct colours, started from the most
    frequent colour and then always the colour worst served so far, so the
    result does not depend on a random seed."""
    distinct, inverse, counts = np.unique(pixels.reshape(-1, 3), axis=0, return_inverse=True, return_counts=True)
    inverse = inverse.reshape(-1)
    points = distinct.astype(float)
    if len(points) <= colours:
        palette = np.zeros((colours, 3))
        palette[:len(points)] = points
        return palette, inverse
    centres = [points[int(np.argmax(counts))]]
    while len(centres) < colours:
        distance = np.min([((points-c)**2).sum(1) for c in centres], axis=0)
        centres.append(points[int(np.argmax(distance*counts))])
    centres = np.array(centres)
    for _ in range(rounds):
        nearest = np.argmin(((points[:, None, :]-centres[None, :, :])**2).sum(2), axis=1)
        moved = centres.copy()
        for k in range(colours):
            members = nearest == k
            if members.any():
                moved[k] = (points[members]*counts[members, None]).sum(0)/counts[members].sum()
        if np.allclose(moved, centres):
            break
        centres = moved
    nearest = np.argmin(((points[:, None, :]-centres[None, :, :])**2).sum(2), axis=1)
    return centres, nearest[inverse]


def composite_image(composite, base_light, pal, luma, curve, sheets):
    """RGB picture of a flattened stack under the baked light of its base
    face: each layer with its own colour word, ramp and luma, a punch-through
    layer's entry 15 showing what lies below (the arcade's draw order)."""
    picture = None
    for header, light in zip(composite['layers'], composite['lumas'][str(base_light)]):
        size = (32 << (header[0] & 7), 32 << ((header[0] >> 3) & 7))
        index = texture_indices(sheets, header[2], *size)
        colour = int(pal[((header[3] >> 6) & 0x3ff)+0x1000])
        level = np.minimum(63, ramp(luma, header)*light//256)
        rgb = np.stack([curve[(colour >> (5*c)) & 31][level][index] for c in range(3)], -1).astype(float)
        picture = rgb if picture is None else np.where((index == 15)[..., None], picture, rgb)
    return picture


def composite_levels(picture, palette):
    """Mip levels of a composite, largest first: box-filtered colour, then
    the nearest palette entry (its palette is not a ramp)."""
    levels = []
    while True:
        flat = picture.reshape(-1, 3)
        levels.append(np.argmin(((flat[:, None, :]-palette[None, :, :])**2).sum(2), axis=1)
                      .reshape(picture.shape[:2]).astype(np.uint8))
        if picture.shape[0] == 1:
            return levels
        picture = (picture[0::2, 0::2]+picture[0::2, 1::2]+picture[1::2, 0::2]+picture[1::2, 1::2])/4


def texture_blob(index, mipmaps, inverted, cutout, levels=None):
    """DcTx: 32-byte header, then the twiddled 4-bit payload."""
    height, width = index.shape
    mode = 5 << 27 | (width.bit_length()-4) << 3 | (height.bit_length()-4)
    if levels is not None:
        payload = bytearray([int(levels[-1][0, 0])*17]*2)
        for level in reversed(levels[:-1]):
            payload += pal4_level(level)
        mode |= 1 << 31
    elif mipmaps and width == height:
        # PVR mip chain, smallest level first. At 4 bits per texel the 1x1
        # level is byte 1, 2x2 byte 2, 4x4 byte 4, 8x8 byte 12 (pvrtex
        # MipMapOffset/4); the lead-in byte and both nibbles of the 1x1 byte
        # repeat the 1x1 texel, as pvrtex fills its padding.
        levels, level = [index], index
        while level.shape[0] > 1:
            level = reduce_indices(level, inverted, cutout)
            levels.append(level)
        payload = bytearray([int(levels[-1][0, 0])*17]*2)
        for level in reversed(levels[:-1]):
            payload += pal4_level(level)
        mode |= 1 << 31
    else:
        payload = bytearray(pal4_level(index))
    payload += bytes(-len(payload) % 32)
    return struct.pack('<4sIBBBBHHI12x', b'DcTx', 32+len(payload), 0, 0, 0, 15, width, height, mode)+bytes(payload)


def prepare(source, dump, buckets=3, split=False, mipmaps=False, mesh=None, share=None, palette_banks=None,
            vram_limit=VRAM_MAX, composites=(), split_mesh=None, texture_fill=None, texture_variants=None,
            add_mesh_keys=False, stipple_variants=None, palette_bits=8, mip_filter='index', mip_gamma=2.2,
            mip_coverage=0.5, hole_colour=False, mip_skip_surface=False, hole_scene=None, mip_skip_composites=False,
            mip_skip_textures=(), extra_keys=(), chase_variants=0):
    """mipmaps: False, True/'all', or 'opaque' (see the module text)."""
    if mip_filter not in ('index', 'colour', 'arcade'):
        raise ValueError('mip filter must be index, colour or arcade')
    if not 1 <= buckets <= 6:
        raise ValueError('saturating buckets per channel must be 1..6')
    if palette_bits not in (8, 5):
        raise ValueError('palette bits must be 8 or 5')
    rows, source_resources = read_pack(source)
    added_keys = []
    weights, stipples = None, []
    composites = [dict(c, header=tuple(c['header'])) for c in composites]
    # Baked decals (bake_decals.py) are composites with their own texels and palette rules.
    decals, composites = [c for c in composites if c.get('decal')], [c for c in composites if not c.get('decal')]
    composite_banks = len(composites)
    if decals:
        import bake_decals
        if mesh is None or split:
            raise ValueError('--composites needs --mesh and shared lists')
        composite_banks += bake_decals.banks(decals)
        rows = rows+bake_decals.ramp_rows(decals)
    if composites and (mesh is None or split):
        raise ValueError('--composites needs --mesh and shared lists')
    if extra_keys and mesh is None:
        raise ValueError('--extra-keys needs --mesh')
    if mesh is not None:
        weights = mesh_keys(mesh)
        for key in extra_keys:   # faces switch to these while the scene runs: as if one face showed each
            weights[key] = weights.get(key, 0) or 1
        present = {(row[0], row[1]) for row in rows}
        # Stipple panels (prepare_face_order.py --stipple-panels): the mesh
        # names a synthetic header; its colour is the solid binding of the
        # screen-checker header it replaces.
        solid = {(row[0], row[1]): row[3] for row in rows if row[2] == SOLID}
        for header, light in weights:
            if header[0] & 0x4000 and header[1] == STIPPLE_MARK:
                source_key = ((0x8000, 0, header[2], header[3]), light)
                if source_key not in solid:
                    raise ValueError(f'stipple header {header} has no solid screen-checker binding in the source pack')
                stipples.append((header, light, solid[source_key]))
        missing = [key for key in weights if key not in present and not key[0][1] & 0xff00]
        added_keys = []
        if add_mesh_keys and missing:
            # World objects (prepare_models.py --world-objects) are not in the
            # cells' source pack: their bindings are made here, a textured one
            # like any other, a solid one with the reference colour
            # (material.h, material_solid_color) of the video tables.
            tables = video(dump)
            for header, light in sorted(missing):
                if header[0] & 0x8000:
                    raise ValueError(f'screen-checker header {header} needs a source binding')
                if header[0] & 0x4000:
                    rows.append((header, light, 0, 0, 0xffffffff))
                else:
                    colour = int(tables[0][((header[3] >> 6) & 0x3ff)+0x1000])
                    rgb = [int(tables[3][(colour >> (5*c)) & 31][light >> 2]) for c in range(3)]
                    rows.append((header, light, SOLID, 0xff000000 | rgb[0] << 16 | rgb[1] << 8 | rgb[2], 0xffffffff))
                added_keys.append([list(header), light])
            missing = []
        if missing:
            raise ValueError(f'{len(missing)} baked (header, luma) pairs are absent from the source pack, e.g. {missing[0]}')
        rows = [row for row in rows if (row[0], row[1]) in weights]
    fixed = None
    if share is not None:
        if split or palette_banks is None:
            raise ValueError('--share needs --palette-banks and shared lists')
        fixed = shared_keys(share, buckets)
        if len(fixed)+composite_banks > palette_banks:
            raise ValueError('the shared pack and the composite palettes exceed the palette banks')
    pal, luma, sheets, curve = video(dump)
    ramps, gains, channel_fit, textures, palettes, keys = {}, {}, {}, {}, {}, []
    chase = chase_plan(luma, rows, chase_variants) if chase_variants else None
    chase_report = None
    if chase:
        ramps[CHASE_LUMABASE], gains[CHASE_LUMABASE] = gain_ramps_base(chase['levels'], curve, buckets, palette_bits)
        chase_report = dict(variants=chase_variants, lumabases=[chase['lumabases'][0], chase['lumabases'][-1]],
                            levels=[int(v) for v in chase['levels']], lights=chase['lights'], bindings=0,
                            texture_copies=0, texture_bytes=0, max_error=0.0)
    custom_palettes = {}
    images = {}
    if decals:
        from prepare_palette_probe import exact_palette
        baked = bake_decals.render(decals, pal, luma, curve, sheets,
                                   dict(quantize=quantize, texture_indices=texture_indices, exact_palette=exact_palette))
        images.update(bake_decals.ramp_images(decals, baked))
    out_rows, errors, key_use = [], [], collections.Counter()
    filled_report = {}

    def chase_row(header, light, cutout):
        """A colour-cycling binding (--chase-variants): its variant's texture copy and the chase palette, the vertex
        colour fitted per channel to this lumabase's own colours; the row's last element is its error."""
        j = chase['variant'][header[1] & 255]
        entries = chase_entries(chase, j, light, cutout)
        size = (32 << (header[0] & 7), 32 << ((header[0] >> 3) & 7))
        texture = (header[2], size, 'chase', j, light, cutout)
        if texture not in images:
            images[texture] = entries[texture_indices(sheets, header[2], *size)]
            textures[texture] = {'index': len(textures), 'inverted': {False}, 'cutout': {cutout}, 'nomip': True}
            chase_report['texture_copies'] += 1
            chase_report['texture_bytes'] += size[0]*size[1]//2
        shown = range(15 if cutout else 16)
        colour = int(pal[((header[3] >> 6) & 0x3ff)+0x1000])
        level = np.minimum(63, ramp(luma, header)*light//256)
        fits, choices = [], []
        for c in range(3):
            c5 = (colour >> (5*c)) & 31
            target = curve[c5][level].astype(float)[list(shown)]
            every = fit_buckets(target, [g[entries][list(shown)] for g in ramps[CHASE_LUMABASE]])
            choices.append(every)
            fits.append(min(every, key=lambda fit: (fit[0], fit[1])))
        wanted = 1 if cutout or not split else 0 if bool((images[texture] == 15).any()) else None
        key = (fits[0][1], fits[1][1], fits[2][1], CHASE_LUMABASE, wanted)
        vertex = 0xff000000 | fits[0][2] << 16 | fits[1][2] << 8 | fits[2][2]
        error = max(f[0] for f in fits)
        chase_report['bindings'] += 1
        chase_report['max_error'] = max(chase_report['max_error'], error)
        return [header, light, textures[texture]['index'], 0, vertex, key, choices,
                weights[header, light] if weights else 1, error]
    for header, light, resource, crop, modulation in rows:
        if resource == SOLID:
            out_rows.append((header, light, SOLID, crop, modulation, 255))
            continue
        if not header[0] & 0x4000 or (header[0] & 0x8000 and not translucent(header)) or \
                (header[0] >> 13) & 3 not in (2, 3) or header[1] & 0xff00:
            raise ValueError(f'unsupported textured header {header}')
        lumabase, cutout = header[1] & 255, (header[0] >> 13) & 3 == 3
        if chase and lumabase in chase['variant']:
            out_rows.append(chase_row(header, light, cutout))
            errors.append(out_rows[-1].pop())
            continue
        if lumabase not in ramps:
            ramps[lumabase], gains[lumabase] = gain_ramps(luma, curve, header, buckets, palette_bits)
            position = ramp(luma, header)
            if not ((np.diff(position) > 0).all() or (np.diff(position) < 0).all()):
                raise ValueError('luma ramp is not monotonic; the index mip reduction assumes it is')
        colour = int(pal[((header[3] >> 6) & 0x3ff)+0x1000])
        level = np.minimum(63, ramp(luma, header)*light//256)
        fits, choices = [], []
        for c in range(3):
            c5 = (colour >> (5*c)) & 31
            every = channel_fit.get((lumabase, c5, light))
            if every is None:
                every = channel_fit[(lumabase, c5, light)] = fit_buckets(curve[c5][level].astype(float), ramps[lumabase])
            choices.append(every)
            fits.append(min(every, key=lambda fit: (fit[0], fit[1])))
        size = (32 << (header[0] & 7), 32 << ((header[0] >> 3) & 7))
        texture = (header[2], size)
        if texture not in images:
            images[texture] = texture_indices(sheets, header[2], *size)
            if texture_fill and header[2] in texture_fill:
                # Texels a specification of opaque_cores.py changes (holes
                # filled, gutters): entries of the texture's own palette.
                images[texture] = images[texture].copy()
                for x, y, value in texture_fill[header[2]]:
                    if not 0 <= value < 15 or images[texture][y, x] != 15:
                        raise ValueError(f'texture {header[2]}: a fill must turn a transparent texel into entry 0..14')
                    images[texture][y, x] = value
                filled_report[str(header[2])] = dict(texels=len(texture_fill[header[2]]), bytes=32+size[0]*size[1]//2,
                                                    replaces_original=True)
        entry = textures.setdefault(texture, {'index': len(textures), 'inverted': set(), 'cutout': set()})
        entry['inverted'].add(bool(ramp(luma, header)[0] > ramp(luma, header)[15]))
        entry['cutout'].add(cutout)
        # List of the palette. Shared: always the alpha-0 variant. Split: the
        # punch-through list needs alpha 0 at entry 15 and an opaque binding
        # whose texture contains entry 15 needs it opaque; an opaque binding
        # without entry 15 takes whichever variant exists (None, resolved below).
        wanted = 1 if cutout or not split else 0 if bool((images[texture] == 15).any()) else None
        key = (fits[0][1], fits[1][1], fits[2][1], lumabase, wanted)
        vertex = 0xff000000 | fits[0][2] << 16 | fits[1][2] << 8 | fits[2][2]
        # Crop field 0: origin 0 at full size, exactly as the DNT4 source.
        out_rows.append([header, light, entry['index'], 0, vertex, key, choices,
                         weights[header, light] if weights else 1])
        errors.append(max(f[0] for f in fits))
    budget_report = None
    if fixed is not None:
        # Palette budget: the car palettes are free, the world may add
        # palette_banks-len(fixed). Cost of a key for a binding: its worst
        # channel error with that key's buckets. Greedy: add the key that
        # removes the most face-weighted error until the banks are used up.
        textured = [row for row in out_rows if row[2] != SOLID]

        def cost(row, key):
            return max(row[6][c][key[c]][0] for c in range(3)) if key[3] == row[5][3] else float('inf')
        allowed = set(fixed)
        best = {id(row): min((cost(row, key) for key in allowed), default=float('inf')) for row in textured}
        wanted = {row[5][:4] for row in textured}
        free_error = sum(row[7]*max(row[6][c][row[5][c]][0] for c in range(3)) for row in textured)
        added = []
        # A composite brings a palette of its own: one bank each.
        while len(allowed) < palette_banks-composite_banks:
            gains = {}
            for key in wanted-allowed:
                gain = 0.0
                for row in textured:
                    now, then = best[id(row)], cost(row, key)
                    if then < now:
                        gain += row[7]*(1e6 if now == float('inf') else now-then)
                if gain > 0:
                    gains[key] = gain
            if not gains:
                break
            key = max(gains, key=lambda k: (gains[k], k))
            allowed.add(key)
            added.append(key)
            for row in textured:
                best[id(row)] = min(best[id(row)], cost(row, key))
        moved = collections.Counter()
        errors = []
        for row in textured:
            free = row[5][:4]
            key = min(allowed, key=lambda k: (cost(row, k), k != free, k))
            if cost(row, key) == float('inf'):
                raise ValueError(f'no palette bank left for lumabase {free[3]}')
            if key != free:
                moved[free, key] += row[7]
            row[4] = 0xff000000 | row[6][0][key[0]][2] << 16 | row[6][1][key[1]][2] << 8 | row[6][2][key[2]][2]
            row[5] = key+(1,)
            errors.append(cost(row, key))
        used = {row[5][:4] for row in textured}
        constrained = sum(row[7]*e for row, e in zip(textured, errors))
        total = sum(row[7] for row in textured)
        budget_report = dict(
            banks=palette_banks, shared_pack_palettes=len(fixed), world_palettes=len(used),
            world_palettes_shared=len(used & set(fixed)), world_palettes_added=len(used-set(fixed)),
            banks_used=len(set(fixed) | used), unconstrained_world_palettes=len(wanted),
            unconstrained_union=len(set(fixed) | wanted),
            weighted_mean_error_unconstrained=free_error/total, weighted_mean_error=constrained/total,
            remapped=[dict(wanted=list(a), used=list(b), weight=n) for (a, b), n in sorted(moved.items())])
    stipple_report = None
    if stipples:
        # No palette of its own: the brightest grey entry of a palette the
        # pack already uses (alpha 0 at entry 15), times the vertex colour.
        best = None
        for row in out_rows:
            if row[2] == SOLID or row[5][4] != 1 or len(set(row[5][:3])) != 1:
                continue
            channel = ramps[row[5][3]][row[5][0]]
            entry = int(np.argmax(channel[:15]))
            if best is None or (channel[entry], row[5]) > (best[0], best[1]):
                best = (float(channel[entry]), row[5], entry)
        if best is None or best[0] < 128:
            raise ValueError('no grey punch-through palette for the stipple panels')
        level, key, entry = best
        side = 32 << (stipples[0][0][0] & 7)
        if any(header[0] & 63 != stipples[0][0][0] & 63 or side != 32 << ((header[0] >> 3) & 7) for header, _, _ in stipples):
            raise ValueError('stipple headers differ in size')
        images['stipple', (side, side)] = stipple_levels(side, entry)[0]
        stipple_entry = textures.setdefault(('stipple', (side, side)), {
            'index': len(textures), 'inverted': {False}, 'cutout': {True}, 'levels': stipple_levels(side, entry)})
        for header, light, colour in stipples:
            vertex = 0xff000000
            for shift in (16, 8, 0):
                vertex |= min(255, int(round(((colour >> shift) & 255)*255/int(round(level))))) << shift
            out_rows.append([header, light, stipple_entry['index'], 0, vertex, key, None, weights[header, light]])
        stipple_report = dict(bindings=len(stipples), texture_side=side, palette_key=list(key), palette_entry=entry,
                              entry_level=level, colours=['%08x' % c for _, _, c in stipples])
        stipple_side, stipple_index = side, entry
    composite_report = []
    for number, composite in enumerate(composites):
        # Flattened stack (prepare_face_order.py --flatten-stacks): one opaque
        # 4-bit texture and one 16-colour palette. The palette is the picture
        # under the brightest baked luma; a darker luma is its vertex colour,
        # fitted per channel over the picture's texels.
        lights = sorted(light for header, light in weights if header == composite['header'])
        if not lights:
            raise ValueError(f'composite {composite["header"]} has no face in the mesh')
        if (composite['header'][0] >> 13) & 7 != 2 or composite['header'][1] & 0xff00 != COMPOSITE_MARK:
            raise ValueError('composite header is not an opaque synthetic one')
        pictures = {light: composite_image(composite, light, pal, luma, curve, sheets) for light in lights}
        reference = pictures[lights[-1]]
        colours, index = quantize(np.rint(reference).astype(np.int64))
        colours = palette_level(np.clip(np.rint(colours), 0, 255), palette_bits).astype(float)
        index = index.reshape(reference.shape[:2]).astype(np.uint8)
        key = ('composite', number, 0, 0, 1)
        entry = textures.setdefault(('composite', number), {'index': len(textures), 'inverted': {False}, 'cutout': {False}})
        images['composite', number] = index
        entry['size'] = (index.shape[1], index.shape[0])
        if mipmaps and index.shape[0] == index.shape[1] and not mip_skip_composites:
            entry['levels'] = composite_levels(reference, colours)
        custom_palettes[key] = colours
        detail = []
        for light in lights:
            shown, target = colours[index], pictures[light]
            vertex, value = 0xff000000, []
            for c, shift in enumerate((16, 8, 0)):
                v = min(255, max(0, int(round(float((shown[..., c]*target[..., c]).sum()/max((shown[..., c]**2).sum(), 1e-9))*255))))
                vertex |= v << shift
                value.append(v)
            drawn = np.rint(shown*np.array(value)/255)
            error = np.abs(drawn-target)
            detail.append(dict(luma=light, faces=weights[composite['header'], light], vertex='%08x' % vertex,
                               max_error=float(error.max()), mean_error=float(error.mean()),
                               texels_off_by_more_than_16=float((error.max(-1) > 16).mean())))
            out_rows.append([composite['header'], light, entry['index'], 0, vertex, key, None,
                             weights[composite['header'], light]])
        composite_report.append(dict(header=list(composite['header']), layers=[list(h) for h in composite['layers']],
                                     source_colours=int(len(np.unique(np.rint(reference).reshape(-1, 3), axis=0))),
                                     palette=['%02x%02x%02x' % tuple(int(v) for v in c) for c in colours], lumas=detail))
    if decals:
        bake_decals.add_materials(decals, baked, weights, textures, images, custom_palettes, out_rows, composite_report)
    split_report = None
    supersampled = set()   # rows of super-sampled and converted headers (copies: they weigh in nowhere)
    if split_mesh is not None:
        if mesh is None or split or (mipmaps and mipmaps != 'opaque' and mip_filter == 'index'):
            raise ValueError('--split-mesh needs --mesh, shared lists and cutout textures without mip chains '
                             '(or --mip-filter colour|arcade, which leaves split textures without them)')
        import lod_groups
        import pt_to_opaque
        converted_rows = 0
        sources = {(tuple(row[0]), row[1]): row for row in out_rows if row[2] != SOLID}
        by_index = {entry['index']: key for key, entry in textures.items()}
        added, copies = 0, {}
        for (header, light), faces in sorted(mesh_keys(split_mesh).items()):
            if lod_groups.supersample_base(header) is not None and (header, light) in weights:
                raise ValueError(f'--mesh already shows the super-sampled header {header}')
            # An opaque split header, a texture-variant header (level of detail) or both.
            derived = lod_groups.base_header(header)
            sparse = lod_groups.stipple_base(header)
            fine = lod_groups.supersample_base(header)
            hole_free = pt_to_opaque.converted_base(header)
            if hole_free is not None:
                # A punch-through face moved to the opaque list (pt_to_opaque.py): the source binding
                # as it is; the texture keeps its chain, and the row weighs in no mip level and no hole colour.
                source = sources.get((hole_free, light))
                if source is None:
                    raise ValueError(f'converted header {header} luma {light} has no punch-through binding to copy')
                out_rows.append([header, light, source[2], source[3], source[4], source[5], None, faces])
                supersampled.add(len(out_rows)-1)
                converted_rows += 1
                added += 1
            elif fine is not None:
                # The far level of a supersample rule: the binding of the header without the mark, as it is.
                source = sources.get((fine, light))
                if source is None:
                    raise ValueError(f'super-sampled header {header} luma {light} has no binding of {fine} to copy')
                out_rows.append([header, light, source[2], source[3], source[4], source[5], None, faces])
                supersampled.add(len(out_rows)-1)
                added += 1
            elif sparse is not None:
                # A sparser stipple (a level of the checker panels): its own synthetic
                # texture, the binding of the plain stipple header otherwise.
                base, variant = sparse
                source = sources.get((base, light))
                if source is None or stipple_report is None:
                    raise ValueError(f'stipple header {header} luma {light} has no plain stipple binding to copy')
                cell = (stipple_variants or {}).get(variant)
                if cell is None:
                    raise ValueError(f'header {header} asks for stipple variant {variant}, which --texture-fill does not describe')
                key = ('stipple', (stipple_side, stipple_side), variant)
                if key not in textures:
                    levels, size = [], stipple_side
                    while size >= 1:
                        levels.append(np.array(lod_groups.stipple_image(cell, size, stipple_index), np.uint8))
                        size //= 2
                    if (levels[0] != 15).any() and ((levels[0] != 15) & (stipple_levels(stipple_side, stipple_index)[0] == 15)).any():
                        raise ValueError('a sparser stipple is opaque where the checkerboard has a hole')
                    images[key] = levels[0]
                    textures[key] = {'index': len(textures), 'inverted': {False}, 'cutout': {True}, 'levels': levels}
                    stipple_report.setdefault('variants', {})[str(variant)] = dict(
                        cell=list(cell), coverage=float((levels[0] != 15).mean()),
                        bytes=len(texture_blob(levels[0], False, False, True, levels))-32,   # with its mip levels, as the checkerboard
                        size=[stipple_side, stipple_side], bindings=0)
                stipple_report['variants'][str(variant)]['bindings'] += 1
                out_rows.append([header, light, textures[key]['index'], source[3], source[4], source[5], None, faces])
                added += 1
            elif derived is not None:
                base, variant, _ = derived
                source = sources.get((base, light))
                if source is None:
                    raise ValueError(f'split header {header} luma {light} has no punch-through binding to copy')
                resource = source[2]
                if variant:
                    # A second copy of the texture with this variant's texels; same palette and colour.
                    original = by_index[resource]
                    key = (header[2], original[1], 'variant', variant)
                    if key not in textures:
                        changes = (texture_variants or {}).get((header[2], variant))
                        if changes is None or original[0] != header[2]:
                            raise ValueError(f'header {header} asks for texture variant {variant}, which --texture-fill does not describe')
                        image = texture_indices(sheets, header[2], *original[1]).copy()
                        for x, y, value in changes:
                            if not 0 <= value < 15 or image[y, x] != 15:
                                raise ValueError(f'texture {header[2]}: a fill must turn a transparent texel into entry 0..14')
                            image[y, x] = value
                        images[key] = image
                        # Never a mip chain: its shapes are cut on the full-size texels.
                        textures[key] = {'index': len(textures), 'inverted': set(textures[original]['inverted']),
                                         'cutout': {True, False}, 'size': original[1]}
                        copies['%d:%d' % (header[2], variant)] = dict(texels=len(changes), bytes=original[1][0]*original[1][1]//2,
                                                                      size=list(original[1]))
                    resource = textures[key]['index']
                elif (header[0] >> 13) & 3 != 3:
                    # The opaque piece samples the punch-through texture: drawn in
                    # both lists, it keeps no mip chain (decided below).
                    textures[by_index[resource]]['cutout'].add(False)
                out_rows.append([header, light, resource, source[3], source[4], source[5], None, faces])
                added += 1
            elif (header, light) not in weights:
                raise ValueError(f'the split mesh shows {header} luma {light}, which the unsplit mesh does not')
        split_report = dict(bindings=added, **(dict(supersampled_bindings=len(supersampled)-converted_rows)
                                               if len(supersampled) > converted_rows else {}),
                            **(dict(converted_bindings=converted_rows) if converted_rows else {}),
                            **(dict(texture_copies=copies, texture_copy_bytes=sum(c['bytes'] for c in copies.values()))
                                               if copies else {}))
    # The same vertex colours with the ramps read through an ARGB1555 palette
    # RAM (NATIVE_PAL16=1555 loading this pack), against the arcade colour as
    # model_error (round(entry x vertex / 255)); the bindings model_error
    # covers that have a fit of their own (not split, stipple or composite copies).
    as_1555 = []
    for row in out_rows:
        if row[2] == SOLID or row[6] is None or row[5][3] == CHASE_LUMABASE:
            continue
        header, light, key = row[0], row[1], row[5]
        colour = int(pal[((header[3] >> 6) & 0x3ff)+0x1000])
        level = np.minimum(63, ramp(luma, header)*light//256)
        worst = [0.0, 0.0]
        for c, shift in enumerate((16, 8, 0)):
            target = curve[(colour >> (5*c)) & 31][level].astype(float)
            entries = np.rint(ramps[key[3]][key[c]])
            v = (row[4] >> shift) & 255
            for k, shown in enumerate((entries, palette_level(entries, 5))):
                worst[k] = max(worst[k], float(np.abs(np.round(shown*v/255)-target).max()))
        as_1555.append(worst)
    required = {row[5] for row in out_rows if row[2] != SOLID and row[5][4] is not None}
    for row in out_rows:
        if row[2] == SOLID:
            continue
        key = row[5]
        if key[4] is None:
            key = key[:4]+(1 if key[:4]+(1,) in required or key[:4]+(0,) not in required else 0,)
        if key not in palettes:
            palettes[key] = len(keys)
            keys.append(key)
        key_use[key] += 1
        row[5] = palettes[key]
    if len(keys) > PALETTES_MAX:
        raise ValueError(f'{len(keys)} palettes exceed the loader limit {PALETTES_MAX}')
    if len(textures) > RESOURCES_MAX or not 1 <= len(out_rows) <= BINDINGS_MAX:
        raise ValueError('resource or binding count exceeds the loader limits')
    table = []
    for bucket_r, bucket_g, bucket_b, lumabase, cutout in keys:
        if bucket_r == 'composite':
            # Every palette of a shared-list pack has alpha 0 at entry 15; the
            # opaque list ignores texture alpha, so all 16 entries are colours.
            custom = custom_palettes[bucket_r, bucket_g, bucket_b, lumabase, cutout]
            table.append([(0 if t == 15 else 255) << 24 | int(c[0]) << 16 | int(c[1]) << 8 | int(c[2])
                          for t, c in enumerate(custom)])
            continue
        channels = [ramps[lumabase][b] for b in (bucket_r, bucket_g, bucket_b)]
        row = []
        for t in range(16):
            alpha = 0 if t == 15 and cutout else 255
            row.append(alpha << 24 | int(round(channels[0][t])) << 16 | int(round(channels[1][t])) << 8 | int(round(channels[2][t])))
        table.append(row)
    by_resource = {entry['index']: texture for texture, entry in textures.items()}
    hole_report = None
    if hole_colour:
        import lod_groups
        # --hole-colour: entry 15 of a punch-through palette is transparent, but
        # bilinear filtering (and a punch-through alpha reference below 1.0)
        # blends its RGB into the texels next to a hole. Its RGB becomes the
        # mean colour of the opaque texels bordering holes in the punch-through
        # textures drawn with it (face weighted), where no opaque polygon shows
        # entry 15 as a colour (an opaque texture of the palette containing
        # index 15, or an opaque split piece). A palette equal to one of the
        # shared pack's (--share) apart from entry 15's RGB takes the shared
        # pack's words, so the banks stay shared; it is otherwise left alone.
        rgb_of = lambda words: [((w >> 16) & 255, (w >> 8) & 255, w & 255) for w in words]
        shared_words = {tuple(w for t, w in enumerate(words) if t != 15)+(words[15] >> 24,): list(words)
                        for words in (fixed or {}).values()}
        sums, blocked = {}, set()
        for at, row in enumerate(out_rows):
            if row[2] == SOLID or keys[row[5]][0] == 'composite' or at in supersampled:
                continue
            image = images[by_resource[row[2]]]
            if (row[0][0] >> 13) & 3 == 3:
                total, count = border_mean(image, rgb_of(table[row[5]]))
                acc = sums.setdefault(row[5], [np.zeros(3), 0.0, 0])
                acc[0] += total*row[7]
                acc[1] += count*row[7]
                acc[2] += count
            elif (image == 15).any() and not (split_mesh is not None and row[6] is None and
                                              lod_groups.base_header(tuple(row[0])) is not None):
                blocked.add(row[5])   # an opaque polygon (not a split cutout's opaque piece) shows entry 15
        scene_shown = opaque_entry15_palettes(hole_scene) if hole_scene is not None else set()
        for p, words in enumerate(table):
            if tuple(words) in scene_shown:
                blocked.add(p)
        hole_report = []
        for p, words in enumerate(table):
            if keys[p][0] == 'composite' or not keys[p][4]:
                continue
            same = shared_words.get(tuple(w for t, w in enumerate(words) if t != 15)+(words[15] >> 24,))
            old = '%06x' % (words[15] & 0xffffff)
            if same is not None:
                if same[15] != words[15] and p in blocked:
                    raise ValueError(f'palette {p}: the shared pack recoloured entry 15, which an opaque polygon here '
                                     'shows (build the shared pack with --hole-colour-scene)')
                table[p] = list(same)
                hole_report.append(dict(palette=p, action='shared pack palette', entry15=old,
                                        now='%06x' % (same[15] & 0xffffff)))
            elif p not in sums:
                hole_report.append(dict(palette=p, action='no punch-through texture', entry15=old))
            elif p in blocked:
                hole_report.append(dict(palette=p, action='kept: an opaque polygon shows entry 15', entry15=old,
                                        border_texels=sums[p][2]))
            else:
                mean = 255.0*(sums[p][0]/max(sums[p][1], 1e-9))**(1/2.2)
                rgb = [int(v) for v in palette_level(np.clip(np.rint(mean), 0, 255), palette_bits)]
                table[p][15] = (words[15] & 0xff000000) | rgb[0] << 16 | rgb[1] << 8 | rgb[2]
                hole_report.append(dict(palette=p, action='recoloured', entry15=old, now='%02x%02x%02x' % tuple(rgb),
                                        border_texels=sums[p][2]))
    skipped = set()
    if mip_skip_surface:
        if mesh is None:
            raise ValueError('--mip-skip-surface needs --mesh with scenery classes')
        skipped = surface_words(mesh)
    unknown = set(mip_skip_textures)-{texture[0] for texture in textures if isinstance(texture[0], int)}
    if unknown:
        raise ValueError('--mip-skip-textures: no texture with word %s' % ','.join(map(str, sorted(unknown))))
    users =collections.defaultdict(collections.Counter)
    if mipmaps and mip_filter != 'index':
        for at, row in enumerate(out_rows):
            if row[2] != SOLID and at not in supersampled:
                users[row[2]][row[5], row[4]] += row[7]
    blobs, mip_cost, mipped = [], 0, 0
    for texture, entry in textures.items():
        index = images[texture]
        width, height = entry['size'] if 'size' in entry else texture[1]
        # A texture drawn with both ramps or in both lists keeps mip levels only
        # if one reduction serves every user. The colour and arcade filters do
        # not depend on the ramp's direction.
        mip = mipmaps and (len(entry['inverted']) == 1 or mip_filter != 'index') and len(entry['cutout']) == 1 and \
            (mipmaps != 'opaque' or not next(iter(entry['cutout']))) and \
            not (isinstance(texture[0], int) and (texture[0] in skipped or texture[0] in mip_skip_textures)) and \
            not (mip_skip_composites and texture[0] == 'composite') and not entry.get('nomip')
        mipped += bool(mip and width == height)
        levels = entry.get('levels')
        if mip and width == height and levels is None and mip_filter != 'index':
            cutout = next(iter(entry['cutout']))
            if mip_filter == 'arcade' and isinstance(texture[0], int) and len(texture) == 2 and \
                    str(texture[0]) not in filled_report:
                levels = arcade_levels(sheets, texture[0], width)
            else:
                shown = []
                for (p, vertex), weight in users[entry['index']].items():
                    tint = np.array([(vertex >> 16) & 255, (vertex >> 8) & 255, vertex & 255], float)/255.0
                    shown.append((np.array([((w >> 16) & 255, (w >> 8) & 255, w & 255) for w in table[p]], float)*tint,
                                  weight))
                levels = colour_levels(index, shown, cutout, mip_gamma, mip_coverage)
        blob = texture_blob(index, mip, next(iter(entry['inverted'])), next(iter(entry['cutout'])), levels)
        plain = 32+width*height//2
        mip_cost += len(blob)-plain
        blobs.append((blob, width, height))
    count = len(out_rows)
    data = bytearray(32+16*len(blobs))
    out_rows.sort(key=lambda r: struct.pack('<5H', *r[0], r[1]))
    data += b''.join(BINDING.pack(*r[0], r[1], r[2], r[3]) for r in out_rows)
    data += b''.join(struct.pack('<I', (r[4] & 0xffffff) | TRANSLUCENT_ALPHA << 24 if r[2] != SOLID and translucent(r[0])
                                 else r[4]) for r in out_rows)
    data += bytes(r[5] for r in out_rows)+bytes(-count % 4)
    data += b''.join(struct.pack('<16I', *row) for row in table)
    # A composite's palette has no gain buckets: key 0,0,0 with the list bit. The chase palette's key names lumabase 0
    # (the loader takes lumabases 0 and 1 only; banks are shared by palette content, not by key).
    data += b''.join(struct.pack('<4B', *((0, 0, 0, 0x80) if k[0] == 'composite' else
                                         (k[0], k[1], k[2], (0 if k[3] == CHASE_LUMABASE else k[3]) | k[4] << 7)))
                     for k in keys)
    metadata = len(data)-32
    data += bytes(-len(data) % 32)
    payload = staging = 0
    for i, (blob, width, height) in enumerate(blobs):
        RESOURCE.pack_into(data, 32+16*i, len(data), len(blob), width, height, 0)
        data += blob
        payload += len(blob)-32
        staging = max(staging, len(blob))
    flags = int(split) | buckets << 8 | int(bool(mipmaps)) << 16
    HEADER.pack_into(data, 0, b'DNT6', len(data), len(blobs), count, payload, staging, len(keys), flags)
    if len(data) > FILE_MAX or payload > vram_limit:
        raise ValueError('pack exceeds the loader file or VRAM limits')
    errors = np.array(errors)
    report = dict(
        format='DNT6', bytes=len(data), sha256=hashlib.sha256(bytes(data)).hexdigest(),
        resources=len(blobs), source_resources=source_resources, bindings=count, textured_bindings=len(errors),
        palettes=len(keys), palette_limit_loader=PALETTES_MAX, saturating_buckets=buckets,
        translucent_bindings=sum(1 for r in out_rows if r[2] != SOLID and translucent(r[0])),
        lists='split' if split else 'shared', mipmaps=mipmaps if mipmaps else False, mipmap_extra_bytes=mip_cost,
        mipmapped_resources=mipped, palette_budget=budget_report, stipple=stipple_report,
        **(dict(mip_filter=mip_filter, mip_gamma=mip_gamma, mip_coverage=mip_coverage)
           if mipmaps and mip_filter != 'index' else {}),
        **(dict(hole_colour=hole_report) if hole_colour else {}),
        **(dict(mip_skipped_surface_words=sorted(skipped)) if mip_skip_surface else {}),
        **(dict(mip_skipped_composites=True) if mip_skip_composites else {}),
        **(dict(mip_skipped_textures=sorted(mip_skip_textures)) if mip_skip_textures else {}),
        composites=composite_report, split_cutouts=split_report,
        **(dict(chase=chase_report, extra_keys=len(extra_keys)) if chase_variants or extra_keys else {}),
        **(dict(added_mesh_keys=added_keys) if added_keys else {}),
        **(dict(texture_fill=filled_report) if texture_fill else {}),
        vram_payload_bytes=payload, vram_limit_bytes=vram_limit, metadata_ram_bytes=metadata,
        palette_ram_entries=16*len(keys), file_limit_bytes=FILE_MAX,
        gains={str(k): v for k, v in gains.items()},
        model_error=dict(max=float(errors.max()), mean_of_binding_max=float(errors.mean()),
                         share_above_16=float((errors > 16).mean())),
        palette_bits=palette_bits,
        model_error_as_1555=dict(
            bindings=len(as_1555),
            stored=dict(max=max(e[0] for e in as_1555), mean_of_binding_max=float(np.mean([e[0] for e in as_1555]))),
            argb1555=dict(max=max(e[1] for e in as_1555), mean_of_binding_max=float(np.mean([e[1] for e in as_1555])),
                          share_above_16=float(np.mean([e[1] > 16 for e in as_1555])))) if as_1555 else None,
        keys=[dict(buckets=list(k[:3]), lumabase=k[3], list=k[4], bindings=key_use[k]) for k in keys
              if k[0] != 'composite'])
    return bytes(data), report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('source', type=Path, help='DNT4 pack supplying the binding keys and solid bindings')
    parser.add_argument('video', type=Path, help='reference dump: palram/colorxlat/lumaram/tex0/tex1 .bin')
    parser.add_argument('output', type=Path, help='NEW file')
    parser.add_argument('--buckets', type=int, default=3, help='saturating gain buckets per channel (default 3)')
    parser.add_argument('--split-palettes', action='store_true',
                        help='separate palettes for the opaque and punch-through lists (entry 15 opaque in the former)')
    parser.add_argument('--mipmaps', action='store_true', help='mip chains for square textures (not verified on hardware)')
    parser.add_argument('--mipmaps-for', choices=('opaque', 'all'),
                        help='mip chains for square textures with opaque users only, or all')
    parser.add_argument('--mesh', type=Path, help='baked DNM6 scenery pack: keep only the bindings its faces show')
    parser.add_argument('--share', type=Path, help='DNT6 car pack of the scene whose palette banks are shared')
    parser.add_argument('--palette-banks', type=int, help='palette banks for both packs together')
    parser.add_argument('--vram-limit', type=int, default=VRAM_MAX, help='resident texture bytes of the loader')
    parser.add_argument('--palette-bits', type=int, choices=(8, 5), default=8,
                        help='5: ramp entries a 5-bit (ARGB1555) palette channel shows, vertex colours fitted to them')
    parser.add_argument('--composites', type=Path,
                        help='composite materials of the mesh (prepare_face_order.py --flatten-stacks --composites)')
    parser.add_argument('--split-mesh', type=Path,
                        help='the scenery pack with split cutouts; --mesh is the same pack without the split')
    parser.add_argument('--texture-fill', type=Path, metavar='SPEC_JSON',
                        help='specification of opaque_cores.py: its fill and gutter texels replace transparent '
                             'texels of the named textures (entries of the same palette; the texture stays PAL4)')
    parser.add_argument('--add-mesh-keys', action='store_true',
                        help='make the bindings the mesh shows and the source pack lacks (world objects)')
    parser.add_argument('--mip-filter', choices=('index', 'colour', 'arcade'), default='index',
                        help='how mip levels are made (with --mipmaps/--mipmaps-for): index (default) 2x2 means of '
                             'ramp positions; colour: box means of the displayed colours in linear light under every '
                             'palette and vertex colour of the texture, then the nearest entry, a punch-through texel '
                             'transparent below --mip-coverage (textures of split cutouts keep no chain, so '
                             '--mipmaps-for all works with --split-mesh); arcade: the levels the arcade stores in its '
                             'texture sheets (colour for textures this tool changed)')
    parser.add_argument('--mip-gamma', type=float, default=2.2, help='--mip-filter colour: display gamma of the means')
    parser.add_argument('--mip-coverage', type=float, default=0.5,
                        help='--mip-filter colour: opaque share below which a punch-through level texel is a hole')
    parser.add_argument('--hole-colour', action='store_true',
                        help='entry 15 (transparent) of a punch-through palette takes the mean colour of the opaque '
                             'texels bordering holes, where no opaque polygon shows entry 15 (see the report)')
    parser.add_argument('--hole-colour-scene', type=Path, metavar='OTHER.dnt',
                        help='with --hole-colour (car pack): also keep entry 15 of palettes equal to one an opaque '
                             'binding of OTHER.dnt (the scene\'s world pack) shows entry 15 with')
    parser.add_argument('--mip-skip-surface', action='store_true',
                        help='no mip chain for textures of road-surface class materials of --mesh (road, kerbs, '
                             'verges: seen at grazing angles, where the PVR\'s isotropic level choice blurs them)')
    parser.add_argument('--mip-skip-composites', action='store_true',
                        help='no mip chain for the flattened stacks (--composites: the grandstands, seen along the track)')
    parser.add_argument('--mip-skip-textures', type=lambda text: frozenset(int(w) for w in text.split(',')), default=frozenset(),
                        metavar='W1,W2,...', help='no mip chain for the textures with these texture words (header word 2): '
                        'flat ground outside the road-surface class, e.g. the forest floor')
    parser.add_argument('--extra-keys', type=Path, metavar='FILE',
                        help='(texture header, luma) pairs faces of --mesh switch to while the scene runs, '
                             '"h0 h1 h2 h3 luma" a line: bindings are made for them as for the mesh\'s own')
    parser.add_argument('--chase-variants', type=int, default=0, metavar='N',
                        help='colour-cycling luma tables (not monotonic): the wave baked into N texture copies with one '
                             'palette of brightness levels (see the module text)')
    parser.add_argument('--report', type=Path, help='NEW JSON report file')
    args = parser.parse_args()
    extra = []
    if args.extra_keys:
        for line in args.extra_keys.read_text().split('\n'):
            if line.strip():
                h0, h1, h2, h3, light = map(int, line.split())
                extra.append(((h0, h1, h2, h3), light))
    fill = variants = cells = None
    if args.texture_fill:
        described = json.loads(args.texture_fill.read_text())
        if 'rules' in described:   # a level-of-detail specification
            import lod_groups
            loaded = lod_groups.load(args.texture_fill)
            fill, variants = lod_groups.texture_fill(loaded)
            cells = lod_groups.stipple_variants(loaded)[1]
        else:
            fill = {int(word): entry.get('fill', [])+entry.get('gutter', [])
                    for word, entry in described.get('textures', {}).items()}
    data, report = prepare(args.source, args.video, args.buckets, args.split_palettes,
                           args.mipmaps_for or ('all' if args.mipmaps else False),
                           args.mesh, args.share, args.palette_banks, args.vram_limit,
                           json.loads(args.composites.read_text()) if args.composites else (), args.split_mesh,
                           fill, variants, args.add_mesh_keys, cells, args.palette_bits, args.mip_filter,
                           args.mip_gamma, args.mip_coverage, args.hole_colour, args.mip_skip_surface,
                           args.hole_colour_scene, args.mip_skip_composites, args.mip_skip_textures, extra,
                           args.chase_variants)
    with args.output.open('xb') as out:
        out.write(data)
    text = json.dumps(report, indent=1)
    if args.report:
        with args.report.open('x') as out:
            out.write(text)
    print(json.dumps({k: v for k, v in report.items() if k != 'keys'}, indent=1))
