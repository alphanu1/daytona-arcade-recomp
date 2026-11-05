"""Diagnostic only: READER SOURCE_DNT4 NEW_DNT4; colour each existing VQ mip.

Keeps resource sizes, mip flags, bindings, geometry and bias unchanged.
Largest level red, then green, blue, yellow, magenta, cyan, white, grey.
"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from prepare_materials import HEADER, RESOURCE


def colour(reader, source, output):
    if output.exists():
        raise FileExistsError(output)
    subprocess.run([str(reader), str(source)], check=True)
    original = source.read_bytes()
    data = bytearray(original)
    magic, _, count, *_ = HEADER.unpack_from(data)
    if magic != b'DNT4':
        raise ValueError('expected validated DNT4')
    colours = [(31,0,0), (0,63,0), (0,0,31), (31,63,0),
               (31,0,31), (0,63,31), (31,63,31), (16,32,16)]
    changed = 0
    for resource in range(count):
        offset, size, width, height, _ = RESOURCE.unpack_from(data, 32+resource*16)
        mode = struct.unpack_from('<I', data, offset+16)[0]
        if not mode & (1 << 31):
            continue
        assert width == height and data[offset+10] == 255 and mode & (1 << 30)
        for index, (r, g, b) in enumerate(colours):
            word = (r<<11 | g<<5 | b) if mode & (1<<27) else (0x8000 | r<<10 | (g//2)<<5 | b)
            struct.pack_into('<4H', data, offset+32+index*8, *([word]*4))
        cursor = offset+32+2048
        levels = width.bit_length()
        for exponent in range(levels):
            pixels = (1 << exponent)**2
            length = max(1, pixels//4)
            index = min(levels-1-exponent, len(colours)-1)
            data[cursor:cursor+length] = bytes([index])*length
            cursor += length
        assert cursor-offset <= size and ((cursor-offset+31)&~31) == size
        assert data[offset:offset+32] == original[offset:offset+32]
        changed += 1
    if not changed:
        raise ValueError('no mipmapped resources')
    assert len(data) == len(original)
    with tempfile.NamedTemporaryFile() as temporary:
        temporary.write(data); temporary.flush()
        subprocess.run([str(reader), temporary.name], check=True)
    with output.open('xb') as file:
        file.write(data)
    print(f'PASS: {changed} mip chains coloured, sizes/header flags unchanged')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    colour(*(Path(arg).resolve() for arg in sys.argv[1:]))
