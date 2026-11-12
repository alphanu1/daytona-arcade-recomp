#!/usr/bin/env python3
"""The Saturn version's secret horse for the PC build: the arcade's own galloping horse as the player's mount.

    python3 prepare_horse.py ASSET_SET

The arcade draws its horse on Expert only: the actor 224854 of the course's actor table, its 32 gallop frames
236840[n & 1f] (include/expert_actors.h). The asset recipe extracts it from the ROM into Expert's world pack
(course1-world.dnm mesh, course1-world.dnt texture and bindings, course1-actors.dna frames). This copies that
horse into the CAR pack, which every course loads:

  car.dnm        two meshes appended after the pack's last mesh (no instance: the race player's draw swaps its body
                 slot's mesh, render/scene/practice.c): the horse as the arcade colours it (the automatic's, the
                 Saturn's brown horse) and the same horse grey (the manual's, the Saturn's grey one: its palette
                 0605c9ec), in Expert's vertex and face order
  car.dnt        the horse's texture (base level of its mip chain, PAL4 on the car's own greyscale palette) and the
                 bindings of both colourings for every luma: the arcade's tint per luma, and for the grey horse
                 the same lightness as a neutral grey (the Saturn's grey ramp is the lighter one)
  car-horse.dnb  the 32 gallop frames as variants of the horse mesh (render/scene/damage_bodies.h format, as the
                 damaged bodies): positions, face normals and bounds of each frame

The recipe's car pack is kept as car-base.dnm / car-base.dnt and every run builds from those (car-horse.sha256
records this tool's output, so a fresh car pack from the recipe replaces them): the tool can run again. ROM-derived output: keep the asset set under build/.
"""
import hashlib
import struct
import sys
from pathlib import Path

ALIGNED = 0x80000000
HORSE_GROUP = 2                 # course1-actors.dna: groups 2..8 are the seven horses, one set of tables
HORSE_LIST = 0x236840           # the frames' object lists (program), the variants' model ids
GREY_BIT = 0x8000               # header word 3 of the grey horse's materials: a key no arcade header uses
GREY_LIGHTNESS = 1.6            # grey value = lightness of the arcade's tint at that luma times this (the Saturn's grey is light)


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def pad(buf, alignment):
    while len(buf) % alignment:
        buf.append(0)


class Pack:
    """A DNM6 pack (meshpack.h): the tables as byte strings, the layout of code/meshpack.c validate()."""

    def __init__(self, data):
        self.h = list(struct.unpack_from('<8I4i4I', data, 0))
        magic = self.h[0]
        if (magic & ~ALIGNED) != 0x364d4e44:
            raise SystemExit('not a DNM6 pack')
        self.aligned = bool(magic & ALIGNED)
        a = 32 if self.aligned else 1
        o = 64
        self.layout = []

        def take(count, stride):
            nonlocal o
            while o % a:
                o += 1
            t = data[o:o + count * stride]
            o += count * stride
            return t
        _, _, M, V, F, MAT, I, groups = self.h[:8]
        self.meshes = take(M, 64)
        self.vertices = take(V, 16)
        self.faces = take(F, 32)
        self.materials = take(MAT, 32)
        self.instances = take(I, 112)
        self.refs = take(V, 2)
        while o % 4:
            o += 1
        self.groups = take(groups, 64) if groups else b''
        strips, indices = self.h[12], self.h[13]
        if strips:
            while o % 32:
                o += 1
            self.strips = take(strips, 32)
            self.strip_indices = take(indices, 2)
        else:
            self.strips = self.strip_indices = b''
        self.lod = b''
        flags = self.h[15]
        if flags & 4:
            while o % 4:
                o += 1
            self.lod = take(flags >> 16, 32)

    def mesh(self, i):
        return struct.unpack_from('<4I6f6I', self.meshes, 64 * i)

    def serialize(self):
        a = 32 if self.aligned else 1
        out = bytearray(64)
        for name in ('meshes', 'vertices', 'faces', 'materials', 'instances', 'refs'):
            pad(out, a)
            out += getattr(self, name)
        pad(out, 4)
        if self.groups:
            pad(out, a)
            out += self.groups
        if self.strips:
            pad(out, 32)
            pad(out, a)
            out += self.strips
            pad(out, a)
            out += self.strip_indices
        if self.lod:
            pad(out, 4)
            pad(out, a)
            out += self.lod
        pad(out, 32 if self.aligned else 4)
        h = self.h[:]
        h[1] = len(out)
        h[2], h[3], h[4] = len(self.meshes) // 64, len(self.vertices) // 16, len(self.faces) // 32
        h[5], h[6] = len(self.materials) // 32, len(self.instances) // 112
        struct.pack_into('<8I4i4I', out, 0, *h)
        return bytes(out)


class Materials:
    """A DNT6 palette pack (include/material_pack.h): records, bindings, modulation, palette indices, palettes,
    keys, then the DcTx resources."""

    def __init__(self, data):
        if data[:4] != b'DNT6':
            raise SystemExit('not a DNT6 pack')
        h = struct.unpack_from('<8I', data, 0)
        self.flags = h[7]
        R, N, P = h[2], h[3], h[6]
        o = 32
        self.records = [list(struct.unpack_from('<IIHHI', data, o + 16 * i)) for i in range(R)]
        o += 16 * R
        self.bindings = [data[o + 16 * i:o + 16 * i + 16] for i in range(N)]
        o += 16 * N
        self.modulation = list(struct.unpack_from(f'<{N}I', data, o))
        o += 4 * N
        self.palette_index = list(data[o:o + N])
        o += (N + 3) & ~3
        self.palettes = data[o:o + 64 * P]
        o += 64 * P
        self.keys = data[o:o + 4 * P]
        self.blobs = [data[r[0]:r[0] + r[1]] for r in self.records]

    def serialize(self):
        order = sorted(range(len(self.bindings)), key=lambda i: self.bindings[i][:10])
        for a, b in zip(order, order[1:]):
            if self.bindings[a][:10] == self.bindings[b][:10]:
                raise SystemExit('duplicate binding')
        N, R, P = len(order), len(self.blobs), len(self.palettes) // 64
        meta = bytearray(32)
        records = bytearray(16 * R)
        meta += records
        for i in order:
            meta += self.bindings[i]
        meta += struct.pack(f'<{N}I', *(self.modulation[i] for i in order))
        meta += bytes(self.palette_index[i] for i in order)
        pad(meta, 4)
        meta += self.palettes + self.keys
        pad(meta, 32)
        offset = len(meta)
        for r, blob in enumerate(self.blobs):
            struct.pack_into('<IIHHI', meta, 32 + 16 * r, offset, len(blob), self.records[r][2], self.records[r][3], 0)
            offset += len(blob)
        out = meta + b''.join(self.blobs)
        payload = sum(len(b) - 32 for b in self.blobs)
        staging = max(len(b) for b in self.blobs)
        struct.pack_into('<8I', out, 0, u32(b'DNT6', 0), len(out), R, N, payload, staging, P, self.flags)
        return bytes(out)


def base_level(blob):
    """A DcTx PAL4 texture without its mip chain: the base level only (material_pack.c, the DNT6 layout)."""
    mode = u32(blob, 16)
    w, h = struct.unpack_from('<HH', blob, 12)
    if not mode & 0x80000000:
        return blob
    if w != h:
        raise SystemExit('mip chain of a non-square texture')
    skip, side = 2, 2
    while side < w:
        skip += side * side // 2
        side *= 2
    body = blob[32 + skip:32 + skip + w * w // 2]
    size = 32 + ((len(body) + 31) & ~31)
    head = bytearray(blob[:32])
    struct.pack_into('<I', head, 4, size)
    struct.pack_into('<I', head, 16, mode & ~0x80000000)
    return bytes(head) + body + bytes(size - 32 - len(body))


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    root = Path(sys.argv[1])
    # car.dnm/.dnt are this tool's own output when their hashes are the recorded ones; else the recipe's (fresh).
    stamp = root / 'car-horse.sha256'
    recorded = stamp.read_text().split() if stamp.exists() else []
    for name in ('car.dnm', 'car.dnt'):
        base = root / name.replace('car.', 'car-base.')
        current = (root / name).read_bytes()
        if hashlib.sha256(current).hexdigest() not in recorded or not base.exists():
            base.write_bytes(current)
    car = Pack((root / 'car-base.dnm').read_bytes())
    mats = Materials((root / 'car-base.dnt').read_bytes())
    world = Pack((root / 'course1-world.dnm').read_bytes())
    wmats = Materials((root / 'course1-world.dnt').read_bytes())
    dna = (root / 'course1-actors.dna').read_bytes()

    # The horse's group (clock_actors.h nm_clock_group_t) and its frames.
    g = 32 + 104 * HORSE_GROUP
    mesh_id = u32(dna, g)
    frames, rate, points, vcount, fcount, states, state_faces, normal_faces = struct.unpack_from('<8H', dna, g + 16)
    origin = struct.unpack_from('<3f', dna, g + 40)
    step = struct.unpack_from('<3f', dna, g + 52)
    off = struct.unpack_from('<10I', dna, g + 64)
    if rate != 1 or frames != 32 or state_faces or normal_faces != fcount:
        raise SystemExit('unexpected horse frames')
    vertex_point = struct.unpack_from(f'<{vcount}H', dna, off[0])

    m = world.mesh(mesh_id)
    if m[1] != vcount or m[3] != fcount:
        raise SystemExit('horse mesh differs from its frames')
    vbase = [list(struct.unpack_from('<3fHH', world.vertices, 16 * (m[0] + v))) for v in range(vcount)]
    faces = [list(struct.unpack_from('<4HHBBI3fHBB', world.faces, 32 * (m[2] + f))) for f in range(fcount)]

    def frame_positions(k):
        pos = []
        for v in range(vcount):
            p = vertex_point[v]
            if p == 0xffff:
                pos.append(tuple(vbase[v][:3]))
            else:
                q = struct.unpack_from('<3h', dna, off[1] + 6 * (points * k + p))
                pos.append(tuple(struct.unpack('<f', struct.pack('<f', q[a] * step[a] + origin[a]))[0] for a in range(3)))
        return pos

    def frame_normals(k):
        nf = struct.unpack_from(f'<{fcount}H', dna, off[5])
        out = [None] * fcount
        for j in range(fcount):
            q = struct.unpack_from('<3h', dna, off[6] + 8 * (fcount * k + j))
            out[nf[j]] = tuple(c / 8192.0 for c in q)
        return out

    # Materials: the world's material records of the horse's faces, then the grey copies.
    used = sorted({f[4] for f in faces})
    world_mat = {i: bytearray(world.materials[32 * i:32 * i + 32]) for i in used}
    first_material = len(car.materials) // 32
    new_materials = bytearray()
    index = {}
    for colour in (0, 1):
        for i in used:
            rec = bytearray(world_mat[i])
            struct.pack_into('<2I', rec, 24, 0, 0)        # no scenery class or super-sample depth in the car pack
            if colour:
                h3 = struct.unpack_from('<H', rec, 6)[0]
                struct.pack_into('<H', rec, 6, h3 | GREY_BIT)
            index[(colour, i)] = first_material + len(new_materials) // 32
            new_materials += rec
    car.materials += bytes(new_materials)

    # Bindings: every luma of every header, from Expert's (the arcade's tint), the grey from its lightness.
    texture = None
    resource_new = len(mats.blobs)
    existing = {b[:10] for b in mats.bindings}
    for i in used:
        header = struct.unpack_from('<4H', world_mat[i], 0)
        rows = {}
        for k, b in enumerate(wmats.bindings):
            # One luma of Expert's (the brightest) takes a blue-tinted palette; the car pack has only the grey one.
            if struct.unpack_from('<4H', b, 0) == header and wmats.palette_index[k] in (0, 255):
                rows[struct.unpack_from('<H', b, 8)[0]] = k
        if not rows:
            raise SystemExit(f'no Expert binding for horse header {header}')
        known = sorted(rows)
        for luma in range(256):
            near = min(known, key=lambda x: (abs(x - luma), x))
            k = rows[near]
            b = wmats.bindings[k]
            res, color = struct.unpack_from('<HI', b, 10)
            if res != 0xffff:
                if texture is None:
                    texture = res
                elif texture != res:
                    raise SystemExit('horse uses more than one texture')
                argb = wmats.modulation[k]
                if wmats.palette_index[k] != 0:
                    raise SystemExit('horse texture is not on palette 0')
            else:
                argb = color
            scale = luma / near if near else 0.0
            r, gr, bl = [min(255, round(((argb >> s) & 255) * scale)) for s in (16, 8, 0)]
            for colour in (0, 1):
                if colour:
                    y = 0.299 * r + 0.587 * gr + 0.114 * bl
                    # The near-black parts (hooves, eyes) stay as they are.
                    if header[3] != 0x500:
                        r = gr = bl = min(255, round(y * GREY_LIGHTNESS))
                h3 = header[3] | (GREY_BIT if colour else 0)
                key = struct.pack('<4HH', header[0], header[1], header[2], h3, luma)
                if key in existing:
                    if res != 0xffff:
                        raise SystemExit(f'car pack already binds {key.hex()}')
                    continue   # a solid colour the cars use too (0x500, near black): the car pack's binding
                value = 0xff000000 | r << 16 | gr << 8 | bl
                if res != 0xffff:
                    mats.bindings.append(key + struct.pack('<HI', resource_new, color))
                    mats.modulation.append(value)
                    mats.palette_index.append(0)
                else:
                    mats.bindings.append(key + struct.pack('<HI', 0xffff, value))
                    mats.modulation.append(0xffffffff)
                    mats.palette_index.append(255)
    if texture is None:
        raise SystemExit('horse has no texture')
    if wmats.palettes[0:64] != mats.palettes[0:64]:
        raise SystemExit('car palette 0 is not the greyscale ramp the horse uses')
    blob = base_level(wmats.blobs[texture])
    mats.blobs.append(blob)
    mats.records.append([0, len(blob), wmats.records[texture][2], wmats.records[texture][3], 0])

    # Meshes: brown, then grey; vertices of frame 0; faces with car-pack fields.
    layers = car.h[14]
    frame0 = frame_positions(0)
    normals0 = frame_normals(0)
    strip_cursor = len(car.strips) // 32
    horse_meshes = []
    for colour in (0, 1):
        vfirst, ffirst = len(car.vertices) // 16, len(car.faces) // 32
        reps = {}
        for v in range(vcount):
            car.vertices += struct.pack('<3fHH', *frame0[v], vbase[v][3], vbase[v][4])
            key = ('p', vertex_point[v]) if vertex_point[v] != 0xffff else ('s', frame0[v])
            rep = reps.setdefault(key, v)
            if frame0[rep] != frame0[v]:
                rep = v
            car.refs += struct.pack('<H', rep)
        for f, face in enumerate(faces):
            idx, mat, count, _, attribute = face[0:4], face[4], face[5], face[6], face[7]
            layer = min(face[12], layers - 1)
            car.faces += struct.pack('<4HHBBI3fHBB', *idx, index[(colour, mat)], count, 0, attribute,
                                     *normals0[f], 0, layer, 0)
        bounds = [min(p[a] for p in frame0) for a in range(3)] + [max(p[a] for p in frame0) for a in range(3)]
        car.meshes += struct.pack('<4I6f6I', vfirst, vcount, ffirst, fcount, *bounds, m[10], 0, 0, strip_cursor, 0, 0)
        horse_meshes.append(len(car.meshes) // 64 - 1)

    # The gallop frames (damage_bodies.h DNB1): variant k is frame k; variant 0 is the "clean" one.
    stride = (32 + 12 * (vcount + fcount) + 31) & ~31
    classes = 0
    body = bytearray()
    for k in range(frames):
        pos, nor = frame_positions(k), frame_normals(k)
        bounds = [min(p[a] for p in pos) for a in range(3)] + [max(p[a] for p in pos) for a in range(3)]
        rec = bytearray(struct.pack('<2I6f', HORSE_LIST + 4 * k, k, *bounds))
        for p in pos:
            rec += struct.pack('<3f', *p)
        for n in nor:
            rec += struct.pack('<3f', *n)
        rec += bytes(stride - len(rec))
        body += rec
        classes = max(classes, len({(faces[f][4], nor[f]) for f in range(fcount)}))
    head = struct.pack('<4s7I', b'DNB1', 32 + len(body), HORSE_LIST, vcount, fcount, frames, stride, classes)
    (root / 'car-horse.dnb').write_bytes(head + body)
    car_out, mats_out = car.serialize(), mats.serialize()
    (root / 'car.dnm').write_bytes(car_out)
    (root / 'car.dnt').write_bytes(mats_out)
    stamp.write_text(hashlib.sha256(car_out).hexdigest() + '\n' + hashlib.sha256(mats_out).hexdigest() + '\n')
    print(f'horse: meshes {horse_meshes[0]} (brown) and {horse_meshes[1]} (grey), {vcount} vertices, {fcount} faces, '
          f'{frames} gallop frames, {len(used)} materials, texture {blob[12] | blob[13] << 8}x{blob[14] | blob[15] << 8}, '
          f'up to {classes} light classes')


if __name__ == '__main__':
    main()
