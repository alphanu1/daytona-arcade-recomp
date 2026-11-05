"""Create a separate diagnostic DNM with explicitly selected materials omitted.

Retains all vertices/materials/instances and conservative bounds. No gameplay or
texture changes. Both packs must pass the existing native C meshpack validator.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import subprocess
import tempfile

import prepare_models as models


def filtered(data, omitted):
    header = list(models.HEADER.unpack_from(data))
    if header[0] & models.ALIGNED:
        raise ValueError('diagnostic face filtering requires legacy layout; align after filtering')
    face_base = models.HEADER.size+header[2]*models.MESH.size+header[3]*models.VERTEX.size
    material_base = face_base+header[4]*models.FACE.size
    prefix = bytearray(data[:face_base])
    faces = bytearray()
    removed = Counter()
    for i in range(header[2]):
        at = models.HEADER.size+i*models.MESH.size
        mesh = list(models.MESH.unpack_from(data, at))
        first = len(faces)//models.FACE.size
        for f in range(mesh[2], mesh[2]+mesh[3]):
            raw = data[face_base+f*models.FACE.size:face_base+(f+1)*models.FACE.size]
            material = models.FACE.unpack(raw)[4]
            if material in omitted:
                removed[material] += 1
            else:
                faces.extend(raw)
        mesh[2], mesh[3] = first, len(faces)//models.FACE.size-first
        if not mesh[3]:
            raise ValueError(f'filter would empty mesh {i}; no automatic instance removal')
        models.MESH.pack_into(prefix, at, *mesh)
    header[4] = len(faces)//models.FACE.size
    header[1] = len(prefix)+len(faces)+len(data)-material_base
    models.HEADER.pack_into(prefix, 0, *header)
    return bytes(prefix)+bytes(faces)+data[material_base:], dict(removed)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reader', type=Path)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--materials', type=int, nargs='+', required=True)
    args = parser.parse_args()
    reader = args.reader.resolve()
    subprocess.run([str(reader), str(args.source.resolve())], check=True)
    original = args.source.read_bytes()
    assert filtered(original, set())[0] == original
    result, counts = filtered(original, set(args.materials))
    if set(counts) != set(args.materials):
        raise ValueError('every requested material must occur; no silent unused IDs')
    # Idempotence guards index rebuilding: a repeated filter changes no bytes.
    assert filtered(result, set(args.materials)) == (result, {})
    with tempfile.TemporaryDirectory(prefix='daytona-face-filter-') as tmp:
        check = Path(tmp)/'candidate.dnm'
        check.write_bytes(result)
        subprocess.run([str(reader), str(check)], check=True)
    with args.output.open('xb') as out:
        out.write(result)
    print(json.dumps(dict(removed=counts, removed_total=sum(counts.values()),
                         source_bytes=len(original), result_bytes=len(result)), sort_keys=True))
