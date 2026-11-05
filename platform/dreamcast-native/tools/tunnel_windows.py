"""The car windows' tunnel texture (Advanced): 6fe0 -> 19dc8 rewrites the window polygons' texture headers while
the player is in a tunnel of course 2: word 0 |= 0x3c0 (wrap and mirror bits), word 2's low 13 bits = 0x0c0c (a
64x64 texture of course 2's texture RAM, sheet 0 at 384, 512); 7008 -> 19dc8 restores them from ROM.

  tunnel_windows.py video VIDEO VIDEO2 NEW_DIR
      VIDEO (course 0's dump, the car pack's) with the tunnel texture's block of sheet 0 taken from VIDEO2 (a dump
      of an Advanced race): the car's own textures lie in the part of texture RAM every course shares, the window
      palette (colour base 0x11a) in the part of palette RAM that is equal, so only the block differs.
  tunnel_windows.py bindings IN.dnt NEW.dnt
      the DNT4 binding keys of the car (append_player_parts.py's) plus, for every binding of the window header
      (4409 0 1b98 4680), one of the tunnel header (47c9 0 0c0c 4680) with the same luma and a texture resource of
      its own; include/render/models/model_renderer.h nm_render_car_material_header swaps them at run time.

ROM-derived output: write only below ignored build/ directories.
"""
import argparse
from pathlib import Path
import shutil
import struct

WINDOW = (0x4409, 0, 0x1b98, 0x4680)
TUNNEL_OR = (0x03c0, 0, 0x0c0c, 0)      # 19dc8 at 6fec: g1 = 03c0:03c0, g3 = 1fff:0c0c (mask:value)
TUNNEL_MASK = (0x03c0, 0, 0x1fff, 0)


def tunnel_header(header):
    return tuple((h & ~m) | (v & m) for h, m, v in zip(header, TUNNEL_MASK, TUNNEL_OR))


def block_words(header):
    """The tex0 words (32-bit) holding the texels of the header's texture (prepare_palette_probe.texels' layout)."""
    width, height = 32 << (header[0] & 7), 32 << ((header[0] >> 3) & 7)
    bx, by = (header[2] & 63) * 32, ((header[2] >> 6) & 31) * 32
    if header[2] & 0x1000:
        raise ValueError('the tunnel texture is on sheet 0')
    words = set()
    for v in range(height):
        for u in range(width):
            x, y = bx + u, by + v
            fold = 512 if x >= 1024 else 0
            if fold:
                x -= 1024
            off = (((y >> 1) ^ fold) << 9) + (x >> 1)
            words.add((off >> 1) & 0x7ffff)
    return sorted(words)


def video(source, advanced, output):
    output.mkdir()
    for f in source.iterdir():
        shutil.copy(f, output / f.name)
    tex = bytearray((source / 'tex0.bin').read_bytes())
    other = (advanced / 'tex0.bin').read_bytes()
    words = block_words(tunnel_header(WINDOW))
    for w in words:
        tex[4 * w:4 * w + 4] = other[4 * w:4 * w + 4]
    (output / 'tex0.bin').write_bytes(tex)
    print(f'{output}: {len(words)} words of tex0 (the tunnel texture of 64x64 texels) from {advanced}')


HEADER = struct.Struct('<4s7I')
RESOURCE = struct.Struct('<IIHHI')
BINDING = struct.Struct('<6HI')
SOLID = 65535


def bindings(source, output):
    data = source.read_bytes()
    magic, start, resources, count = struct.unpack_from('<4s3I', data)
    if magic != b'DNT4':
        raise ValueError('expected a DNT4 binding-key pack')
    at = HEADER.size + RESOURCE.size * resources
    rows = [BINDING.unpack_from(data, at + BINDING.size * i) for i in range(count)]
    window = [r for r in rows if tuple(r[:4]) == WINDOW]
    if not window or any(tuple(r[:4]) == tunnel_header(WINDOW) for r in rows):
        raise ValueError('expected window bindings and no tunnel binding yet')
    used = [r[5] for r in rows if r[5] != SOLID]
    new = max(used, default=-1) + 1
    rows += [(*tunnel_header(WINDOW), r[4], new, 0) for r in window]
    records = sorted(BINDING.pack(*r) for r in rows)
    resources = new + 1
    start = (HEADER.size + RESOURCE.size * resources + 20 * len(records) + 31) & ~31
    out = bytearray(HEADER.pack(b'DNT4', start, resources, len(records), 0, 0, 0, 0))
    out += RESOURCE.pack(start, 0, 0, 0, 0) * resources
    out += b''.join(records) + struct.pack(f'<{len(records)}I', *([0xffffffff] * len(records)))
    out += bytes(start - len(out))
    with output.open('xb') as f:
        f.write(out)
    print(f'{output}: {len(window)} tunnel window bindings added (resource {new}), {len(records)} bindings')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='mode', required=True)
    v = sub.add_parser('video'); v.add_argument('source', type=Path); v.add_argument('advanced', type=Path)
    v.add_argument('output', type=Path)
    b = sub.add_parser('bindings'); b.add_argument('source', type=Path); b.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.mode == 'video':
        video(args.source, args.advanced, args.output)
    else:
        bindings(args.source, args.output)


if __name__ == '__main__':
    main()
