"""The console heap model (tools/heap_budget.py): its allocator, its pack arithmetic and its verdict.

ROM-free: the packs are synthetic (counts only). With the asset sets of the hardware runs of 13 October
2026 under build/ it also replays those runs and requires the model within 16 KiB of each measured heap.
usage: test_heap_budget.py [ASSET_DIR STAGE]    (with arguments: that set must also pass the default budget)
"""
import importlib.util, struct, subprocess, sys, tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
tool = root / 'tools/heap_budget.py'
spec = importlib.util.spec_from_file_location('heap_budget', tool)
budget = importlib.util.module_from_spec(spec); spec.loader.exec_module(budget)

# The allocator: what made the stage 6 build fail on the console.
heap = budget.Heap(0x8c110000)
# (Tables of 32-byte records between two others, as in the renderer: 24 spare bytes on either side.)
heap.alloc('before', 1024, 32)
first = heap.alloc('table', 1808960, 32)
heap.alloc('after', 1024, 32)
top = heap.brk
heap.release('table')
again = heap.alloc('table', 1808960, 32)
assert again != first and heap.brk - top == 1806336, 'memalign must not fit the hole of the block it replaces (the console: "diff 1806336")'
assert heap.free_inside() >= 1808960
# A plain malloc of the same size does fit it, and a freed top block goes back above 256 KiB.
heap = budget.Heap(0x8c110000)
heap.alloc('low', 4096); first = heap.alloc('table', 500000); heap.alloc('high', 4096)
heap.release('table'); assert heap.alloc('table', 500000) == first
heap.release('high'); heap.alloc('large', 1 << 20); before = heap.brk; heap.release('large')
assert before - heap.brk >= (1 << 20) - 2 * budget.PAGE and heap.high == before
# Best fit, and the refusal at the ceiling of mm_sbrk.
heap = budget.Heap(0x8c110000)
for name, size in (('a', 300000), ('s1', 64), ('b', 100000), ('s2', 64), ('c', 200000), ('s3', 64)): heap.alloc(name, size)
at = {owner: address for address, (size, owner, request, kind) in heap.used.items()}
for name in 'abc': heap.release(name)
assert heap.alloc('fit', 90000) == at['b']
try:
    heap.alloc('everything', 16 << 20); raise SystemExit('a request beyond the heap limit was granted')
except MemoryError:
    assert heap.failed[0] == 'everything' and heap.failed[2] + heap.failed[3] >= budget.HEAP_LIMIT
print('PASS: allocator model: memalign after free grows the heap, malloc reuses the hole, trim, best fit, refusal at the limit')


def mesh_pack(path, meshes, vertices, faces, instances, stage_of_face=lambda f: 0, materials=4, strips=0, lod=0):
    """Aligned DNM6 with `meshes` equal meshes; instance i uses mesh i % meshes; material m has stage m."""
    flags = 1 | 2 | (4 | lod << 16 if lod else 0)
    parts = [struct.pack('<8I4i4I', 0x364d4e44 | 0x80000000, 0, meshes, meshes * vertices, meshes * faces, materials, instances, 1,
                         0, 0, 0, 0, strips, strips * 4 if strips else 0, 1, flags)]
    def pad():
        size = sum(map(len, parts)); parts.append(b'\0' * (-size % 32))
    parts.append(b''.join(struct.pack('<4I6f6I', m * vertices, vertices, m * faces, faces, *([0.0] * 6), *([0] * 6)) for m in range(meshes))); pad()
    parts.append(b'\0' * (16 * meshes * vertices)); pad()
    parts.append(b''.join(struct.pack('<5H2BI3fH2B', 0, 1, 2, 0, stage_of_face(f), 3, 0, 0x100, 0.0, 0.0, 1.0, 0, 0, 0)
                          for m in range(meshes) for f in range(faces))); pad()
    parts.append(b''.join(struct.pack('<4H3f3I', 0x4000, 0, m, 0, 1.0, 0.0, 0.0, 0, m, 0) for m in range(materials))); pad()
    parts.append(b''.join(struct.pack('<4I', i % meshes, 0, 0, 0) + b'\0' * 96 for i in range(instances))); pad()
    data = bytearray(b''.join(parts)); struct.pack_into('<I', data, 4, len(data))
    Path(path).write_bytes(data)


def material_pack(path, resources, bindings, payload=4096, palettes=2, lumas=32):
    metadata = (resources + bindings) * 16 + bindings * 4 + ((bindings + 3) & ~3) + palettes * 68
    start = (32 + metadata + 31) & ~31
    body = b'\0' * (16 * resources)
    body += b''.join(struct.pack('<6HI', 0x4000, 0, i // lumas, 0, i % lumas, (i // lumas) % resources, 0xffffffff) for i in range(bindings))
    body += b'\xff' * (4 * bindings) + bytes((i // lumas) % palettes for i in range(bindings)) + b'\0' * (-bindings % 4) + b'\0' * (68 * palettes)
    assert len(body) == metadata
    data = struct.pack('<4s7I', b'DNT6', start + payload, resources, bindings, payload - 32 * resources, 2048, palettes, 0)
    Path(path).write_bytes(data + body + b'\0' * (start - 32 - metadata) + b'\0' * payload)


def asset_set(directory, car_instances, car_vertices, bindings):
    directory = Path(directory); directory.mkdir()
    mesh_pack(directory / 'car.dnm', 4, car_vertices, 60, car_instances, strips=8)
    mesh_pack(directory / 'world.dnm', 6, 500, 100, 8, stage_of_face=lambda f: f % 4, lod=5)
    material_pack(directory / 'car.dnt', 16, bindings); material_pack(directory / 'world.dnt', 8, 512)
    for name in ('drive.dnd', 'course0.dnc', 'course0.dnr', *('course0-route%u.dnp' % i for i in range(8)), 'scenery.dnv',
                 'finish.dnm', 'result.dnm'):
        (directory / name).write_bytes(b'\0' * 4096)
    return directory


with tempfile.TemporaryDirectory(prefix='heap-budget-') as scratch:
    small = asset_set(Path(scratch) / 'small', 40, 300, 4096)
    car = budget.MeshPack(small / 'car.dnm'); world = budget.MeshPack(small / 'world.dnm', 1); whole = budget.MeshPack(small / 'world.dnm', 5)
    assert (car.faces, car.instances, car.queue_points, car.queue_faces, car.strip_indices) == (240, 40, 40 * 300, 40 * 60, 32)
    # The scenery stage keeps the faces of materials 0..stage: half of them at material stage 1.
    assert (world.queue_points, world.queue_faces, whole.queue_faces, world.faces, world.lod_groups) == (8 * 500, 8 * 50, 8 * 100, 600, 5)
    dnt = budget.MaterialPack(small / 'car.dnt')
    assert (dnt.count, dnt.metadata, dnt.classes) == (4096, (16 + 4096) * 16 + 4096 * 4 + 4096 + 2 * 68, 4096 // 32)
    for layout in ('tree', 'head'):
        heap = budget.run(small, 6, False, layout, 0x8c112000)
        assert not heap.failed
        names = {owner: request for size, owner, request, kind in heap.used.values()}
        assert names['car point cache'] == 40 * 300 * 32 and names['world face queues'] == 8 * 100 * 4
        assert ('car emission bindings' in names) == (layout == 'tree') and ('car emission records' in names) == (layout == 'head')
        if layout == 'tree':
            assert names['car emission bindings'] == 4096 * 8 and names['car emission classes'] == dnt.classes * 32
            assert not any(kind in ('scratch', 'load') for size, owner, request, kind in heap.used.values()), 'load scratch survives the load'
        else:
            assert names['car emission records'] == 4096 * 32
    # The header/UV cache of commit c318e6b was live while everything else was loaded: its peak is that much higher.
    assert budget.run(small, 6, False, 'head', 0x8c112000).high - budget.run(small, 6, False, 'tree', 0x8c112000).high >= 4096 * 64
    assert budget.run(small, 6, True, 'tree', 0x8c112000).in_use() - budget.run(small, 6, False, 'tree', 0x8c112000).in_use() >= budget.SAMPLE_BYTES
    run = lambda *arguments: subprocess.run([sys.executable, str(tool), *map(str, arguments)], capture_output=True, text=True)
    passed = run(small, '--stage', 6)
    assert passed.returncode == 0 and passed.stdout.rstrip().splitlines()[-1].startswith('PASS:'), passed.stdout + passed.stderr
    # A budget the set cannot keep, and content that does not fit at all: both must fail, by name.
    tight = run(small, '--stage', 6, '--peak-free', 15 << 20)
    assert tight.returncode == 1 and tight.stdout.rstrip().splitlines()[-1].startswith('FAIL:'), tight.stdout
    large = asset_set(Path(scratch) / 'large', 400, 1400, 60000)
    refused = run(large, '--stage', 6)
    assert refused.returncode == 1 and 'OUT OF MEMORY: car point cache needs 17920000 bytes' in refused.stdout, refused.stdout
print('PASS: pack arithmetic (instances, stage selection, DNT6 metadata), both layouts, budget verdicts and the refusal by owner')

sets = root.parents[1] / 'build'
if all((sets / name).is_dir() for name in ('dc-native-lod.70M0gR/assets-L8', 'dc-native-world-objects.ZINSuG/assets-objects')):
    assert budget.hardware_check() <= 16 * 1024, 'the model left the hardware measurements'
    print('PASS: commit c318e6b replayed against the console logs of 13 October 2026 within 16 KiB, stage 6 failure included')
else:
    print('SKIP: hardware replay (asset sets of the 13 October runs are not under build/)')

if len(sys.argv) > 1:
    for profile in ((), ('--profile',)):
        result = subprocess.run([sys.executable, str(tool), sys.argv[1], '--stage', sys.argv[2], *profile], capture_output=True, text=True)
        print(result.stdout.rstrip())
        assert result.returncode == 0, 'heap budget exceeded: %s stage %s %s' % (sys.argv[1], sys.argv[2], ' '.join(profile))
