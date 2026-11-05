"""ROM-free DNM1..5 table-byte preservation and real-reader alignment checks."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import prepare_models as models
from prepare_strips import prepare_strips
from inspect_scenery_components import inspect as inspect_components
from inspect_quad_strips import inspect as inspect_strips
from filter_scenery_faces import filtered


def tables(data):
    # Independent layout walk: compare complete records, not floating tolerances.
    h = models.HEADER.unpack_from(data)
    version = (h[0] >> 24) & 0x7f
    sizes = [h[2]*64, h[3]*16, h[4]*32, h[5]*32, h[6]*112]
    alignments = [1]*5
    if version >= 0x32:
        sizes.append(h[3]*2)
        alignments.append(1)
    if version >= 0x33:
        sizes.append(h[7]*(24 if version == 0x33 else 64))
        alignments.append(4)
    if version == 0x35:
        sizes.extend((h[12]*32, h[13]*2))
        alignments.extend((32, 1))
    offset, spans, padding = 64, [], []
    for size, alignment in zip(sizes, alignments):
        if h[0] & 0x80000000:
            alignment = 32
        start = (offset+alignment-1)&-alignment
        padding.extend(range(offset, start))
        spans.append((start, start+size))
        offset = start+size
    end = (offset+31)&~31 if h[0]&0x80000000 else (
        (offset+3)&~3 if version == 0x35 else offset)
    padding.extend(range(offset, end))
    assert end == len(data) and not any(data[i] for i in padding)
    return spans, padding


with tempfile.TemporaryDirectory(prefix='meshpack-align-') as temp:
    root = Path(temp)
    # Odd vertex count forces the reported half-line face/material defect;
    # one and two instances exercise both parities of the 112-byte stride.
    (root/'mesh_0.txt').write_text('mode 1\nbounds 0 0 1 2 1 1\n'+
        'face 3 514 16384 0 0 0 0 0 1 128 64 0 0\n'+
        'v 0 0 1 0 0\nv 1 0 1 256 0\nv 0 1 1 0 256\n'+
        'face 3 514 16384 0 0 0 0 0 1 128 64 0 0\n'+
        'v 0 1 1 0 256\nv 1 0 1 256 0\nv 1 1 1 256 256\n'+
        'face 3 514 16384 0 0 0 0 0 1 128 64 0 0\n'+
        'v 1 1 1 256 256\nv 1 0 1 256 0\nv 2 0 1 512 0\n')
    path = root/'check.dnm'
    def accepted(data):
        path.write_bytes(data)
        result = subprocess.run([sys.argv[1], str(path)], capture_output=True, text=True)
        assert result.returncode in (0, 1), (result.stdout, result.stderr)
        return result.returncode == 0
    checked = rejected = 0
    for instances in (1, 2):
        (root/'manifest.json').write_text(json.dumps(dict(meshes=1, instances=instances,
            unique_faces=3, unindexed_vertices=9, crtc_offset=[0, 0], render_offset=[0, 0])))
        instance = '0 8388608 0 0 1 0 0 0 1 0 0 0 1 0 0 10 256 256 0 0 1 0 0 496 384 248 192\n'
        (root/'instances.txt').write_text(instance*instances)
        plain, _ = models.prepare(root)
        shared = models.share_positions(plain)
        grouped = models.group_faces(shared, 2)
        strips, _ = prepare_strips(grouped)
        assert models.HEADER.unpack_from(strips)[0] == 0x354d4e44
        # DNM3 is still supported: same group references, spatial bounds only.
        spans, _ = tables(grouped)
        h = list(models.HEADER.unpack_from(grouped))
        bounds = b''.join(grouped[i:i+24] for i in range(spans[-1][0], len(grouped), 64))
        spatial = bytearray(grouped[:spans[-1][0]]+bounds)
        h[0], h[1] = 0x334d4e44, len(spatial)
        models.HEADER.pack_into(spatial, 0, *h)
        for original in (plain, shared, bytes(spatial), grouped, strips):
            aligned = models.align_tables(original)
            old_spans, _ = tables(original)
            new_spans, padding = tables(aligned)
            assert all(start%32 == 0 for start, _ in new_spans)
            assert len(aligned)%32 == 0 and padding
            assert models.table_offsets(models.HEADER.unpack_from(aligned)) == tuple(a for a, _ in new_spans[:5])
            assert models.table_offsets(models.HEADER.unpack_from(original)) == tuple(a for a, _ in old_spans[:5])
            assert models.HEADER.unpack_from(aligned)[2:] == models.HEADER.unpack_from(original)[2:]
            assert [original[a:b] for a, b in old_spans] == [aligned[a:b] for a, b in new_spans]
            assert new_spans[4][0] == new_spans[3][1]  # SQ original-instance sizing.
            assert accepted(original) and accepted(aligned)
            try:
                filtered(aligned, set())
            except ValueError:
                pass
            else:
                raise AssertionError('legacy face filter must not misread aligned packs')
            path.write_bytes(original)
            before_components = inspect_components(Path(sys.argv[1]), path, {0})
            before_strips = inspect_strips(Path(sys.argv[1]), path)
            path.write_bytes(aligned)
            assert inspect_components(Path(sys.argv[1]), path, {0}) == before_components
            assert inspect_strips(Path(sys.argv[1]), path) == before_strips
            checked += 1
            # Corrupt every padding byte, not just a convenient representative.
            for offset in padding:
                bad = bytearray(aligned)
                bad[offset] = 1
                assert not accepted(bad), offset
                rejected += 1
            for offset, value in ((0, 0x914d4e44), (8, 0), (12, 0xffffffff), (24, 0xffffffff)):
                bad = bytearray(aligned)
                struct.pack_into('<I', bad, offset, value)
                assert not accepted(bad)
                rejected += 1
            for bad in (aligned[:-1], aligned+b'\0'):
                assert not accepted(bad)
                rejected += 1
            falsely_marked = bytearray(original)
            struct.pack_into('<I', falsely_marked, 0, models.HEADER.unpack_from(original)[0]|0x80000000)
            assert not accepted(falsely_marked)
        # Real CLI must apply alignment LAST, also after grouped strip metadata.
        for flags, expected in (([], plain), (['--face-group-size', '2', '--join-strips'], strips)):
            output = root/f'cli-{instances}-{len(flags)}.dnm'
            result = subprocess.run([sys.executable, str(Path(models.__file__)), str(root),
                str(output), '--align-tables', *flags], capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            assert output.read_bytes() == models.align_tables(expected)
            report = json.loads(result.stdout)
            assert report['aligned_tables'] and report['bytes'] == output.stat().st_size
            assert accepted(output.read_bytes())
    for bad in (b'', plain[:63], plain[:-1], plain+b'x', struct.pack('<I', 0xffffffff)+plain[4:]):
        try:
            models.align_tables(bad)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid input repacked')
print(f'PASS aligned DNM1..5: {checked} byte-identical table comparisons, {rejected} rejected inputs, CLI and real C reader')
