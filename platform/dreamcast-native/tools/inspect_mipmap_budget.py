"""Encode square scenery candidates offline to measure actual mipmap bytes.

Usage: inspect_mipmap_budget.py READER PVRTEX DNT MATERIALS_JSON NEW_DIR ID...
Not a shipping exporter: inputs are already lossy anchors, not source images.
Rectangular resources are reported, never silently resized. No pack is changed.
"""
import json
from pathlib import Path
import struct
import subprocess
import sys

from PIL import Image
from check_materials import decode_texture16
from prepare_materials import HEADER, RESOURCE, BINDING


def inspect(reader, encoder, pack_path, material_path, output, ids):
    subprocess.run([str(reader), str(pack_path), '--cpu-backed'], check=True)
    data = pack_path.read_bytes()
    magic, _, nr, nb, *_ = HEADER.unpack_from(data)
    if magic not in (b'DNT2', b'DNT4'):
        raise ValueError('requires resident DNT2/DNT4')
    materials = json.loads(material_path.read_text())
    selected = {m['material']: tuple(m['header']) for m in materials if m['material'] in ids}
    if set(selected) != set(ids):
        raise ValueError('unknown material IDs')
    bindings = [BINDING.unpack_from(data, HEADER.size+nr*RESOURCE.size+i*BINDING.size)
                for i in range(nb)]
    resources = {b[5] for b in bindings if b[:4] in selected.values() and b[5] != 65535}
    output.mkdir(exist_ok=False)
    rows = []
    for resource in sorted(resources):
        offset, size, w, h, _ = RESOURCE.unpack_from(data, HEADER.size+resource*RESOURCE.size)
        blob = data[offset:offset+size]
        dw, dh, pixels = decode_texture16(blob)
        assert (dw, dh) == (w, h)
        row = dict(resource=resource, width=w, height=h, old_bytes=size,
                   materials=[i for i, key in selected.items()
                              if any(b[:4] == key and b[5] == resource for b in bindings)])
        rows.append(row)
        if w != h:
            row['status'] = 'rectangular: needs separate layout/UV review'
            continue
        source, target = output/f'{resource}.png', output/f'{resource}.dt'
        Image.frombytes('RGBA', (w, h), pixels, 'raw', 'BGRA').save(source)
        fmt = 'RGB565' if struct.unpack_from('<I', blob, 16)[0] & (1 << 27) else 'ARGB1555'
        subprocess.run([str(encoder), '-i', str(source), '-o', str(target),
                        '-f', fmt, '-c', '256', '-m'], check=True, capture_output=True)
        encoded = target.read_bytes()
        ew, eh, mode = struct.unpack_from('<HHI', encoded, 12)
        # Independent size check: full codebook plus every square mip index.
        indices = 1 + sum((1 << level)**2//4 for level in range(1, w.bit_length()))
        expected = (32 + 2048 + indices + 31) & ~31
        assert encoded[:4] == b'DcTx' and len(encoded) == expected
        assert struct.unpack_from('<I', encoded, 4)[0] == expected
        assert (ew, eh) == (w, h) and mode & (1 << 31) and mode & (1 << 30)
        assert encoded[8:11] == bytes([0, 0, 255])
        row.update(status='encoded; quality/runtime unverified', new_bytes=expected,
                   extra_bytes=expected-size)
    report = dict(material_ids=sorted(ids), resources=rows,
                  encoded_resources=sum('extra_bytes' in r for r in rows),
                  rectangular_resources=sum(r['width'] != r['height'] for r in rows),
                  extra_bytes=sum(r.get('extra_bytes', 0) for r in rows),
                  caveat='Square subset only; re-encoded lossy anchors, not shipping assets or hardware approval.')
    (output/'budget.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'resources'}, indent=2))


if __name__ == '__main__':
    if len(sys.argv) < 7:
        raise SystemExit(__doc__)
    inspect(*(Path(v).resolve() for v in sys.argv[1:6]), {int(v) for v in sys.argv[6:]})
