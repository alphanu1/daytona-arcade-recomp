"""Binding-key DNT4 packs for the asset recipes (platform/dreamcast-native/recipes/build_assets.sh).

The world and car packs of the delivered asset set are DNT6 packs made by
tools/prepare_palette_materials.py from a DNT4 source pack, and the mesh packs
are ordered by tools/prepare_face_order.py --materials. Of a DNT4 those tools
read only the bindings: the (texture header, luma) keys in pack order, which
bindings are solid and their colour, and (prepare_face_order.py
material_ranks) the texture resource number of each binding. Texture blobs,
crops and the modulation of textured bindings are never read. The historical
DNT4 packs were made by one-off fitting scripts (VQ anchors, adaptive palette
fits) that are not part of the repository; this tool writes a DNT4 with the
same bindings and no texture data, which gives the same outputs downstream.

  key_pack.py cells EXPORT_DIR RESOURCES.txt NEW.dnt
      world: keys of an m2segment --export-native-cells --race-scenery-lights
      export (materials/materials.txt textured, solid_materials.txt solid with
      its colour); every binding of a textured header takes the resource
      number RESOURCES.txt gives that header. RESOURCES.txt "auto" (a course
      without a historical pack): the resource numbers are the distinct
      texture words (h2, h3) of the textured headers, numbered in sorted order,
      the rule the historical numbers follow for most headers.
  key_pack.py dnt5 CAR.dnt NEW.dnt
      cars: the bindings of tools/prepare_indexed_materials.py's DNT5 pack, in
      its order (solid colours are carried in the crop field as in DNT4).

Bindings are written sorted by their first ten record bytes, the order of the
historical packs. Output holds ROM-derived keys and colours: write it under an
ignored directory only.
"""
import argparse
from pathlib import Path
import struct

HEADER = struct.Struct('<4s7I')
RESOURCE = struct.Struct('<IIHHI')
BINDING = struct.Struct('<6HI')
SOLID = 65535


def rows(path):
    for line in path.read_text().splitlines():
        if line.strip() and not line.startswith('#'):
            yield line.split()


def cells_bindings(export, resources):
    table = {}
    if str(resources) == 'auto':
        headers = {tuple(map(int, f[4:8])) for f in rows(export/'materials'/'materials.txt')}
        words = sorted({h[2:4] for h in headers})
        table = {h: words.index(h[2:4]) for h in headers}
    else:
        for f in rows(resources):
            table[tuple(map(int, f[:4]))] = int(f[4])
    bindings = []
    for f in rows(export/'materials'/'materials.txt'):
        header, luma = tuple(map(int, f[4:8])), int(f[8])
        if header not in table:
            raise ValueError(f'textured header {header} has no resource number in {resources}')
        bindings.append((header, luma, table[header], 0))
    for f in rows(export/'solid_materials.txt'):
        header, luma = tuple(map(int, f[:4])), int(f[4])
        bindings.append((header, luma, SOLID, int(f[5])))
    return bindings


def dnt5_bindings(path):
    data = path.read_bytes()
    magic, size, resources, count = struct.unpack_from('<4s3I', data)
    if magic != b'DNT5' or size != len(data):
        raise ValueError('expected a DNT5 pack (tools/prepare_indexed_materials.py)')
    out = []
    for i in range(count):
        *header, luma, resource, crop = BINDING.unpack_from(data, 32+16*resources+16*i)
        out.append((tuple(header), luma, SOLID if resource == SOLID else 0, crop if resource == SOLID else 0))
    return out


def write(bindings, output):
    records = sorted(BINDING.pack(*header, luma, resource, crop) for header, luma, resource, crop in bindings)
    if len({r[:10] for r in records}) != len(records):
        raise ValueError('duplicate (header, luma) binding')
    used = [struct.unpack_from('<H', r, 10)[0] for r in records]
    resources = max([r for r in used if r != SOLID], default=-1)+1
    start = (HEADER.size+RESOURCE.size*resources+20*len(records)+31) & ~31
    data = bytearray(HEADER.pack(b'DNT4', start, resources, len(records), 0, 0, 0, 0))
    data += RESOURCE.pack(start, 0, 0, 0, 0)*resources
    data += b''.join(records)+struct.pack(f'<{len(records)}I', *([0xffffffff]*len(records)))
    data += bytes(start-len(data))
    with output.open('xb') as f:
        f.write(data)
    print(f'{output}: DNT4 binding keys, {len(records)} bindings ({used.count(SOLID)} solid), '
          f'{resources} resource numbers, no texture data')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='mode', required=True)
    cells = sub.add_parser('cells')
    cells.add_argument('export', type=Path)
    cells.add_argument('resources', type=Path)
    cells.add_argument('output', type=Path)
    dnt5 = sub.add_parser('dnt5')
    dnt5.add_argument('source', type=Path)
    dnt5.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.mode == 'cells':
        write(cells_bindings(args.export, args.resources), args.output)
    else:
        write(dnt5_bindings(args.source), args.output)


if __name__ == '__main__':
    main()
