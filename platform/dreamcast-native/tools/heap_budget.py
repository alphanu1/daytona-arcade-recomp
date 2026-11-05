#!/usr/bin/env python3
"""Heap of the Dreamcast build, computed on the host from the asset packs.

The console has one heap between the end of the image and 64 KiB below 16 MiB (include/heap.h). This tool
replays what a scene load allocates and frees, in the order and with the sizes of the code, through a
model of KOS's allocator (dlmalloc 2.7: best fit, coalescing, a top block grown by sbrk in pages and
trimmed above 256 KiB, memalign as an over-sized malloc with the ends given back), and prints the heap at
its high-water mark and when the scene runs. It exits 1 when less than the stated budget stays free.

usage: heap_budget.py ASSET_DIR [--stage N] [--profile] [--late-capture BYTES] [--no-streams] [--layout tree|head]
                      [--image-end HEX | --elf FILE] [--peak-free BYTES] [--steady-free BYTES] [--table] [--hardware-check]

  --stage N       scenery stage of the build (make NATIVE_SCENERY_STAGE), default 6
  --no-streams    a build with NATIVE_SQ_STREAM=0 (no prebuilt world streams)
  --profile       NATIVE_SAMPLE_PROFILE=1: the 1.92 MB sample buffer
  --late-capture BYTES  NATIVE_DIAG_LATE_CAPTURE: its picture pool (slots x picture bytes), allocated before any asset
  --layout head   the allocation order of commit c318e6b (before the header/UV cache and the 32-byte
                  emission records went); `tree` is the code as it is now
  --image-end     address of `_end` (default 0x8c112000: 1.1 MiB of image; --elf reads it with sh-elf-nm)
  --peak-free     least memory that must never have been taken by the heap, default 256 KiB (see "Budgets")
  --steady-free   least free memory (inside the heap and above it) while the scene runs, default 512 KiB
  --frame-compare NATIVE_FRAME_COMPARE: its 921,600-byte capture taken during the race, added to both budgets
  --session COURSES  a whole play session (boot into the attract and its loop over the three courses, then select, race,
                  finish and podium music, name entry, the attract on the raced course and the next one, for each course
                  slot of the list), replayed through the allocator: the budgets apply to the least never taken over the
                  session and the least free while racing
  --hardware-check  the model against the console logs of 13 October 2026 (layout head): needs the
                  asset sets under build/ and prints model and measurement side by side

What is modelled: every allocation of code/ that can exceed a few kilobytes, with its owner. SYSTEM_BYTES
stands for everything KOS, enDjinn and newlib hold before the game loads its first file (thread stacks,
the PVR's and enDjinn's tables); it is calibrated on one hardware log and checked on the others.
Not modelled: allocations below a kilobyte, and the order inside newlib's stdio.

Budgets (18 October 2026; until then 2 MiB never taken and 3 MiB free, a margin set after the out-of-memory failure of
13 October, not derived). What the game allocates once a race has loaded, all modelled by Session: the vertex marks
of its first frames (16,416), enDjinn's render-list segments (5,440), the finish and podium music (an 8,224-byte
staging buffer, then the flow, at most 34,967 for these pieces, each freed after its upload): at most about 60 KB,
the largest single block 34,967. A scene switch frees the old scene's blocks before the new one loads;
what stays across it (the AICAflow and enDjinn tables, the course packs of the same course) is in the replay, and so is
the fragmentation. Not modelled: allocations below a kilobyte (KOS file handles, newlib's FILE table, maple hot-plug:
tens of KB at most) and the model's own error (the load model is within 2.6 KiB of the 13 October console logs,
--hardware-check; the session's music and scene-switch parts are not yet checked on the console:
make NATIVE_HEAP_TRACE=1 and the `Native heap` lines). So: never taken over the session at least 256 KiB (twice the
race-time needs and any of their blocks above the top whatever the holes, plus 64 KiB for model error and small
allocations), free while racing at least 512 KiB. Diagnostic builds: the sample profile buffer and the late
capture pool are taken before any asset and modelled (same budgets); NATIVE_FRAME_COMPARE takes a 921,600-byte
capture during the race (--frame-compare adds it to both).

Keep the two functions `load_head` and `load_tree` in step with the code: nm_practice_run (code/practice.c),
nm_practice_render_load (code/render/scene/practice_assets.c), nm_materials_load / nm_world_materials_upload
(code/render/materials/materials.c), nm_render_init / nm_render_world_attach
(code/render/models/model_renderer.c) and the tables of include/render/models/model_submission.h. The
console prints `Native heap ...` lines (nm_heap_report) with the same quantities: compare them with this
tool's after a hardware run. make NATIVE_HEAP_TRACE=1 prints each named allocation.
"""
import os
import re
import shutil
import argparse, struct, subprocess, sys
from pathlib import Path

RAM_TOP = 0x8d000000
KERNEL_STACK = 64 * 1024            # THD_KERNEL_STACK_SIZE: mm_sbrk refuses the last 64 KiB
HEAP_LIMIT = RAM_TOP - KERNEL_STACK
PAGE = 4096
TRIM = 256 * 1024                   # DEFAULT_TRIM_THRESHOLD of KOS's malloc.c
# Calibrated on build/dc-native-fastpath.MdhHxq/hw/console-L8.log (in use 9,311,804 at tick 600).
SYSTEM_BYTES = 96440
SAMPLE_BYTES = 480000 * 4           # NM_SAMPLE_CAPACITY words
FILE_BUFFER = 64 * 1024             # setvbuf(file, NULL, _IOFBF, 64 KiB) of the pack loaders
SKY_TEXTURE = 67616                 # NM_SKY_TEXTURE_BYTES
TEXTURE_INFO = 48                   # enj_texture_info_t


class Heap:
    """dlmalloc 2.7 as KOS configures it, to the precision that matters for blocks of kilobytes."""

    # Owners whose blocks a library allocates (KOS threads, AICAflow, newlib's stdio buffers): plain malloc, never
    # raised to the game's alignment.
    LIBRARY = {'voices worker thread', 'voices worker stack', 'AICAflow asset slots'}

    def __init__(self, start, min_alignment=0):
        self.start = self.top_base = (start + 7) & ~7
        # The game's allocations are memalign(32) at least (include/nm_mem.h nm_alloc, heap.h nm_heap_alloc): the
        # `tree` layout and the session raise every request of the game's to it, the `head` layout (the code of
        # 13 October, checked against the console logs of then) keeps the alignment each call passed.
        self.min_alignment = min_alignment
        self.brk = self.top_base     # end of the top block: what sbrk has handed out
        self.used = {}               # address -> (size, owner)
        self.free = {}               # address -> size
        self.high = self.brk
        self.failed = None
        self.events = []

    @staticmethod
    def chunk(request):
        return max(16, (request + 4 + 7) & ~7)

    def _take(self, need):
        best = None
        for address, size in self.free.items():
            if size >= need and (best is None or size < self.free[best] or (size == self.free[best] and address < best)):
                best = address
        if best is not None:
            size = self.free.pop(best)
            if size - need >= 16: self.free[best + need] = size - need
            else: need = size
            return best, need
        if self.brk - self.top_base < need + 16:
            grow = need + 16 - (self.brk - self.top_base)
            grow = (grow + PAGE - 1) & ~(PAGE - 1)
            if self.brk + grow >= HEAP_LIMIT:
                self.failed = (self.brk, grow)
                return None, 0
            self.brk += grow
            self.high = max(self.high, self.brk)
        address = self.top_base
        self.top_base += need
        return address, need

    def _give(self, address, size):
        # Coalesce with free neighbours, then with the top block.
        for other, other_size in list(self.free.items()):
            if other + other_size == address: address, size = other, size + other_size; del self.free[other]
        if address + size in self.free: size += self.free.pop(address + size)
        if address + size == self.top_base:
            self.top_base = address
            top = self.brk - self.top_base
            if top >= TRIM:
                extra = ((top - 16 + PAGE - 1) // PAGE - 1) * PAGE
                if extra > 0: self.brk -= extra
        else:
            self.free[address] = size

    def alloc(self, owner, request, alignment=0, kind='kept'):
        if request <= 0: request = 1
        if kind != 'system' and owner not in self.LIBRARY and not owner.endswith(' file buffer'):
            alignment = max(alignment, self.min_alignment)
        need = self.chunk(request)
        if alignment <= 8:
            address, size = self._take(need)
            if address is None: return self._refuse(owner, request)
        else:
            # memalign: malloc(chunk size + alignment + MINSIZE), i.e. 56 bytes more than the block for
            # alignment 32; the user pointer is 8 bytes into the chunk; the ends go back to the heap.
            address, size = self._take(self.chunk(need + alignment + 16))
            if address is None: return self._refuse(owner, request)
            aligned = address
            if (address + 8) % alignment:
                aligned = ((address + 8 + alignment - 1) & ~(alignment - 1)) - 8
                if aligned - address < 16: aligned += alignment
                self._give(address, aligned - address)
                size -= aligned - address
            if size - need > 16:
                self._give(aligned + need, size - need)
                size = need
            address = aligned
        self.used[address] = (size, owner, request, kind)
        self.events.append(('alloc', owner, request, address, self.brk))
        return address

    def _refuse(self, owner, request):
        self.failed = (owner, request) + self.failed
        raise MemoryError(owner)

    def release(self, owner):
        for address, (size, name, request, kind) in list(self.used.items()):
            if name == owner:
                del self.used[address]
                self._give(address, size)
                self.events.append(('free', owner, request, address, self.brk))
                return
        raise KeyError(owner)

    def in_use(self): return sum(size for size, *_ in self.used.values())
    def free_inside(self): return sum(self.free.values()) + (self.brk - self.top_base)


def sh_tool(name):
    """sh-elf-NAME: $KOS_NAME (as sample_profile_report.py), the KOS default path, then PATH."""
    for candidate in (os.environ.get('KOS_'+name.upper()), f'/opt/toolchains/dc/sh-elf/bin/sh-elf-{name}',
                      shutil.which(f'sh-elf-{name}')):
        if candidate and Path(candidate).exists():
            return candidate
    raise SystemExit(f'sh-elf-{name} not found; set KOS_{name.upper()}')


def u32s(data, offset, count): return struct.unpack_from('<%dI' % count, data, offset)


class MeshPack:
    """What the renderer's tables are sized by (include/meshpack.h, code/meshpack.c validate)."""

    def __init__(self, path, stage=None):
        data = Path(path).read_bytes()
        (magic, self.size, meshes, vertices, faces, materials, instances, groups) = u32s(data, 0, 8)
        padding = u32s(data, 48, 4)
        alignment = 32 if magic & 0x80000000 else 1
        offset = 64
        def table(count, stride):
            nonlocal offset
            offset = (offset + alignment - 1) & ~(alignment - 1)
            at = offset; offset += count * stride
            return at
        mesh_at = table(meshes, 64); table(vertices, 16); face_at = table(faces, 32)
        material_at = table(materials, 32); instance_at = table(instances, 112)
        self.faces, self.materials, self.instances, self.meshes = faces, materials, instances, meshes
        self.material_headers = [struct.unpack_from('<4H', data, material_at + 32 * m) for m in range(materials)]
        self.strip_indices = padding[1]
        self.strips = padding[0]
        flags = padding[3] if (magic & 0x7fffffff) == 0x364d4e44 else 0
        self.lod_groups = flags >> 16 if flags & 4 else 0
        mesh = [u32s(data, mesh_at + 64 * m, 4) for m in range(meshes)]   # vertex_first, vertex_count, face_first, face_count
        live = [m[3] for m in mesh]
        if stage is not None and flags & 2:   # nm_scenery_stage_select: faces of the materials up to the stage
            keep = [(u32s(data, material_at + 32 * m + 24, 1)[0] & 0xff) <= stage for m in range(materials)]
            live = [sum(keep[struct.unpack_from('<H', data, face_at + 32 * (m[2] + f) + 8)[0]] for f in range(m[3])) for m in mesh]
        # World objects super-sampled by instance (model_submission.h, sq_ss_prepare): a live mesh whose first face's
        # material carries a switch depth (material word padding[1]).
        self.supersampled_meshes = sum(1 for at, m in zip(range(meshes), mesh) if live[at] and
                                       u32s(data, material_at + 32 * struct.unpack_from(
                                           '<H', data, face_at + 32 * m[2] + 8)[0] + 28, 1)[0])
        self.largest_vertices = max(m[1] for m in mesh)
        self.largest_faces = max(live) if live else 0
        # Prebuilt world streams (model_submission.h, sq_stream_prepare): corners of the live faces, the largest
        # per-mesh slot (face groups plus level-of-detail groups) and the largest mesh in vertices or faces.
        self.groups = groups
        self.stream_corners = 0
        self.stream_mesh_corners = 1   # the most corners of one mesh (sq_stream_prepare's position scratch)
        keep_material = None
        if stage is not None and flags & 2:
            keep_material = [(u32s(data, material_at + 32 * m + 24, 1)[0] & 0xff) <= stage for m in range(materials)]
        for m in mesh:
            here = 0
            for f in range(m[3]):
                at = face_at + 32 * (m[2] + f)
                if keep_material is None or keep_material[struct.unpack_from('<H', data, at + 8)[0]]:
                    here += data[at + 10]
            self.stream_corners += here; self.stream_mesh_corners = max(self.stream_mesh_corners, here)
        padding_of = [u32s(data, mesh_at + 64 * m + 44, 5) for m in range(meshes)]
        self.stream_slot = max([1] + [p[1] + ((p[4] >> 16) if self.lod_groups else 0) for p in padding_of])
        self.stream_largest = max([1, self.largest_vertices] + live)
        # Prebuilt car streams (model_submission.h, sq_car_stream_prepare): the distinct positions each mesh's faces name
        # (position representatives, the table after the instances; a pack without it: the vertices themselves) and the
        # most light classes of one mesh (faces of one supplied normal and material).
        refs_at = table(vertices, 2)
        refs = struct.unpack_from('<%dH' % vertices, data, refs_at) if refs_at + 2 * vertices <= len(data) else None
        self.car_positions = 0; self.car_classes = 1
        for vf, vc, ff, fc in mesh:
            seen, classes = set(), set()
            for f in range(fc):
                at = face_at + 32 * (ff + f)
                indices = struct.unpack_from('<4H', data, at)
                for v in range(min(data[at + 10], 4)): seen.add(refs[vf + indices[v]] if refs else indices[v])
                classes.add((data[at + 8:at + 10], data[at + 16:at + 28]))
            self.car_positions += len(seen); self.car_classes = max(self.car_classes, len(classes))
        # Translucent effects (model_submission.h, sq_translucent_prepare): meshes whose every face is drawn and of the
        # translucent class (textured, texel 15 transparent, screen checker: header word 0 bits 13-15); their points
        # are SQ_TR_INSTANCES_MAX (16) times the largest such mesh.
        self.translucent_vertices = self.translucent_faces = 0
        for vf, vc, ff, fc in mesh:
            ok = fc > 0
            for f in range(fc):
                at = face_at + 32 * (ff + f)
                header = struct.unpack_from('<H', data, material_at + 32 * struct.unpack_from('<H', data, at + 8)[0])[0]
                attribute = u32s(data, at + 12, 1)[0]
                ok = ok and (header & 0xe000) == 0xe000 and attribute & 0x300 != 0
            if ok: self.translucent_vertices = max(self.translucent_vertices, vc); self.translucent_faces += fc if ok else 0
        self.queue_points = self.queue_faces = 0
        self.instance_meshes = []
        for i in range(instances):
            m = u32s(data, instance_at + 112 * i, 1)[0]
            self.instance_meshes.append(m)
            self.queue_points += mesh[m][1]; self.queue_faces += live[m]
        self.mesh_points = [m[1] for m in mesh]; self.mesh_faces = live
        # Live specular faces (meshpack.h NM_PACK_LIVE_SPECULAR, model_submission.h sq_live_prepare): mode 1, specular
        # control and scale non-zero, ambient below 255.
        self.live_faces, live_materials = 0, set()
        # The meshes of the frame-counter animations and placed actors (instances of kind 15, NM_OBJECT_CLOCK) are not
        # live specular ones (model_submission.h sq_live_prepare leaves them to the clock's light).
        clock_meshes = set()
        for i in range(instances):
            word = u32s(data, instance_at + 112 * i + 96, 1)[0]
            if word & 0x80000000 and (word >> 12) & 15 == 15: clock_meshes.add(u32s(data, instance_at + 112 * i, 1)[0])
        if flags & 16:
            for at, m in enumerate(mesh):
                if not u32s(data, mesh_at + 64 * at + 40, 1)[0] & 1 or at in clock_meshes: continue
                for f in range(m[3]):
                    material = struct.unpack_from('<H', data, face_at + 32 * (m[2] + f) + 8)[0]
                    ambient, scale, control = struct.unpack_from('<2fI', data, material_at + 32 * material + 12)
                    if control and scale and ambient < 255: self.live_faces += 1; live_materials.add(material)
        self.live_materials = len(live_materials)

    def frame_bound(self, visibility):
        """The world's point cache and face queues sized to one frame (practice_assets.c practice_world_bound,
        nm_render_world_capacity): the largest 5x5 neighbourhood of the visibility pack's cells plus every world
        object (the instances after the cells')."""
        data = Path(visibility).read_bytes()
        cell_instances = u32s(data, 0, 8)[5]
        cells = [struct.unpack_from('<2H', data, 32 + 628 + 4 * c) for c in range(256)]
        points, faces = [0] * 256, [0] * 256
        for c, (first, count) in enumerate(cells):
            for i in range(first, first + count):
                points[c] += self.mesh_points[self.instance_meshes[i]]; faces[c] += self.mesh_faces[self.instance_meshes[i]]
        objects = self.instance_meshes[cell_instances:]
        best_points = best_faces = 0
        for z in range(16):
            for x in range(16):
                near = [xx + 16 * zz for zz in range(z - 2, z + 3) for xx in range(x - 2, x + 3) if 0 <= xx < 16 and 0 <= zz < 16]
                best_points = max(best_points, sum(points[c] for c in near))
                best_faces = max(best_faces, sum(faces[c] for c in near))
        self.queue_points = min(self.queue_points, best_points + sum(self.mesh_points[m] for m in objects))
        self.queue_faces = min(self.queue_faces, best_faces + sum(self.mesh_faces[m] for m in objects))


class MaterialPack:
    """DNT6 (include/material_pack.h, code/material_pack.c open_pack)."""

    def __init__(self, path):
        data = Path(path).read_bytes()
        assert data[:4] == b'DNT6', path
        size, self.resources, self.count, self.payload, self.staging, palettes, _ = u32s(data, 4, 7)
        self.metadata = (self.resources + self.count) * 16 + self.count * 4 + ((self.count + 3) & ~3) + palettes * 68
        self.backing = size - ((32 + self.metadata + 31) & ~31)
        self.palettes = palettes
        # Emission classes: bindings that differ only in luma and colour share one. An upper bound from the
        # binding records (header words, texture, crop, palette); the renderer merges equal compiled headers.
        bindings = 32 + 16 * self.resources
        modulation = bindings + 16 * self.count
        index = modulation + 4 * self.count
        classes, headers = set(), set()
        self.lumas = {}   # header -> [(luma, binding index)] in binding order
        for i in range(self.count):
            h0, h1, h2, h3, luma, resource, color = struct.unpack_from('<6HI', data, bindings + 16 * i)
            self.lumas.setdefault((h0, h1, h2, h3), []).append((luma, i))
            solid = resource == 65535
            classes.add((h0, h1, h2, h3, resource, None if solid else color, data[index + i]))
            headers.add((h0 & 0x6300, resource, data[index + i]))
        self.classes, self.headers = len(classes), len(headers)


def luma_lookup_bytes(car, car_dnt):
    """The car's material/luma table (model_renderer.c, model_submission.h sq_luma_runs_build): 4 bytes a material
    when every material's bindings are one run of consecutive lumas up to 255 with consecutive indices, else the
    dense 512 bytes a material."""
    for header in car.material_headers:
        run = car_dnt.lumas.get(header)
        if not run: continue
        lo, first = run[0]
        if [l for l, _ in run] != list(range(lo, 256)) or [i for _, i in run] != list(range(first, first + 256 - lo)):
            return car.materials * 512
    return car.materials * 4


def keyset_scratch(count):
    """Scratch of sq_keyset_index for `count` distinct keys: (keys bytes, slots bytes) at their last size."""
    capacity, slots = 512, 1024
    while capacity < count: capacity *= 2
    while 2 * count > slots: slots *= 2
    return capacity * 32, slots * 2


# The course of the modelled race (--course; include/course_assets.h): its file names (the owners keep course 0's
# names: world.dnm stands for courseN-world.dnm).
COURSE = 0
# Courses whose world takes the whole prebuilt world stream tables (include/course_assets.h NM_COURSE_WORLD_STREAMS,
# make NATIVE_WORLD_STREAM_COURSES; --world-stream-courses): a bit per slot, Expert's off. The others take the record
# cache of WORLD_STREAM_POOL bytes (model_submission.h NM_SQ_STREAM_POOL, make NATIVE_WORLD_STREAM_POOL; --stream-pool).
WORLD_STREAM_COURSES = 5
WORLD_STREAM_POOL = 448 * 1024
def course_file(kind):
    if kind == 'visibility': return 'course%u.dnv' % COURSE if COURSE else 'scenery.dnv'
    if kind == 'actors.dna': return 'course%u-actors.dna' % COURSE
    if kind in ('world.dnm', 'world.dnt', 'sky.dns'): return 'course%u-%s' % (COURSE, kind) if COURSE else kind
    return kind.replace('course0', 'course%u' % COURSE)


def physics(heap, assets):
    for name in ('drive.dnd', 'course0.dnc', 'course0.dnr', *('course0-route%u.dnp' % i for i in range(8)), 'visibility'):
        heap.alloc(course_file(name), (assets / course_file(name)).stat().st_size - 32)


def with_file(heap, name):
    heap.alloc(name + ' file buffer', FILE_BUFFER, kind='scratch')


def load_common_start(heap, assets, car, car_dnt):
    """car.dnm, car.dnt and the SH4 self-checks, up to nm_render_init."""
    with_file(heap, 'car.dnm'); heap.alloc('car.dnm (mesh pack)', car.size, 32); heap.release('car.dnm file buffer')
    # The player's damaged bodies (render/scene/damage_bodies.h), kept for the scene: variants of the clean body's mesh.
    damage = assets / 'car-damage.dnb'
    if damage.is_file(): heap.alloc('car damage bodies', damage.stat().st_size, 32)
    hoods = assets / 'car-hoods.dnb'   # the hood view's damaged hoods: variants of the clean hood's mesh
    if hoods.is_file(): heap.alloc('car hood variants', hoods.stat().st_size, 32)
    with_file(heap, 'car.dnt'); heap.alloc('car.dnt bindings, colours, palettes', car_dnt.metadata)
    heap.alloc('car texture upload scratch', car_dnt.backing, 32, kind='scratch')
    heap.alloc('car texture descriptors', car_dnt.resources * TEXTURE_INFO)
    heap.release('car.dnt file buffer'); heap.release('car texture upload scratch')
    heap.alloc('car FTRV check scratch', car.largest_vertices * 32, kind='scratch'); heap.release('car FTRV check scratch')


def face_tables(heap, source, pack, lit):
    name = 'world' if source else 'car'
    if pack.strips: heap.alloc(name + ' strip triangles', pack.faces * 2)
    heap.alloc(name + ' emission faces', pack.faces * 32, 32)
    if pack.strips: heap.alloc(name + ' strip corners', pack.strip_indices * 8, 32)


def facing(heap, source, pack, aligned=False):
    """aligned: the tree's 32-byte aligned light class tables (model_submission.h, 15 October 2026)."""
    name = 'world' if source else 'car'
    if not source:
        heap.alloc('car light classes', pack.faces * 2, 32 if aligned else 0)
        heap.alloc('car light class cache', 4096, 32 if aligned else 0)
    heap.alloc(name + ' face planes', pack.faces * 8, 32)
    if not source: heap.alloc('car lit face records', pack.faces * 32, 32)


def queues(heap, source, pack):
    name = 'world' if source else 'car'
    heap.alloc(name + ' batches', pack.instances * 64, 32)
    heap.alloc(name + ' point cache', pack.queue_points * 32, 32)
    heap.alloc(name + ' face queues', pack.queue_faces * 4, 32)


def clock_frames(assets):
    """The course's frame-counter animations (include/clock_actors.h, courseN-actors.dna): (file bytes, states, the
    corners of the groups whose u, v change, at most four a face, the faces of the PLACED groups, the materials of
    those faces: model_renderer.c's light rows), or None without the file."""
    path = Path(assets) / course_file('actors.dna')
    if not path.is_file(): return None
    data = path.read_bytes()
    states = corners = placed = 0
    placed_meshes = []
    for i in range(struct.unpack_from('<I', data, 8)[0]):
        counts = struct.unpack_from('<12H', data, 32 + 104 * i + 16)   # frames, rate, points, vertices, faces, states, ...
        states += counts[5]
        if counts[8]: corners += 4 * counts[4]
        if struct.unpack_from('<H', data, 32 + 104 * i + 42)[0] & 1:   # group flags: PLACED
            placed += counts[4]; placed_meshes.append(struct.unpack_from('<I', data, 32 + 104 * i)[0])
    rows = 0
    world = Path(assets) / course_file('world.dnm')
    if placed_meshes and world.is_file():
        pack = world.read_bytes()
        (magic, _, meshes, vertices, faces) = u32s(pack, 0, 5)
        alignment = 32 if magic & 0x80000000 else 1
        mesh_at = 64
        face_at = mesh_at + 64 * meshes
        face_at = (face_at + alignment - 1) & ~(alignment - 1)
        face_at = (face_at + 16 * vertices + alignment - 1) & ~(alignment - 1)
        materials = set()
        for m in placed_meshes:
            _, _, first, count = u32s(pack, mesh_at + 64 * m, 4)
            materials.update(struct.unpack_from('<H', pack, face_at + 32 * (first + f) + 8)[0] for f in range(count))
        rows = len(materials)
    return len(data), states, corners, placed, rows


def world_files(heap, assets, world, world_dnt):
    with_file(heap, 'world.dnm'); heap.alloc('world.dnm (mesh pack)', world.size, 32); heap.release('world.dnm file buffer')
    heap.alloc('world instances', world.instances * 112, 32)
    heap.alloc('world FTRV check scratch', world.largest_vertices * 32, kind='scratch'); heap.release('world FTRV check scratch')
    heap.alloc('sky upload scratch', SKY_TEXTURE, 32, kind='scratch'); heap.release('sky upload scratch')
    clock = clock_frames(assets)   # practice_assets.c practice_clock_load, kept with the world pack
    if clock: heap.alloc('world clock frames', clock[0], 32)
    with_file(heap, 'world.dnt'); heap.alloc('world.dnt bindings, colours, palettes', world_dnt.metadata)
    heap.alloc('world texture descriptors', world_dnt.resources * TEXTURE_INFO)


def world_classes(heap, world, world_dnt, aligned=False):
    heap.alloc('world material/luma lookup', world.materials * 512, 32 if aligned else 0)
    heap.alloc('world surface and point classes', 2 * world_dnt.count)
    heap.alloc('world baked bindings', world.faces * 4, 32)
    heap.alloc('world texture upload scratch', world_dnt.staging, 32, kind='scratch')
    heap.release('world.dnt file buffer'); heap.release('world texture upload scratch')


def lod(heap, world, aligned=False):
    if world.lod_groups:
        heap.alloc('world level-of-detail groups', world.lod_groups * 32, 32)
        if aligned:   # the per-frame state 32-byte aligned on its own, the inspection view's face levels as before
            heap.alloc('world level-of-detail state', world.lod_groups, 32)
            heap.alloc('world level-of-detail face levels', world.faces)
        else: heap.alloc('world level-of-detail state', world.lod_groups + world.faces)


def world_streams(heap, world, cache=False):
    """sq_stream_prepare (model_submission.h): the prebuilt world streams, after the world's emission tables. The whole
    tables (each mesh's block: 96 bytes a face record, 16 a strip vertex, 2 a position and 2 a face rank, to 32 bytes;
    the positions bounded by the corners, the strip vertices by the pack's), or the record cache (cache=True: its pool
    of WORLD_STREAM_POOL bytes and the builder's tables)."""
    meshes = max(world.meshes, 1)
    heap.alloc('world stream list index', (2 * (world.groups + 3 * world.lod_groups) + 1) * 4, 32)
    heap.alloc('world stream meshes', meshes * 32, 32)
    heap.alloc('world stream mesh flags', meshes)
    heap.alloc('world stream mesh plans', meshes * 16)
    heap.alloc('world stream batch state', world.instances * (32 + ((world.stream_slot + 3) & ~3)), 32)
    scratch = 'world stream builder scratch' if cache else 'world stream scratch'
    heap.alloc(scratch, 2 * world.stream_largest + 2 + 2 * (2 * world.stream_largest + 1), kind='kept' if cache else 'scratch')
    heap.alloc('world stream position scratch', world.stream_mesh_corners * 2, kind='scratch')
    heap.release('world stream position scratch')
    if cache:
        heap.alloc('world stream record pool', WORLD_STREAM_POOL, 32)
        heap.alloc('world stream walked frames', meshes * 4)
        heap.alloc('world stream block offsets', meshes * 4)
        heap.alloc('world stream resident meshes', meshes * 2)
        return
    records = world.faces * 96 + (world.strip_indices * 16 if world.strips else 0) + (world.stream_corners + world.faces) * 2
    heap.alloc('world stream records', records + 31 * world.meshes, 32)
    heap.release(scratch)


def car_streams(heap, car):
    """sq_car_stream_prepare (model_submission.h): the prebuilt car streams, after the cars' emission tables (nm_render_init)."""
    heap.alloc('car stream scratch', max(car.largest_vertices, 1), kind='scratch')
    heap.alloc('car stream meshes', car.meshes * 8, 32)
    heap.alloc('car stream positions', (car.car_positions + 2) * 2, 32)
    heap.alloc('car stream batch state', car.instances * 16, 32)
    heap.alloc('car stream class entries', 2 * car.car_classes * 16, 32)
    heap.release('car stream scratch')


def presentation(heap, assets, hud_scratch=33 * 1024):
    for name in ('finish.dnm', 'result.dnm'):
        heap.alloc(name, (assets / name).stat().st_size, 32)
    heap.alloc('HUD upload scratch', hud_scratch, 32, kind='scratch'); heap.release('HUD upload scratch')


VOICE_STREAM = False  # NATIVE_SFX_VOICE_STREAM=1 (the ELF's name has -voices): main reads it from --elf


def sound_effects(heap, assets):
    """nm_sfx_init (code/audio/sfx.c), from nm_sound_init before any asset: the .dsx while its flows upload, the
    directory (12 bytes a flow), the engine table and AICAflow's asset slots (80 bytes each, the array doubling from
    16; music and bank included). Nothing without ASSET_DIR/sfx/race.dsx."""
    dsx = Path(assets) / 'sfx' / 'race.dsx'
    if not dsx.is_file(): return
    data = dsx.read_bytes()
    flows = struct.unpack_from('<I', data, 8)[0]
    heap.alloc('sound effects .dsx (while the flows upload)', len(data), kind='load')
    heap.alloc('sound effects directory', 12 * flows)
    if struct.unpack_from('<I', data, 16)[0]: heap.alloc('sound effects engine table', (8 * 128 * 3 + 2 * 16 * 3) * 2)
    slots = 16
    while slots < flows + 3: slots *= 2
    heap.alloc('AICAflow asset slots', 80 * slots)
    heap.release('sound effects .dsx (while the flows upload)')
    pak = Path(assets) / 'sfx' / 'voices.pak'
    if VOICE_STREAM and pak.is_file():
        # voice_init (NM_SFX_VOICE_STREAM): the pack's directory (read, then kept as entries), four slots of the
        # largest phrase (its bank to a 32-byte end, then its flow) and the worker thread (8 KiB stack, its kthread_t).
        head = pak.read_bytes()[:12 + 20 * 256]
        count = struct.unpack_from('<I', head, 8)[0]
        entries = [struct.unpack_from('<5I', head, 12 + 20 * i) for i in range(count)]
        need = 4 * max((((bank + 31) & ~31) + flow + 31) & ~31 for _, _, bank, _, flow in entries)  # VOICE_SLOTS
        heap.alloc('voices directory (read)', 20 * count, kind='load')
        heap.alloc('voices directory', 20 * count)
        heap.release('voices directory (read)')
        heap.alloc('voices slots (four of the largest phrase)', need, 32)
        heap.alloc('voices worker thread', 512); heap.alloc('voices worker stack', 8192)


def load_head(heap, assets, car, car_dnt, world, world_dnt, profile):
    """Commit c318e6b."""
    heap.alloc('KOS, enDjinn, newlib (calibrated)', SYSTEM_BYTES, kind='system')
    sound_effects(heap, assets)
    physics(heap, assets)
    load_common_start(heap, assets, car, car_dnt)
    heap.alloc('face group scratch', max(car.largest_faces, world.largest_faces))
    heap.alloc('car header/UV cache', car_dnt.count * 64, 32, kind='load')
    heap.alloc('car material/luma lookup', car.materials * 512)
    face_tables(heap, 0, car, True)
    heap.alloc('car emission records (first)', car_dnt.count * 32, 32, kind='load')
    facing(heap, 0, car); queues(heap, 0, car)
    world_files(heap, assets, world, world_dnt)
    heap.alloc('world header/UV cache', world_dnt.count * 64, 32)
    world_classes(heap, world, world_dnt)
    face_tables(heap, 1, world, False)
    heap.alloc('world emission records (first)', world_dnt.count * 32, 32, kind='load')
    lod(heap, world); facing(heap, 1, world); queues(heap, 1, world)
    heap.alloc('header sort scratch', (car_dnt.count + world_dnt.count) * 8, kind='scratch')
    heap.alloc('car shared-header map', car_dnt.count * 4, 32)
    heap.alloc('world shared-header map', world_dnt.count * 4, 32)
    heap.release('header sort scratch')
    heap.release('car emission records (first)')
    heap.alloc('car emission records', car_dnt.count * 32, 32)      # memalign does not fit the hole it left
    heap.release('world emission records (first)')
    heap.alloc('world emission records', world_dnt.count * 32, 32)
    heap.alloc('header slot scratch', (car_dnt.count + world_dnt.count) * 2, kind='scratch')
    heap.alloc('header pool', 750 * 32, 32)
    heap.release('header slot scratch')
    heap.release('car header/UV cache')
    presentation(heap, assets)
    if profile: heap.alloc('sample profile buffer', SAMPLE_BYTES, 32)


def emission_tables(heap, source, dnt):
    name = 'world' if source else 'car'
    heap.alloc(name + ' emission bindings', dnt.count * 8, 32)
    keys, slots = keyset_scratch(dnt.headers); class_keys, class_slots = keyset_scratch(dnt.classes)
    scratch = [('header keys', keys), ('header slots', slots), ('class keys', class_keys), ('class slots', class_slots),
               ('resolved headers', dnt.headers * 4)]
    for label, size in scratch: heap.alloc('%s emission scratch: %s' % (name, label), size, kind='scratch')
    heap.alloc(name + ' header pool', dnt.headers * 32, 32)
    heap.alloc(name + ' emission classes', dnt.classes * 32, 32)
    for label, size in scratch: heap.release('%s emission scratch: %s' % (name, label))


SMOKE_TIGHT = 0   # pieces a face with make NATIVE_SMOKE_TIGHT=1 (main: from the ELF's name), else 0


def load_tree(heap, assets, car, car_dnt, world, world_dnt, profile, streams=True, car_stream=True, late_capture=0):
    """The code as it is: no header/UV cache, bindings of 8 bytes and emission classes, profile buffer first;
    the prebuilt world streams unless the build has NATIVE_SQ_STREAM=0 (streams=False), the prebuilt car streams unless
    it has NATIVE_SQ_CAR_STREAM=0 (car_stream=False) or NATIVE_SQ_STREAM=0 (which turns both off)."""
    heap.alloc('KOS, enDjinn, newlib (calibrated)', SYSTEM_BYTES, kind='system')
    sound_effects(heap, assets)
    if profile: heap.alloc('sample profile buffer', SAMPLE_BYTES, 32)
    if late_capture: heap.alloc('late capture pool', late_capture, 32)
    physics(heap, assets)
    render_load(heap, assets, car, car_dnt, world, world_dnt, streams, car_stream)


def render_load(heap, assets, car, car_dnt, world, world_dnt, streams=True, car_stream=True):
    """nm_practice_render_load (and the race's HUD, presentation): from car.dnm to the result labels."""
    load_common_start(heap, assets, car, car_dnt)
    heap.alloc('face group scratch', car.largest_faces, 32)   # nm_render_init: the car's largest mesh
    heap.alloc('car material/luma lookup', luma_lookup_bytes(car, car_dnt), 32)
    face_tables(heap, 0, car, True); facing(heap, 0, car, True); queues(heap, 0, car)
    if car.translucent_vertices and SMOKE_TIGHT:
        # make NATIVE_SMOKE_TIGHT=1 (model_submission.h, sq_tr_tight_prepare; from the ELF's name, --elf): bases per mesh,
        # an entry per translucent face, NATIVE_SMOKE_PIECES pieces of 16 bytes a face; texture masks and a face grid
        # (9 x 4 KiB) while they are made.
        heap.alloc('smoke tight bases', 2 * car.meshes, 32)
        heap.alloc('smoke tight faces', 4 * car.translucent_faces, 32)
        heap.alloc('smoke tight pieces', 16 * SMOKE_TIGHT * car.translucent_faces, 32)
        heap.alloc('smoke tight masks', 9 * 4096, kind='scratch'); heap.release('smoke tight masks')
    if car.translucent_vertices: heap.alloc('translucent effect points', 16 * car.translucent_vertices * 32, 32)
    emission_tables(heap, 0, car_dnt)
    if streams and car_stream: car_streams(heap, car)
    world_files(heap, assets, world, world_dnt)
    if world.largest_faces > car.largest_faces:   # nm_render_world_attach: a larger table, the car's freed after it
        heap.alloc('face group scratch', world.largest_faces, 32); heap.release('face group scratch')
    world_classes(heap, world, world_dnt, True)
    clock = clock_frames(assets)   # model_renderer.c clock_attach_bindings: each state's bindings, the u, v corners
    if clock:
        heap.alloc('world clock bindings', 4 * clock[1], 32)
        heap.alloc('world clock u, v corners', 4 * (2 * clock[2] + 1), 32)
        if clock[3]:   # clock_attach_bindings: the PLACED groups' faces and light rows (the row map is freed again)
            heap.alloc('world clock light faces', 2 * clock[3], 32)
            heap.alloc('world clock light rows', 512 * clock[4], 32)
    face_tables(heap, 1, world, False); lod(heap, world, True); facing(heap, 1, world); queues(heap, 1, world)
    emission_tables(heap, 1, world_dnt)
    if world.supersampled_meshes: heap.alloc('world super-sample state', world.meshes, 32)
    if world.live_faces:   # sq_live_prepare (NM_PACK_LIVE_SPECULAR): faces, classes (at most one a face), rows
        heap.alloc('world live specular faces', world.live_faces * 12, 32)
        heap.alloc('world live specular classes', world.live_faces * 44, 32)
        heap.alloc('world live specular rows', world.live_materials * 512, 32)
    if streams and WORLD_STREAM_COURSES >> COURSE & 1: world_streams(heap, world)
    elif streams and WORLD_STREAM_POOL: world_streams(heap, world, cache=True)
    heap.release('world material/luma lookup')  # nm_render_world_attach: load only (the walk takes the baked bindings)
    presentation(heap, assets, 41 * 1024)  # hud_render.c: the largest sheet and the 8 KiB marks of the tight-table check


# ---------------------------------------------------------------------------------------------------- a whole session
# What runs after a scene has loaded: the music loads of code/audio/sound.c (spread over the idle waits: an 8,224-byte
# staging buffer while the .afb streams, then the whole .afx read into a malloc and freed once uploaded: nothing of a
# recording stays on the SH4), the renderer's vertex marks taken at a
# scene's first frames (sq_marks, at most SQ_MARK_VERTICES + 32), enDjinn's render-list segments (544 bytes per 64
# registrations, never freed), the name entry (name.dnm and its 64 KiB file buffer), the select screens, the VMU
# save writer (a thread with a 16 KiB stack and vmufs's buffers, running while the attract loads) and the scene
# switches of code/practice.c (nm_practice_render_release keeps the CPU packs, practice_course_load replaces them
# and the course packs). Allocations below a kilobyte stay out, as in the load.
MUSIC_STAGING = 8192 + 32     # sound.c LOAD_CHUNK_MAX + AFX_UPLOAD_ALIGN
FRAME_COMPARE = 640 * 480 * 3   # NATIVE_FRAME_COMPARE: vid_screen_shot_data's capture (code/practice.c)
STDIO_BUFFER = 1024           # newlib BUFSIZ: an fopen'd file's buffer at its first read
SQ_MARKS = 16384 + 32         # model_submission.h sq_marks: (vertices + 35) & ~31 at most
ENJ_SEGMENTS = 5 * 2 * 544    # enj_render.c: two segments of each of the five lists, never freed
VMU_WRITER = 16384 + 512 + 6656 + 512 + 2560   # game_save.c thread stack and kthread_t; vmufs.c directory, FAT, file
CPU_PACKS = ('car.dnm (mesh pack)', 'car damage bodies', 'car hood variants', 'world.dnm (mesh pack)', 'world instances',
             'world clock frames')
# include/audio/music_select.h by course (501a94 = the course without a VR button) and music_map.h's tracks.
RACE_TRACK, FINISH_TRACK, PODIUM_TRACK = (3, 5, 4), (7, 9, 8), (11, 13, 12)
ADVERTISE_TRACK, GAME_START_TRACK, NAME_TRACK = 1, 2, 17


def music(heap, assets, track):
    """sound.c load_step: the bank streamed through the staging buffer (files unbuffered), then the flow read whole and
    uploaded; both freed. The ADPCM copy (music/adpcm/) has the same flow."""
    flow = Path(assets) / 'music' / ('t%02u.afx' % track)
    if not flow.is_file(): return
    heap.alloc('music staging buffer', MUSIC_STAGING, 32, kind='scratch')
    heap.release('music staging buffer')
    heap.alloc('music flow (while it uploads)', flow.stat().st_size, kind='scratch')
    heap.release('music flow (while it uploads)')


class Session:
    """A play session through the model: boot into the attract (course 0) and its loop (2, 1, 0), then for each course of COURSES the
    select, that race (load, its first frames, the finish and podium music), the results, the name entry and the
    attract again. Records the heap's state at every point that matters."""

    def __init__(self, assets, stage, image_end, streams=True, car_stream=True, profile=False, late_capture=0):
        self.assets, self.stage, self.streams, self.car_stream = Path(assets), stage, streams, car_stream
        self.heap = Heap(image_end, 32)
        self.course = None
        self.points = []   # (label, steady free, never taken)
        self.packs = {}
        self.render_owners = []
        h = self.heap
        h.alloc('KOS, enDjinn, newlib (calibrated)', SYSTEM_BYTES, kind='system')
        sound_effects(h, self.assets)
        if profile: h.alloc('sample profile buffer', SAMPLE_BYTES, 32)
        if late_capture: h.alloc('late capture pool', late_capture, 32)
        h.alloc('drive.dnd file buffer', FILE_BUFFER, kind='scratch')
        h.alloc('drive.dnd', (self.assets / 'drive.dnd').stat().st_size - 32)
        h.release('drive.dnd file buffer')

    def note(self, label):
        h = self.heap
        self.points.append((label, h.free_inside() + HEAP_LIMIT - h.brk, HEAP_LIMIT - h.high))

    def course_packs(self, course):
        """practice_course_load: the CPU packs of the scene and the old course's packs go, the new course's come."""
        global COURSE
        if self.course == course: return
        h = self.heap
        self.render_unload()
        if self.course is not None:
            for name in ('course0.dnc', 'course0.dnr', *('course0-route%u.dnp' % i for i in range(8)), 'visibility'):
                h.release('course pack ' + name)
        COURSE = course
        h.alloc('course0.dnc file buffer', FILE_BUFFER, kind='scratch')
        for name in ('course0.dnc', 'course0.dnr', *('course0-route%u.dnp' % i for i in range(8)), 'visibility'):
            h.alloc('course pack ' + name, (self.assets / course_file(name)).stat().st_size - 32)
            if name == 'course0.dnc': h.release('course0.dnc file buffer')
        self.course = course

    def render_unload(self):
        for owner in CPU_PACKS:
            if any(o == owner for _, o, *_ in self.heap.used.values()): self.heap.release(owner)

    def render_release(self):
        """nm_practice_render_release: everything of the render load but the CPU packs."""
        for owner in self.render_owners:
            if owner not in CPU_PACKS: self.heap.release(owner)
        self.render_owners = []
        if any(o == 'world vertex marks' for _, o, *_ in self.heap.used.values()): self.heap.release('world vertex marks')

    def render_load(self):
        global COURSE
        COURSE = self.course
        key = self.course
        if key not in self.packs:
            car = MeshPack(self.assets / 'car.dnm'); world = MeshPack(self.assets / course_file('world.dnm'), max(self.stage - 1, 0))
            world.frame_bound(self.assets / course_file('visibility'))
            for name in ('car-damage.dnb', 'car-hoods.dnb'):
                damage = self.assets / name
                if damage.is_file(): car.car_classes = max(car.car_classes, struct.unpack_from('<8I', damage.read_bytes())[7])
            self.packs[key] = (car, MaterialPack(self.assets / 'car.dnt'), world, MaterialPack(self.assets / course_file('world.dnt')))
        car, car_dnt, world, world_dnt = self.packs[key]
        self.render_unload()
        before = set(a for a in self.heap.used)
        render_load(self.heap, self.assets, car, car_dnt, world, world_dnt, self.streams, self.car_stream)
        self.render_owners = [o for a, (_, o, *_) in self.heap.used.items() if a not in before]

    def first_frames(self):
        if not any(o == 'enDjinn render-list segments' for _, o, *_ in self.heap.used.values()):
            self.heap.alloc('enDjinn render-list segments', ENJ_SEGMENTS, 32)
        self.heap.alloc('world vertex marks', SQ_MARKS, 32)

    def attract(self, course, after_game=True):
        """Phase 2 and the cinematic of COURSE (include/attract.h): after a game the raced course (501460 keeps it),
        at once; in the unattended loop the next course, loaded in the title's frames (code/scene_loader.c: the same
        calls in the same order, the title holds no heap). The advertisement is resident in the loop."""
        h = self.heap
        if after_game: h.alloc('VMU save writer', VMU_WRITER, 32)   # started at the name entry's end, runs meanwhile
        if after_game: music(h, self.assets, ADVERTISE_TRACK)
        self.course_packs(course)
        self.render_load()
        self.first_frames()
        if after_game: h.release('VMU save writer')
        self.note('course %u attract running' % course)

    def attract_loop(self, course):
        """The unattended loop after the cinematic of COURSE: the title (nm_practice_render_release before it), then
        the next course's cinematic (attract.h nm_attract_next_course: 0 -> 2 -> 1 -> 0)."""
        self.render_release()
        self.attract({0: 2, 2: 1, 1: 0}[course], after_game=False)

    def select(self):
        h = self.heap
        self.render_release()
        h.alloc('select.dsl', 4612, 32)
        h.alloc('select texture (while it uploads)', 32800, 32, kind='scratch'); h.release('select texture (while it uploads)')
        h.release('select.dsl')
        music(h, self.assets, GAME_START_TRACK)
        self.note('select running')

    def race(self, course):
        h = self.heap
        self.course_packs(course)
        self.render_load()
        music(h, self.assets, RACE_TRACK[course])
        self.note('course %u race loaded' % course)
        self.first_frames()
        self.note('course %u racing' % course)
        music(h, self.assets, FINISH_TRACK[course]); music(h, self.assets, PODIUM_TRACK[course])
        self.note('course %u podium' % course)
        # phase 29: the renderer goes, the name entry comes
        self.render_release()
        h.alloc('name.dnm file buffer', FILE_BUFFER, kind='scratch'); h.alloc('name.dnm', 117656, 32); h.release('name.dnm file buffer')
        h.alloc('name label texture (while it uploads)', 8224, 32, kind='scratch'); h.release('name label texture (while it uploads)')
        music(h, self.assets, NAME_TRACK)
        self.note('course %u name entry' % course)
        h.release('name.dnm')


def session(assets, stage, image_end, courses, streams=True, car_stream=True, profile=False, late_capture=0):
    s = Session(assets, stage, image_end, streams, car_stream, profile, late_capture)
    try:
        s.course_packs(0); s.render_load(); s.first_frames(); s.note('boot: attract running')
        for course in (0, 2, 1): s.attract_loop(course)    # the unattended loop: Advanced, Expert, Beginner
        for course in courses:
            s.select(); s.race(course); s.attract(course)
            s.attract_loop(course)                          # and on from the raced course
    except MemoryError:
        pass
    return s


def run(assets, stage, profile, layout, image_end, streams=True, car_stream=True, late_capture=0):
    assets = Path(assets)
    car = MeshPack(assets / 'car.dnm'); world = MeshPack(assets / course_file('world.dnm'), max(stage - 1, 0))
    if layout == 'tree': world.frame_bound(assets / course_file('visibility'))   # since 17 October (courses)
    for name in ('car-damage.dnb', 'car-hoods.dnb'):
        damage = assets / name
        if damage.is_file():   # nm_render_car_light_classes_reserve: the light class cache holds a variant's classes too
            car.car_classes = max(car.car_classes, struct.unpack_from('<8I', damage.read_bytes())[7])
    car_dnt = MaterialPack(assets / 'car.dnt'); world_dnt = MaterialPack(assets / course_file('world.dnt'))
    heap = Heap(image_end, 0 if layout == 'head' else 32)
    try:
        if layout == 'head': load_head(heap, assets, car, car_dnt, world, world_dnt, profile)
        else: load_tree(heap, assets, car, car_dnt, world, world_dnt, profile, streams, car_stream, late_capture)
    except MemoryError:
        pass
    return heap


def report(heap, table):
    lines = []
    if table:
        rows = sorted(heap.used.items())
        lines.append('address   bytes      kind     owner')
        at = heap.start
        for address, (size, owner, request, kind) in rows:
            hole = sum(s for a, s in heap.free.items() if at <= a < address)
            if hole >= 16384: lines.append('          %-10u free     (hole)' % hole)
            if request >= 16384 or kind == 'system': lines.append('%08x  %-10u %-8s %s' % (address, request, kind, owner))
            at = address
        tail = sum(s for a, s in heap.free.items() if a > at)
        if tail >= 16384: lines.append('          %-10u free     (hole)' % tail)
        small = sum(size for size, owner, request, kind in heap.used.values() if request < 16384 and kind != 'system')
        lines.append('          %-10u          allocations below 16 KiB and block headers' % small)
    return lines


def summary(heap, label):
    limit = HEAP_LIMIT
    print('%s: image end=%08x heap limit=%08x capacity=%u' % (label, heap.start, limit, limit - heap.start))
    if heap.failed:
        owner, request, top, grow = heap.failed
        print('  OUT OF MEMORY: %s needs %u bytes; heap top %08x, sbrk request %u reaches %08x (limit %08x, %u over)' % (
            owner, request, top, grow, top + grow, limit, top + grow - limit))
    print('  high-water=%08x: %u taken at the peak, %u never taken' % (heap.high, heap.high - heap.start, limit - heap.high))
    print('  %s: top=%08x in use=%u free=%u (inside the heap %u, never taken %u)' % (
        'at the failure' if heap.failed else 'scene running', heap.brk, heap.in_use(),
        heap.free_inside() + limit - heap.brk, heap.free_inside(), limit - heap.brk))


def hardware_check():
    """Layout head against the console logs of 13 October 2026 (arena, in use and free at tick 600)."""
    root = Path(__file__).resolve().parents[3] / 'build'
    cases = [   # name, assets, stage, image end, arena, in use, free inside
        ('nomip stage 5', 'dc-native-road-filter.H1hcIW/assets-nomip', 5, 0x8c10e5cc, 14830132, 8953252, 5876880),
        ('nomip stage 3', 'dc-native-road-filter.H1hcIW/assets-nomip', 3, 0x8c10e58c, 14821940, 8949948, 5871992),
        ('L8 stage 5', 'dc-native-lod.70M0gR/assets-L8', 5, 0x8c10e5cc, 15178292, 9311804, 5866488),
        ('L6 stage 5', 'dc-native-lod.70M0gR/assets-L6', 5, 0x8c10e5cc, 15575604, 9710436, 5865168),
        ('th2 stage 5', 'dc-native-opaque-cutouts.OokCA8/assets-th2', 5, 0x8c10e5cc, 15370804, 9507572, 5863232),
        ('tl1 stage 5', 'dc-native-emit.JHtXeB/assets-tl1', 5, 0x8c10e5cc, 15526452, 9651396, 5875056),
    ]
    print('layout head, model against hardware (bytes; arena = taken by the heap, never returned):')
    worst = 0
    for name, assets, stage, image_end, arena, used, free in cases:
        if not (root / assets).is_dir(): print('  %-14s asset set missing' % name); continue
        heap = run(root / assets, stage, False, 'head', image_end)
        model = (heap.brk - heap.start, heap.in_use(), heap.free_inside())
        print('  %-14s arena model %u hardware %u (%+d)   in use model %u hardware %u (%+d)   free inside model %u hardware %u (%+d)' % (
            name, model[0], arena, model[0] - arena, model[1], used, model[1] - used, model[2], free, model[2] - free))
        worst = max(worst, abs(model[0] - arena))
    objects = root / 'dc-native-world-objects.ZINSuG/assets-objects'
    if objects.is_dir():
        heap = run(objects, 6, False, 'head', 0x8c11060c)
        if heap.failed:
            owner, request, top, grow = heap.failed
            print('  objects stage 6 model: %s refused, heap top %08x, sbrk request %u; hardware: "Requested sbrk_base 8d02d000, was 8ce74000, diff 1806336"' % (
                owner, top, grow))
            worst = max(worst, abs(top - 0x8ce74000))
        else:
            print('  objects stage 6 model: the load fits (hardware: out of memory at heap top 8ce74000)'); worst = 1 << 30
    print('  largest difference of the heap top: %u bytes' % worst)
    return worst


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('assets', nargs='?'); parser.add_argument('--stage', type=int, default=6)
    parser.add_argument('--profile', action='store_true'); parser.add_argument('--layout', choices=('tree', 'head'), default='tree')
    parser.add_argument('--late-capture', type=int, default=0, help='NATIVE_DIAG_LATE_CAPTURE: bytes of the picture pool')
    parser.add_argument('--image-end', type=lambda v: int(v, 16), default=0x8c112000); parser.add_argument('--elf')
    parser.add_argument('--peak-free', type=int, default=256 * 1024); parser.add_argument('--steady-free', type=int, default=512 * 1024)
    parser.add_argument('--frame-compare', action='store_true', help='NATIVE_FRAME_COMPARE: its race-time capture on top of both budgets')
    parser.add_argument('--table', action='store_true'); parser.add_argument('--hardware-check', action='store_true')
    parser.add_argument('--course', type=int, default=0, choices=(0, 1, 2),
                        help='the race course slot (include/course_assets.h): its packs, world and sky by name')
    parser.add_argument('--no-streams', action='store_true', help='a NATIVE_SQ_STREAM=0 build: no prebuilt world or car streams')
    parser.add_argument('--no-car-streams', action='store_true', help='a NATIVE_SQ_CAR_STREAM=0 build: no prebuilt car streams')
    parser.add_argument('--world-stream-courses', type=int, default=5, choices=range(8),
                        help='make NATIVE_WORLD_STREAM_COURSES: a bit per course whose world takes the prebuilt streams (default 5)')
    parser.add_argument('--stream-pool', type=int, default=448 * 1024,
                        help='make NATIVE_WORLD_STREAM_POOL: bytes of the world stream record cache of the other courses (0: none)')
    parser.add_argument('--session', metavar='COURSES',
                        help='a whole play session (Session): boot into the attract, then select, race, podium, name entry and '
                             'attract for each course slot of the comma-separated list')
    arguments = parser.parse_args()
    if arguments.hardware_check:
        return 1 if hardware_check() > 128 * 1024 else 0
    if not arguments.assets: parser.error('ASSET_DIR')
    globals()['COURSE'] = arguments.course
    globals()['WORLD_STREAM_COURSES'] = arguments.world_stream_courses
    globals()['WORLD_STREAM_POOL'] = arguments.stream_pool & ~31
    image_end = arguments.image_end
    if arguments.elf:
        nm = subprocess.run([sh_tool('nm'), arguments.elf], capture_output=True, text=True, check=True).stdout
        image_end = next(int(line.split()[0], 16) for line in nm.splitlines() if line.endswith(' _end'))
        globals()['VOICE_STREAM'] = '-voices-' in Path(arguments.elf).name or Path(arguments.elf).name.endswith('-voices.elf')
        tight = re.search(r'-tight([123]?)-', Path(arguments.elf).name)
        if tight: globals()['SMOKE_TIGHT'] = int(tight.group(1) or 2)
    if arguments.late_capture and arguments.layout == 'head': parser.error('--late-capture models the tree layout only')
    if arguments.frame_compare:
        arguments.peak_free += FRAME_COMPARE; arguments.steady_free += FRAME_COMPARE
    if arguments.session:
        courses = [int(c) for c in arguments.session.split(',')]
        if any(c not in (0, 1, 2) for c in courses): parser.error('--session takes course slots 0, 1, 2')
        s = session(arguments.assets, arguments.stage, image_end, courses, not arguments.no_streams, not arguments.no_car_streams,
                    arguments.profile, arguments.late_capture)
        heap = s.heap
        print('session: boot into the attract, then for each of the courses %s: select, race, finish and podium music, '
              'name entry, attract%s%s' % (','.join(map(str, courses)), ', no prebuilt world or car streams' if arguments.no_streams else '',
                                           ', sample profile' if arguments.profile else ''))
        for label, steady, never in s.points:
            print('  %-28s free %9u (never taken %u)' % (label, steady, never))
        if heap.failed:
            owner, request, top, grow = heap.failed
            print('FAIL: out of memory in the session: %s needs %u bytes (heap top %08x)' % (owner, request, top)); return 1
        racing = [steady for label, steady, _ in s.points if label.endswith('racing')]
        peak_free = HEAP_LIMIT - heap.high
        ok = peak_free >= arguments.peak_free and min(racing) >= arguments.steady_free
        print('%s: %u never taken over the session (budget %u), least free while racing %u (budget %u)' % (
            'PASS' if ok else 'FAIL', peak_free, arguments.peak_free, min(racing), arguments.steady_free))
        return 0 if ok else 1
    heap = run(arguments.assets, arguments.stage, arguments.profile, arguments.layout, image_end, not arguments.no_streams,
               not arguments.no_car_streams, arguments.late_capture)
    summary(heap, 'stage %u%s, %s%s%s%s%s' % (arguments.stage, ', course %u' % arguments.course if arguments.course else '', 'commit c318e6b' if arguments.layout == 'head' else 'working tree',
                                          ', sample profile' if arguments.profile else '',
                                          ', late capture pool %u bytes' % arguments.late_capture if arguments.late_capture else '',
                                          ', no prebuilt world or car streams' if arguments.no_streams else '',
                                          ', no prebuilt car streams' if arguments.no_car_streams and not arguments.no_streams else ''))
    for line in report(heap, arguments.table): print(line)
    if heap.failed: print('FAIL: the load runs out of memory'); return 1
    peak_free = HEAP_LIMIT - heap.high; steady_free = heap.free_inside() + HEAP_LIMIT - heap.brk
    ok = peak_free >= arguments.peak_free and steady_free >= arguments.steady_free
    print('%s: %u never taken at the peak (budget %u), %u free while the scene runs (budget %u)' % (
        'PASS' if ok else 'FAIL', peak_free, arguments.peak_free, steady_free, arguments.steady_free))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
