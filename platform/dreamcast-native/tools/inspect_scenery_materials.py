"""Read-only material contact sheets for identifying scenery, not filtering it.

Usage: python3 inspect_scenery_materials.py MESH_READER MATERIAL_READER DNM DNT NEW_DIR
ROM-derived images/metadata must stay in ignored local output directories.
"""
from collections import Counter
import json
from pathlib import Path
import struct
import subprocess
import sys

from PIL import Image, ImageDraw
from check_materials import decode_texture16
import prepare_models as models
from prepare_materials import HEADER, RESOURCE, BINDING


def inspect(mesh_reader, material_reader, mesh_path, texture_path, output):
    subprocess.run([str(mesh_reader), str(mesh_path)], check=True)
    subprocess.run([str(material_reader), str(texture_path), '--cpu-backed'], check=True)
    mesh, textures = mesh_path.read_bytes(), texture_path.read_bytes()
    h = models.HEADER.unpack_from(mesh)
    _, _, face_base, material_base, _ = models.table_offsets(h)
    counts = Counter(models.FACE.unpack_from(mesh, face_base+i*models.FACE.size)[4] for i in range(h[4]))
    magic, _, resources, bindings, *_ = HEADER.unpack_from(textures)
    if magic not in (b'DNT2', b'DNT4'):
        raise ValueError('requires resident DNT2/DNT4 textures')
    binding_base = HEADER.size+resources*RESOURCE.size
    representatives = {}
    for i in range(bindings):
        b = BINDING.unpack_from(textures, binding_base+i*BINDING.size)
        if b[:4] not in representatives or b[4] > representatives[b[:4]][4]:
            representatives[b[:4]] = b
    output.mkdir(exist_ok=False)
    rows = []
    sheet = None
    for i in range(h[5]):
        if i % 64 == 0:
            sheet = Image.new('RGB', (1024, 1024), '#303030')
        m = models.MATERIAL.unpack_from(mesh, material_base+i*models.MATERIAL.size)
        b = representatives[m[:4]]
        x, y = (i%8)*128, ((i%64)//8)*128
        if b[5] != 65535:
            offset, size, _, _, _ = RESOURCE.unpack_from(textures, HEADER.size+b[5]*RESOURCE.size)
            width, height, pixels = decode_texture16(textures[offset:offset+size])
            tile = Image.frombytes('RGBA', (width,height), pixels, 'raw', 'BGRA')
            tile.thumbnail((124,100), Image.Resampling.NEAREST)
            sheet.paste(tile, (x+2,y+24), tile)
        else:
            ImageDraw.Draw(sheet).rectangle((x+2,y+24,x+125,y+123), fill=f'#{b[6]&0xffffff:06x}')
        ImageDraw.Draw(sheet).text((x+2,y+2), f'M{i} faces={counts[i]}', fill='white')
        rows.append(dict(material=i, header=m[:4], faces=counts[i], resource=b[5], luma=b[4],
                         diffuse=m[4], ambient=m[5], sheet=f'materials-{i//64}.png'))
        if i%64 == 63 or i+1 == h[5]:
            sheet.save(output/f'materials-{i//64}.png')
    assert sum(row['faces'] for row in rows) == h[4]
    (output/'materials.json').write_text(json.dumps(rows, indent=2)+'\n')
    print(f'PASS: {len(rows)} labelled materials, {h[4]} faces; brightest stored anchors, not final modulated appearance')


if __name__ == '__main__':
    if len(sys.argv) != 6:
        raise SystemExit(__doc__)
    inspect(*(Path(value).resolve() for value in sys.argv[1:]))
