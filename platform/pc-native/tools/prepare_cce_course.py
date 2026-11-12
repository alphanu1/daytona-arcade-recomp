#!/usr/bin/env python3
"""A course of Daytona USA Circuit Edition (Saturn, 1996) as a world for the PC build.

    python3 prepare_cce_course.py CCE_DAYTONA_DIR CCE_COURSE SLOT ASSET_SET [--profiles N] [--name STEM]

CCE_DAYTONA_DIR  the DAYTONA folder of your Circuit Edition disc (COURSEn.MDL/.TEX)
CCE_COURSE       CCE's course number: 3 National Park Speedway, 4 Desert City
SLOT             the course slot of the PC build (3, 4)
ASSET_SET        writes courseSLOT-world.dnm, courseSLOT-world.dnt and courseSLOT.dnv there
--profiles N     road profiles of the course's road pack (the visibility pack's count must equal it)
--name STEM      write STEM-world.dnm/.dnt and STEM.dnv instead (a test: "world"/"scenery" replaces course 0's)

CCE's course files (decoded here; tools/README in the CCE repo for the car's):
  COURSEn.MDL  a trailer at the end: (id, offset from the trailer as a negative 32-bit word) pairs, then their
               count. Model blocks (the course's sectors): nverts, npolys (16 bits), offsets of vertices (0x14),
               polygons and texture table (32 bits) from the block. Vertices: x, y, z (16 bits) in 1/32 world
               unit, the arcade's world space (CCE's ground CSn_COL.BIN and the arcade's contact quads agree).
               Polygons: attribute, texture (byte offset of an 8-byte entry in the table, or the RGB555 colour of a
               flat polygon), three (attribute bit 0) or four vertex references (index x 16). Attribute bits 5-8:
               draw mode (0 flat, 2 textured distorted sprite), bits 12-13 the sprite's flips.
               Texture table: 8-byte entries (CMDPMOD, lookup table address / 8, pixel address / 8, CMDSIZE), the
               first holding the count; a polygon's texture word is the byte offset from the first real entry (the
               table + 8). VDP1 4-bit lookup-table sprites, CMDPMOD 0x1488 with colour 0 transparent.
  COURSEn.TEX  a trailer as above: the 16-colour RGB555 lookup tables, then the 4-bit pixels.

What it writes (meshpack.h, material_pack.h, visibility.h):
  - every textured sprite as a texture of its own, its 16 colours applied, scaled (nearest) to power-of-two
    sides of at least 32, twiddled RGB565 (opaque) or ARGB1555 (colour 0 transparent: the punch-through list):
    the PC's direct-colour textures in a DNT6 pack (palette index 254, code/material_pack.c), as the Saturn's
    ~200 lookup tables would not fit the PVR's palette banks
  - the geometry in world coordinates, each block's polygons split by the 128-unit scenery cell of their
    centre, a mesh and instance each, every face two-sided and lit 255 (the Saturn draws the course unlit), the
    sprite's corners as u, v
  - the visibility pack: the instances of each cell, the camera probes of course 0's (scenery.dnv in ASSET_SET)
  - with --contact CCE_REPO (the daytona-cce95 checkout): the ground, courseSLOT.dnc (contact.h). CCE's
    CSn_COL.BIN cut into quads whose planes step where they meet (the car rocks); the CCE repo's
    tools/col_generate.py rebuilds it the way the Saturn Daytona USA's ground was built (one plane a cell of the
    track's grid, triangles that meet). Its polygons become the arcade's contact quads: x, z corners clockwise,
    y from the polygon's plane (y = 4D/Ny - (Nx x + Nz z)/Ny, checked on Three Seven against the arcade's own
    to 1e-5), a triangle's third corner repeated (as the arcade's are), the surface by CCE's type (on Three
    Seven, under CCE's types 1 road, 2 roofed road, 3 grass, 4 apron, 0 wall strip lie the arcade's 1, 1, 2, 8,
    5), the heading the generator's road direction (the arcade's convention: atan2(-dx, dz) of the race's way,
    1/65536 turn), the recovery point the quad's centre at its height (as the arcade's). CCE's wall strips are
    left out: the arcade's walls are the road profiles'. The 128 x 128 grid of 16-unit cells lists the quads
    that overlap each cell.
Game-derived output: keep the asset set under build/.
"""
import argparse
import json
import math
import struct
from pathlib import Path

ALIGNED = 0x80000000
LAYERED = 0x364d4e44
BAKED, CLASSES = 1, 2
DIRECT = 254                    # material_pack.h: a direct-colour texture (PC)
HEADER_ID = 0xCE00


def trailer(d):
    n = struct.unpack_from('>I', d, len(d) - 4)[0]
    t = len(d) - 4 - 8 * n
    return {i: (t + o) - (1 << 32) for i, o in (struct.unpack_from('>II', d, t + 8 * k) for k in range(n))}


def rgb555(w):
    return ((w & 31) * 255 // 31, ((w >> 5) & 31) * 255 // 31, ((w >> 10) & 31) * 255 // 31)


def pow2(n):
    return max(32, 1 << (n - 1).bit_length())


def twiddle(x, y, w, h):
    xb, yb = w.bit_length() - 1, h.bit_length() - 1
    s = min(xb, yb)
    i = 0
    for b in range(s):
        i |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1)
    if xb > s:
        i |= (x >> s) << (2 * s)
    elif yb > s:
        i |= (y >> s) << (2 * s)
    return i


def load_course(mdl, tex):
    tt = trailer(tex)
    ids = sorted(tt)
    luts, pixels = tt[ids[0]], tt[ids[1]]
    blocks, table = [], None
    for _, o in sorted(trailer(mdl).items(), key=lambda t: t[1]):
        nv, npoly, vo, po, to = struct.unpack_from('>HHIII', mdl, o)
        if vo != 0x14:
            continue
        table = o + to
        verts = [tuple(c / 32.0 for c in struct.unpack_from('>3h', mdl, o + vo + 6 * k)) for k in range(nv)]
        polys, at = [], o + po
        for _ in range(npoly):
            a = struct.unpack_from('>H', mdl, at)[0]
            n = 3 if a & 1 else 4
            r = struct.unpack_from(f'>{2 + n}H', mdl, at)
            polys.append((r[0], r[1], [verts[v // 16] for v in r[2:]]))
            at += 2 * (2 + n)
        blocks.append(polys)
    # The table: 8-byte entries (CMDPMOD, lookup table / 8, pixels / 8, CMDSIZE), the first holding the count; a
    # polygon's texture word is the byte offset of its entry from the first real one (table + 8): a tree's crown
    # and trunk quads (11 and 4 units tall, one over the other) take the leafy tree and the dark trunk this way.
    count = struct.unpack_from('>H', mdl, table)[0]
    entries = [struct.unpack_from('>4H', mdl, table + 8 * k) for k in range(count + 1)]

    def texture(offset):
        pmod, lut, src, size = entries[offset // 8 + 1]
        w, h = ((size >> 8) & 0x3f) * 8, size & 0xff
        clut = struct.unpack_from('>16H', tex, luts + lut * 8)
        data = tex[pixels + src * 8:pixels + src * 8 + w * h // 2]
        index = []
        for b in data:
            index += [b >> 4, b & 15]
        transparent = pmod == 0x1488
        return w, h, [(None if (k == 0 and transparent) else clut[k]) for k in index], transparent
    return blocks, texture


def dctx(w, h, rgba_words, transparent):
    """rgba_words: w*h RGB555 words or None (transparent), row-major."""
    out = bytearray(w * h * 2)
    for y in range(h):
        for x in range(w):
            c = rgba_words[y * w + x]
            if c is None:
                v = 0                                                         # transparent (ARGB1555 alpha 0)
            elif transparent:
                r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31             # the Saturn's colours are BGR555
                v = 0x8000 | r << 10 | g << 5 | b
            else:
                r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
                v = r << 11 | ((g << 1) | (g >> 4)) << 5 | b                  # RGB565
            struct.pack_into('<H', out, 2 * twiddle(x, y, w, h), v)
    fmt = 0 if transparent else 1 << 27
    pvr = fmt | ((w.bit_length() - 4) << 3) | (h.bit_length() - 4)
    head = struct.pack('<4sIBBBBHHI12x', b'DcTx', 32 + len(out), 0, 0, 0, 0, w, h, pvr)
    return head + bytes(out)


CCE_STARTS = {3: ((-145.93, -117.3), 21888), 4: ((-337.0, -65.0), 0)}   # the CCE repo's col_convert.py
SURFACE = {0: 5, 1: 1, 2: 1, 3: 2, 4: 8}


def plane_height(corners, x, z):
    """The plane of a quad's first three corners at (x, z)."""
    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = corners[:3]
    ax, ay, az = x1 - x0, y1 - y0, z1 - z0
    bx, by, bz = x2 - x0, y2 - y0, z2 - z0
    nx, ny, nz = ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx
    if abs(ny) < 1e-12:
        return y0
    return y0 - (nx * (x - x0) + nz * (z - z0)) / ny


def overlaps(poly, x0, z0, x1, z1):
    """Convex polygon (x, z) against the rectangle: separating axes."""
    xs = [p[0] for p in poly]
    zs = [p[1] for p in poly]
    if max(xs) < x0 or min(xs) > x1 or max(zs) < z0 or min(zs) > z1:
        return False
    rect = [(x0, z0), (x1, z0), (x1, z1), (x0, z1)]
    n = len(poly)
    for i in range(n):
        ax, az = poly[i]
        bx, bz = poly[(i + 1) % n]
        nx, nz = az - bz, bx - ax
        a = [nx * px + nz * pz for px, pz in poly]
        b = [nx * px + nz * pz for px, pz in rect]
        if max(a) < min(b) or max(b) < min(a):
            return False
    return True


def contact_pack(cce, repo, course, slot):
    import sys
    sys.path.insert(0, str(Path(repo) / 'tools'))
    import col_generate
    start, heading = CCE_STARTS[course]
    col = (cce / f'CS{course}_COL.BIN').read_bytes()
    data, report = col_generate.generate(col, start, heading)
    load = col_generate.LOAD
    ids = set()
    lists = {}                                       # polygon -> the grid cells listing it
    for c in range(8192):                            # both grids (+0, +0x4000): they differ where a course crosses itself
        pointer = struct.unpack_from('>I', data, 4 * c)[0]
        if not pointer:
            continue                                   # a cell without ground
        at = pointer - load
        n = struct.unpack_from('>H', data, at)[0]
        members = struct.unpack_from(f'>{n}H', data, at + 2)
        ids.update(members)
        for m in members:
            lists.setdefault(m, set()).add(c)
    quads, walls, quad_of = [], [], {}
    for p in sorted(ids):
        o = col_generate.POLYS_AT + 0x34 * p
        flags = struct.unpack_from('>I', data, o)[0]
        kind = flags >> 16
        if kind not in SURFACE:
            continue
        nx, ny, nz, d = struct.unpack_from('>4i', data, o + 36)
        n = 3 if flags & 1 else 4
        if kind == 0:
            # A wall strip: vertical in CCE's file; here its footprint, a wall quad (surface 5) at the ground's height.
            xz = [tuple(v / 2**20 for v in struct.unpack_from('>ii', data, o + 4 + 8 * k)) for k in range(n)]
            area = sum(xz[k][0] * xz[(k + 1) % n][1] - xz[(k + 1) % n][0] * xz[k][1] for k in range(n))
            walls.append((xz[::-1] if area > 0 else xz, flags, p))
            continue
        if ny == 0:
            continue
        xz = [tuple(v / 2**20 for v in struct.unpack_from('>ii', data, o + 4 + 8 * k)) for k in range(n)]
        area = sum(xz[k][0] * xz[(k + 1) % n][1] - xz[(k + 1) % n][0] * xz[k][1] for k in range(n))
        if area > 0:
            xz = xz[::-1]
        y = lambda x, z: 4 * d / ny - (nx * x + nz * z) / ny
        corners = [(x, y(x, z), z) for x, z in xz]
        if n == 3:
            corners.append(corners[2])
        direction = (flags & 0xFFFC) if flags & 2 else 0
        if direction & 0x8000:
            direction |= 0xFFFF0000
        cx = sum(x for x, _ in xz) / n
        cz = sum(z for _, z in xz) / n
        quad_of[p] = len(quads)
        quads.append((corners, SURFACE[kind], direction, y(cx, cz), (cx, y(cx, cz), cz), xz))
    # The walls' corners at the height of the nearest ground quad's plane there: of the ground the wall's own grid
    # cells list. Where the course crosses itself the two grids hold one level each (Desert City's tunnel: grid 1
    # the tunnel's road and walls, grid 0 the road over it), and the nearest ground of all can be the other level's
    # (the road over the tunnel's walls then stood across the tunnel at its height).
    import numpy as np
    centres = np.array([q[4] for q in quads])
    cell_ground = {}
    for p, cells in lists.items():
        if p in quad_of:
            for c in cells:
                cell_ground.setdefault(c, []).append(quad_of[p])
    for xz, flags, p in walls:
        own = sorted({k for c in lists.get(p, ()) for k in cell_ground.get(c, ())})
        pool = np.array(own) if own else np.arange(len(centres))
        corners = []
        for x, z in xz:
            k = int(pool[np.argmin((centres[pool, 0] - x) ** 2 + (centres[pool, 2] - z) ** 2)])
            g = quads[k][0]
            corners.append((x, plane_height(g, x, z) + 0.02, z))
        if len(corners) == 3:
            corners.append(corners[2])
        direction = (flags & 0xFFFC) if flags & 2 else 0
        if direction & 0x8000:
            direction |= 0xFFFF0000
        cx = sum(c[0] for c in corners[:len(xz)]) / len(xz)
        cz = sum(c[2] for c in corners[:len(xz)]) / len(xz)
        k = int(pool[np.argmin((centres[pool, 0] - cx) ** 2 + (centres[pool, 2] - cz) ** 2)])
        quads.append((corners, 5, direction, quads[k][3], quads[k][4], xz))
    quads.sort(key=lambda q: q[1] != 1)              # road quads first: the road pack maps them (road.h)
    per_cell = [[] for _ in range(16384)]
    for i, q in enumerate(quads):
        xs = [p[0] for p in q[5]]
        zs = [p[1] for p in q[5]]
        for cz in range(max(0, int((min(zs) + 1024) // 16)), min(127, int((max(zs) + 1024) // 16)) + 1):
            for cx in range(max(0, int((min(xs) + 1024) // 16)), min(127, int((max(xs) + 1024) // 16)) + 1):
                x0, z0 = cx * 16 - 1024, cz * 16 - 1024
                if overlaps(q[5], x0, z0, x0 + 16, z0 + 16):
                    per_cell[cx | cz << 7].append(i)
    lists, starts, words = {}, [], []
    for cell in range(16384):
        members = tuple(per_cell[cell])
        if members not in lists:
            lists[members] = len(words)
            words += [len(members), *members]
        starts.append(lists[members])
    peak = max(len(m) for m in lists)
    if peak > 33 or len(words) > 65536 or len(quads) > 8192:
        raise SystemExit(f'contact pack too dense: {peak} quads in a cell, {len(words)} list words, {len(quads)} quads')
    body = bytearray(struct.pack(f'<16384H{len(words)}H', *starts, *words))
    while len(body) % 4:
        body.append(0)
    for corners, surface, direction, height, centre, _ in quads:
        body += struct.pack('<12f2I4f', *(c for v in corners for c in v), surface, direction, height, *centre)
    head = struct.pack('<8I', 0x33434e44, 32 + len(body), slot, len(words), len(quads), peak, 0, 0)
    print(f'contact: {report}; {len(quads)} quads, {len(words)} list words, at most {peak} a cell')
    return head + bytes(body), quads, per_cell


def inside(xz, x, z):
    """contact.c nm_contact_contains: clockwise x, z corners, inclusive edges."""
    n = len(xz)
    for i in range(n):
        ax, az = xz[i]
        bx, bz = xz[(i + 1) % n]
        if z * ax - x * az - z * bx + x * bz - bz * ax + bx * az < 0:
            return False
    return True


class Ground:
    """The contact quads by 16-unit cell, queried as the arcade's contact.c does (closest height wins)."""

    def __init__(self, quads, per_cell):
        self.quads, self.per_cell = quads, per_cell

    def at(self, x, z, near=None):
        cell = ((int(round(x)) + 1024) >> 4 & 127) | (((int(round(z)) + 1024) >> 4 & 127) << 7)
        best = None
        for i in self.per_cell[cell]:
            q = self.quads[i]
            if not inside(q[5], x, z):
                continue
            h = plane_height(q[0], x, z)
            key = abs(h - near) if near is not None else -h
            if best is None or key < best[0]:
                best = (key, i, h, q[1])
        return None if best is None else best[1:]          # (quad, height, surface)


def heading_of(dx, dz):
    return int(round(math.atan2(-dx, dz) * 65536 / (2 * math.pi))) & 0xffff


def s16(v):
    v &= 0xffff
    return v - 65536 if v & 0x8000 else v


def centre_line(cce, course, ground):
    """The course's centre line (road_pack): from CCE's course map CSn_BLK.BIN and the ground, every 8 units from
    the point nearest CCE's start, a closed loop of x, z; the lap's length and the sectors used."""
    blk = (cce / f'CS{course}_BLK.BIN').read_bytes()
    first = struct.unpack_from('>I', blk, 0)[0]
    sectors = []
    for i in range((struct.unpack_from('>I', blk, 4)[0] - first) // 48):
        c = [tuple(v / 65536 for v in struct.unpack_from('>ii', blk, first + 48 * i + 4 + 8 * k)) for k in range(4)]
        if sectors and (abs(c[0][0] - sectors[-1][1][0]) > 1e-3 or abs(c[0][1] - sectors[-1][1][1]) > 1e-3):
            break
        sectors.append(c)
    start, _ = CCE_STARTS[course]
    centres, prev = [], start
    for c in sectors:
        (x0, z0), (x1, z1) = c[0], c[3]
        length = math.hypot(x1 - x0, z1 - z0)
        steps = int(length / 0.25)
        runs, run = [], []
        for k in range(steps + 1):
            t = k / steps
            x, z = x0 + (x1 - x0) * t, z0 + (z1 - z0) * t
            g = ground.at(x, z)
            if g and g[2] == 1:
                run.append((x, z))
            elif run:
                runs.append(run)
                run = []
        if run:
            runs.append(run)
        if not runs:
            continue
        mids = [((r[0][0] + r[-1][0]) / 2, (r[0][1] + r[-1][1]) / 2) for r in runs if len(r) > 8]
        if not mids:
            continue
        m = min(mids, key=lambda p: math.hypot(p[0] - prev[0], p[1] - prev[1]))
        centres.append(m)
        prev = m
    # Resample the closed centre line every 8 units, starting at the point nearest CCE's start.
    k0 = min(range(len(centres)), key=lambda i: math.hypot(centres[i][0] - start[0], centres[i][1] - start[1]))
    loop = centres[k0:] + centres[:k0]
    loop.append(loop[0])
    seg = [math.hypot(loop[i + 1][0] - loop[i][0], loop[i + 1][1] - loop[i][1]) for i in range(len(loop) - 1)]
    total = sum(seg)
    count = max(16, round(total / 8))
    pts, at, i, acc = [], 0.0, 0, 0.0
    for j in range(count):
        target = j * total / count
        while acc + seg[i] < target:
            acc += seg[i]
            i += 1
        t = (target - acc) / seg[i] if seg[i] else 0
        pts.append((loop[i][0] + (loop[i + 1][0] - loop[i][0]) * t, loop[i][1] + (loop[i + 1][1] - loop[i][1]) * t))
    # Smooth a little (the sector midpoints jitter where the road widens).
    for _ in range(3):
        pts = [((pts[j - 1][0] + 2 * pts[j][0] + pts[(j + 1) % count][0]) / 4,
                (pts[j - 1][1] + 2 * pts[j][1] + pts[(j + 1) % count][1]) / 4) for j in range(count)]
    return pts, total, len(sectors)


def road_pack(cce, out, a, quads, per_cell):
    """courseSLOT.dnr (DNR7, road.h) and courseSLOT-route0..7.dnp (path.h), from CCE's course map and the ground.

    Centre line: CCE's course map CSn_BLK.BIN, a chain of sectors along the race (at +0x588: 48-byte records,
    4 bytes, then four x, z corners in 16.16; a sector's edge c0-c3 is across the course, c1-c2 the next one's),
    each cross edge met where it crosses the road (surface 1) nearest the line so far; resampled every 8 units
    (the arcade's profile spacing). Each profile: the road's edges across it, lanes 1..6 evenly between them
    (lane 1 to 6 along the profile's angle, the road heading plus a quarter turn, as the arcade's), heights
    from the ground, pitch along and bank across (signs as the arcade's profiles: checked on Advanced).
    Profile 0 is the profile nearest CCE's start position; the last record is the terminal one before it.
    Start grid, award parameters and start constants: Beginner's (course0.dnr), the grid's profiles laid out
    the same way on this course and the time per lap scaled by the lap's length."""
    ground = Ground(quads, per_cell)
    pts, total, sector_count = centre_line(cce, a.course, ground)
    count = len(pts)
    profiles, last_h = [], None
    for j in range(count):
        px, pz = pts[j]
        tx, tz = pts[(j + 1) % count][0] - pts[j - 1][0], pts[(j + 1) % count][1] - pts[j - 1][1]
        road = heading_of(tx, tz)
        across = (road + 16384) & 0xffff
        ax, az = -math.sin(across * 2 * math.pi / 65536), math.cos(across * 2 * math.pi / 65536)
        g = ground.at(px, pz, last_h)
        h = g[1] if g else (last_h or 0.0)
        edges = []
        for sign in (-1, 1):
            d, last = 0.0, (px, pz, h)
            while d < 40:
                d += 0.25
                x, z = px + sign * ax * d, pz + sign * az * d
                gg = ground.at(x, z, last[2])
                if not gg or gg[2] != 1:
                    break
                last = (x, z, gg[1])
            edges.append(last)
        (lx, lz, ly), (rx, rz, ry) = edges
        lanes = []
        for k in range(6):
            t = k / 5
            x, z = lx + (rx - lx) * t, lz + (rz - lz) * t
            gg = ground.at(x, z, h)
            lanes.append((x, gg[1] if gg else ly + (ry - ly) * t, z))
        width = math.hypot(rx - lx, rz - lz) or 1.0
        bank = int(round(math.atan2(lanes[5][1] - lanes[0][1], width) * 65536 / (2 * math.pi)))
        profiles.append([lanes, s16(across), bank, h, road])
        last_h = h
    for j in range(count):
        nx_, nz_ = pts[(j + 1) % count]
        horiz = math.hypot(nx_ - pts[j][0], nz_ - pts[j][1]) or 1.0
        profiles[j].append(int(round(math.atan2(profiles[(j + 1) % count][3] - profiles[j][3], horiz) * 65536 / (2 * math.pi))))
    # As Beginner's: the loop's last profile (the one before 0) is the terminal record P-1; positions on the
    # course wrap at P-1 (its grid goes 192 + 4 -> 1).
    P = count
    body = bytearray()
    rows = profiles
    for lanes, across, bank, h, road, pitch in rows:
        body += struct.pack('<bbh', 1, 6, 0)
        body += struct.pack('<3f4h', 0, 0, 0, 0, 0, 0, 0)
        for x, y, z in lanes:
            body += struct.pack('<3f4h', x, y, z, pitch, across, bank, 0)
        body += struct.pack('<3f4h', 0, 0, 0, 0, 0, 0, 0)
    # Map: each road quad to the first profile at least 6 units ahead of its centre along the race (Beginner's
    # quads point about 12 units ahead, past their front edge): the rolling start and the guided cars steer to the
    # profile their quad maps to, so it must stay ahead of them or they stop on it.
    import numpy as np
    centre_pts = np.array([[(r[0][2][0] + r[0][3][0]) / 2, (r[0][2][2] + r[0][3][2]) / 2] for r in profiles])
    road_quads = [q for q in quads if q[1] == 1]
    for q in road_quads:
        d2 = (centre_pts[:, 0] - q[4][0]) ** 2 + (centre_pts[:, 1] - q[4][2]) ** 2
        j = int(np.argmin(d2))
        for step in range(6):
            k = (j + step) % count
            hd = profiles[k][4] * 2 * math.pi / 65536
            ahead = (centre_pts[k][0] - q[4][0]) * -math.sin(hd) + (centre_pts[k][1] - q[4][2]) * math.cos(hd)
            if ahead >= 6.0:
                break
        body += struct.pack('<HH', k, 1)
    # Awards, grid, speeds, guided scales, start: Beginner's.
    ref = (Path(a.reference or a.assets) / 'course0.dnr').read_bytes()
    rh = struct.unpack_from('<8I', ref, 0)
    rP, rM, rC, rG = rh[3], rh[4], rh[5], rh[7]
    ao = 32 + 164 * rP + 4 * rM
    awards = bytearray(ref[ao:ao + 4224])
    scale = total / 1568.0                               # Beginner's lap, about 196 profiles of 8 units
    for d in range(4):
        for k in range(257):
            v = struct.unpack_from('<I', awards, 4 * (257 * d + k))[0]
            struct.pack_into('<I', awards, 4 * (257 * d + k), int(round(v * scale)))
    body += awards
    base = 164 * P + 4 * len(road_quads) + 4224
    while len(body) < ((base + 31) & ~31):
        body.append(0)
    # The grid: Advanced's (course2.dnr), a standing start as CCE's on its own courses (the CCE repo's
    # docs/driving_model.md: the rolling start is Three Seven's; race_scene.h rolls only slot 0): the player just
    # before the line (lap -1), the others from the line on, the same profiles here. Its speeds, guided scales and
    # start constants with it.
    ref = (Path(a.reference or a.assets) / 'course2.dnr').read_bytes()
    rh = struct.unpack_from('<8I', ref, 0)
    rP, rM, rC, rG = rh[3], rh[4], rh[5], rh[7]
    # Offsets in the reference's payload, as code/road.c lays it out.
    r_grid = (164 * rP + 4 * rM + 4224 + 2 * rC + 31) & ~31
    r_speeds = r_grid + 32 * rG
    r_guided = (r_speeds + 4 * rG + 31) & ~31
    r_start = (r_guided + 4 * rG + 31) & ~31
    for g in range(rG):
        speed, profile, previous, path_offset, lateral, lap, route, style = struct.unpack_from('<IIIIIiII', ref, 32 + r_grid + 32 * g)
        prof = profile if profile < (rP - 1) // 2 else P - (rP - profile)    # behind the line: as far behind
        body += struct.pack('<IIIIIiII', speed, prof, previous, (prof * 28) & 65535, lateral,
                            -1 if prof >= (P - 1) // 2 else 0, route, style)
    body += ref[32 + r_speeds:32 + r_speeds + 4 * rG]
    while len(body) % 32:
        body.append(0)
    body += ref[32 + r_guided:32 + r_guided + 4 * rG]
    while len(body) % 32:
        body.append(0)
    body += ref[32 + r_start:32 + r_start + 32]
    head = struct.pack('<8I', 0x37524e44, 32 + len(body), a.slot if a.name != 'world' else 0, P, len(road_quads), 0, a.laps, rG)
    write(out / f'course{a.slot if a.name != "world" else 0}.dnr', head + bytes(body))
    # Routes: eight lines across the lanes; speed eased by the curvature.
    laterals = (640, 896, 1152, 1408, 512, 768, 1024, 1280)
    curv = []
    for j in range(count):
        h0, h1 = profiles[j - 1][4], profiles[(j + 1) % count][4]
        dh = ((h1 - h0 + 32768) & 0xffff) - 32768
        curv.append(abs(dh) / 16.0)                      # heading change per unit (two profiles, 16 units)
    for r, lat in enumerate(laterals):
        rows_out = []
        for j in range(P):
            lanes, across, bank, h, road, pitch = rows[j]
            g = None
            t = (lat / 256 - 1) / 5
            x = lanes[0][0] + (lanes[5][0] - lanes[0][0]) * t
            z = lanes[0][2] + (lanes[5][2] - lanes[0][2]) * t
            g = ground.at(x, z, h)
            y = g[1] if g else h
            k = max(curv[(j + m) % count] for m in range(-6, 12))
            speed = max(0.95, min(1.45, 1.45 - k * 0.0011))
            if g and g[2] == 1:
                quad = g[0]
            else:                                        # off the road quads: the nearest road quad
                quad = min(range(len(road_quads)), key=lambda q: (road_quads[q][4][0] - x) ** 2 + (road_quads[q][4][2] - z) ** 2)
            rows_out.append(struct.pack('<4f5hH', x, y, z, speed, pitch, s16(road) // 4, bank, 0, lat, quad))
        rows_out += rows_out[:8]
        dnp = struct.pack('<8I', 0x31504e44, 32 + 28 * (P + 8), a.slot, r, P, P + 8, 0, 0) + b''.join(rows_out)
        write(out / f'course{a.slot if a.name != "world" else 0}-route{r}.dnp', dnp)
    print(f'road: {sector_count} sectors, lap {total:.0f} units, {P} profiles, {len(road_quads)} road quads, {a.laps} laps')
    return P


def face_plane(corners):
    p0, p1, p2 = corners[0], corners[1], corners[2]
    e1 = [p1[i] - p0[i] for i in range(3)]
    e2 = [p2[i] - p0[i] for i in range(3)]
    n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
    ln = math.sqrt(sum(x * x for x in n)) or 1.0
    n = [x / ln for x in n]
    return n, sum(n[i] * p0[i] for i in range(3))


def overlap_in_plane(n, a, b, eps=0.05):
    """Two convex polygons of one plane overlap by more than eps (a shared edge or corner is not an overlap)."""
    drop = max(range(3), key=lambda i: abs(n[i]))
    keep = [i for i in range(3) if i != drop]
    pa = [(c[keep[0]], c[keep[1]]) for c in a]
    pb = [(c[keep[0]], c[keep[1]]) for c in b]
    for poly in (pa, pb):
        m = len(poly)
        for i in range(m):
            (ax, ay), (bx, by) = poly[i], poly[(i + 1) % m]
            nx, ny = ay - by, bx - ax
            ln = math.hypot(nx, ny)
            if ln < 1e-9:
                continue
            nx, ny = nx / ln, ny / ln
            ra = [nx * x + ny * y for x, y in pa]
            rb = [nx * x + ny * y for x, y in pb]
            if min(max(ra), max(rb)) - max(min(ra), min(rb)) <= eps:
                return False
    return True


def write(path, data):
    """A new file (an asset set of links to another: never through the link)."""
    path.unlink(missing_ok=True)
    path.write_bytes(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cce')
    ap.add_argument('course', type=int)
    ap.add_argument('slot', type=int)
    ap.add_argument('assets', nargs='?')
    ap.add_argument('--map', metavar='JSON', help='with --contact: only the course map\'s centre line (prepare_hud.py --generated-map)')
    ap.add_argument('--profiles', type=int, default=0)
    ap.add_argument('--name')
    ap.add_argument('--contact', metavar='CCE_REPO')
    ap.add_argument('--road', action='store_true', help='with --contact: the road pack and the eight routes')
    ap.add_argument('--laps', type=int, default=6)   # Circuit Edition's National Park: 6 laps (LAP 1/6 on the Saturn)
    ap.add_argument('--reference', help="the asset set with Beginner's packs (default ASSET_SET)")
    a = ap.parse_args()
    if a.map:
        if not a.contact:
            ap.error('--map needs --contact CCE_REPO')
        _, quads, per_cell = contact_pack(Path(a.cce), a.contact, a.course, a.slot)
        pts, _, _ = centre_line(Path(a.cce), a.course, Ground(quads, per_cell))
        write(Path(a.map), json.dumps(dict(course=a.course, slot=a.slot, centre=[[round(x, 3), round(z, 3)] for x, z in pts])).encode())
        print(f'CCE course {a.course} -> slot {a.slot}: map centre line of {len(pts)} points')
        return
    if not a.assets:
        ap.error('ASSET_SET is needed')
    cce, out = Path(a.cce), Path(a.assets)
    mdl = (cce / f'COURSE{a.course}.MDL').read_bytes()
    tex = (cce / f'COURSE{a.course}.TEX').read_bytes()
    blocks, texture = load_course(mdl, tex)

    # Materials: a header per textured sprite entry and per flat colour (material_pack.h's keys).
    materials, textures, bindings = {}, [], []

    def material(attr, word):
        textured = ((attr >> 5) & 15) != 0
        key = ('t', word) if textured else ('c', word)
        if key in materials:
            return materials[key]
        if textured:
            w, h, words, transparent = texture(word)
            W, H = pow2(w), pow2(h)
            scaled = [words[(y * h // H) * w + (x * w // W)] for y in range(H) for x in range(W)]
            textures.append(dctx(W, H, scaled, transparent))
            h0 = 0x4000 | ((3 << 13) if transparent else 0) | (W.bit_length() - 6) | (H.bit_length() - 6) << 3
            header = (h0, 0, HEADER_ID | a.slot, len(textures) - 1)
            bindings.append((header, len(textures) - 1, 0, 0xffffffff, DIRECT))
            stage = 3 if transparent else 1
            info = (header, stage, W, H)
        else:
            header = (0, 0, HEADER_ID | a.slot | 0x80, word)
            r, g, b = rgb555(word)
            bindings.append((header, 0xffff, 0xff000000 | r << 16 | g << 8 | b, 0xffffffff, 255))
            info = (header, 1, 32, 32)
        materials[key] = (len(materials), info)
        return materials[key]

    # Faces into meshes by (block, cell).
    def cell_of(x, z):
        return (((int(round(x)) + 1024) >> 7) & 15) | ((((int(round(z)) + 1024) >> 7) & 15) << 4)

    # Draw layers (meshpack.h DNM6 face.layer): CCE lays decals (lines, kerbs, markings) as polygons in the plane of
    # the road under them, and the Saturn's painter's sort puts them on top: a polygon after another in its block,
    # or of a sort mode without the bias (bits 1-3: 3 is max + bias, under the others), when the two lie in one
    # plane and overlap. Here each such face goes one layer above the highest it covers.
    layer_of = {}
    for b, polys in enumerate(blocks):
        planes = []
        order = sorted(range(len(polys)), key=lambda i: (((polys[i][0] >> 1) & 7) != 3, i))
        for i in order:
            attr, word, corners = polys[i]
            n, d = face_plane(corners)
            layer = 0
            for j, (nj, dj, cj, lj) in enumerate(planes):
                if abs(n[0] * nj[0] + n[1] * nj[1] + n[2] * nj[2]) > 0.999 and \
                        all(abs(nj[0] * c[0] + nj[1] * c[1] + nj[2] * c[2] - dj) < 0.03 for c in corners) and \
                        overlap_in_plane(nj, corners, cj):
                    layer = max(layer, lj + 1)
            layer = min(layer, 7)
            planes.append((n, d, corners, layer))
            layer_of[(b, i)] = layer
    meshes = {}
    for b, polys in enumerate(blocks):
        for index, (attr, word, corners) in enumerate(polys):
            if ((attr >> 5) & 15) and not texture(word)[0]:
                continue                       # an empty texture entry (0: size 0): nothing to draw
            mid, info = material(attr, word)
            cx = sum(p[0] for p in corners) / len(corners)
            cz = sum(p[2] for p in corners) / len(corners)
            meshes.setdefault((cell_of(cx, cz), b), []).append((attr, mid, info, corners, layer_of[(b, index)]))

    vertices, faces, mesh_records, groups, refs, instances = [], [], [], [], [], []
    cells = [[0, 0] for _ in range(256)]
    for (cell, b) in sorted(meshes):
        polys = meshes[(cell, b)]
        vfirst, ffirst = len(vertices), len(faces)
        index, first_of = {}, {}
        local = []
        for attr, mid, info, corners, layer in polys:
            _, _, W, H = info
            n = len(corners)
            # The sprite's corners A(0,0) B(w,0) C(w,h) D(0,h); a triangle takes A, B, C; flips mirror them.
            uv = [(0, 0), (1, 0), (1, 1), (0, 1)][:n]
            if attr & 0x1000:
                uv = [(1 - u, v) for u, v in uv]
            if attr & 0x2000:
                uv = [(u, 1 - v) for u, v in uv]
            ids = []
            for p, (u, v) in zip(corners, uv):
                U = int(4 + u * (8 * W - 8))
                V = int(4 + v * (8 * H - 8))
                key = (p, U, V)
                if key not in index:
                    index[key] = len(local)
                    local.append(key)
                ids.append(index[key])
            p0, p1, p2 = corners[0], corners[1], corners[2]
            e1 = [p1[i] - p0[i] for i in range(3)]
            e2 = [p2[i] - p0[i] for i in range(3)]
            nx, ny, nz = e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]
            ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
            # Attribute bit 4: the Saturn culls the back (a fence's two sides are two polygons back to back): one-
            # sided here too, the others two-sided. This winding's normal (CCE's culled road faces: 7236 of 7244
            # horizontal ones point down) is the one the renderer's side test keeps the face for when seen from
            # above (checked in the race: the opposite normal hid the road).
            normal = (nx / ln, ny / ln, nz / ln) if ln > 1e-9 else (0.0, -1.0, 0.0)
            two = not (attr & 0x10)
            attribute = (1 if n == 4 else 2) | 0x100 | ((1 << 17) if two else 0)
            faces.append(struct.pack('<4HHBBI3fHBB', *(ids + [0] * (4 - n)), mid, n, 255, attribute, *normal,
                                     len(groups), layer, 255 if two else 0))
        for k, (p, U, V) in enumerate(local):
            vertices.append(struct.pack('<3fHH', *p, U, V))
            rep = first_of.setdefault(p, k)
            refs.append(struct.pack('<H', rep))
        pts = [k[0] for k in local]
        bounds = [min(p[i] for p in pts) for i in range(3)] + [max(p[i] for p in pts) for i in range(3)]
        groups.append(struct.pack('<6f4f4fIf', *bounds, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.0))
        mesh_records.append(struct.pack('<4I6f6I', vfirst, len(local), ffirst, len(polys), *bounds, 1,
                                       len(groups) - 1, 1, 0, 0, 0))
        if cells[cell][1] == 0:
            cells[cell][0] = len(instances)
        cells[cell][1] += 1
        instances.append(struct.pack('<4I12f2f3f4h2h4I', len(mesh_records) - 1, 0xCE0000 | len(instances), 0, 0,
                                     1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 640, 480, 320, 240,
                                     0, 0, 0, 0))
    # Instances are in cell order already (sorted keys), so each cell's are contiguous.
    mats = sorted(materials.values())
    material_records = b''.join(struct.pack('<4H3f3I', *info[0], 0.0, 255.0, 0.0, 0, info[1], 0)
                                for _, info in mats)

    def table(buf, data):
        while len(buf) % 32:
            buf.append(0)
        buf += data
    body = bytearray(64)
    for t in (b''.join(mesh_records), b''.join(vertices), b''.join(faces), material_records, b''.join(instances),
              b''.join(refs)):
        table(body, t)
    while len(body) % 4:
        body.append(0)
    table(body, b''.join(groups))
    while len(body) % 32:
        body.append(0)
    header = struct.pack('<8I4i4I', LAYERED | ALIGNED, len(body), len(mesh_records), len(vertices), len(faces),
                         len(mats), len(instances), len(groups), 0, 0, 0, 0, 0, 0, 8, BAKED | CLASSES)
    body[:64] = header
    stem = a.name or f'course{a.slot}'
    world_name = f'{stem}-world' if not a.name else ('world' if a.name == 'world' else f'{stem}-world')
    write(out / f'{world_name}.dnm', bytes(body))

    # Materials (DNT6 with direct-colour textures; one palette, the car pack's greyscale ramp, for the format).
    car = (Path(a.reference or a.assets) / 'car.dnt').read_bytes()
    ch = struct.unpack_from('<8I', car, 0)
    cpo = 32 + 16 * ch[2] + 16 * ch[3] + 4 * ch[3]
    cpal = cpo + ((ch[3] + 3) & ~3)
    palette, key = car[cpal:cpal + 64], car[cpal + 64 * ch[6]:cpal + 64 * ch[6] + 4]
    bindings_sorted = sorted(bindings, key=lambda b: struct.pack('<4HH', *b[0], 255))
    N, R = len(bindings_sorted), len(textures)
    meta = bytearray(32 + 16 * R)
    for b in bindings_sorted:
        meta += struct.pack('<4HHHI', *b[0], 255, b[1], b[2])
    meta += struct.pack(f'<{N}I', *(b[3] for b in bindings_sorted))
    meta += bytes(b[4] for b in bindings_sorted)
    while len(meta) % 4:
        meta.append(0)
    meta += palette + key
    while len(meta) % 32:
        meta.append(0)
    at = len(meta)
    for r, t in enumerate(textures):
        w, h = struct.unpack_from('<HH', t, 12)
        struct.pack_into('<IIHHI', meta, 32 + 16 * r, at, len(t), w, h, 0)
        at += len(t)
    dnt = meta + b''.join(textures)
    struct.pack_into('<4sIIIIIII', dnt, 0, b'DNT6', len(dnt), R, N, sum(len(t) - 32 for t in textures),
                     max(len(t) for t in textures), 1, 0)
    write(out / f'{world_name}.dnt', bytes(dnt))

    if a.contact:
        dnc, quads, per_cell = contact_pack(cce, a.contact, a.course, a.slot)
        write(out / ('course0.dnc' if a.name == 'world' else f'course{a.slot}.dnc'), dnc)
        if a.road:
            a.profiles = road_pack(cce, out, a, quads, per_cell)
    # Visibility: course 0's orders and probes, this world's cells.
    ref = (Path(a.reference or a.assets) / 'scenery.dnv').read_bytes()
    profiles = a.profiles or struct.unpack_from('<I', ref, 12)[0]
    dnv = bytearray(struct.pack('<8I', 0x31564e44, 1684, a.slot, profiles, 0, len(instances), 0, 0))
    dnv += ref[32:32 + 628]
    end = 0
    for c in range(256):                       # each cell's instances, contiguous in cell order
        dnv += struct.pack('<HH', end, cells[c][1])
        end += cells[c][1]
    vis_name = 'scenery' if a.name == 'world' else (f'{stem}' if a.name else f'course{a.slot}')
    write(out / f'{vis_name}.dnv', bytes(dnv))
    print(f'CCE course {a.course} -> slot {a.slot}: {len(mesh_records)} meshes, {len(vertices)} vertices, {len(faces)} faces, '
          f'{len(mats)} materials, {R} textures ({sum(len(t) for t in textures)} bytes), {len(instances)} instances; '
          f'visibility {profiles} profiles')


if __name__ == '__main__':
    main()
