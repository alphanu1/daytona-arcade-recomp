"""python3 test_mipmap_materials.py READER WORLD_DNT MIPMAP_BUDGET_DIR [NEW_DNT]

Exercise actual encoder output through native reader in a temporary DNT4 pack.
Optional NEW_DNT retains the checked diagnostic pack for hardware testing.
Not a source-quality exporter: original assets stay unchanged.
"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from check_materials import decode_texture16, decode_vq16
from prepare_materials import HEADER, RESOURCE


def check(reader, source, candidates, output=None):
    if output is not None and output.exists():
        raise FileExistsError(output)
    # Distinct lower-mip/top-level indices detect accidentally decoding mip0
    # at the old base-only offset. Smallest 8x8 chain has22 indices, top16 last.
    blob = bytearray(2112)
    struct.pack_into('<4sI4BHHI', blob, 0, b'DcTx', len(blob), 0, 0, 255, 0, 8, 8, 0xc8000000)
    struct.pack_into('<4H', blob, 40, *([0xf800]*4))
    blob[2086:2102] = bytes([1]*16)
    assert decode_texture16(blob) == (8, 8, bytes([0, 0, 255, 255])*64)
    try:
        decode_vq16(blob)
    except ValueError:
        pass
    else:
        raise AssertionError('legacy crop decoder must reject mips')
    data = source.read_bytes()
    header = list(HEADER.unpack_from(data))
    assert header[0] == b'DNT4'
    nr = header[2]
    records = [list(RESOURCE.unpack_from(data, 32+i*16)) for i in range(nr)]
    replacement = {r['resource']: (candidates/f"{r['resource']}.dt").read_bytes()
                   for r in json.loads((candidates/'budget.json').read_text())['resources']
                   if 'extra_bytes' in r}
    assert replacement
    rebuilt = bytearray(data[:records[0][0]])
    payload = maximum = 0
    for i, record in enumerate(records):
        offset, size, w, h, _ = record
        texture = replacement.get(i, data[offset:offset+size])
        assert decode_texture16(texture)[:2] == (w, h)
        record[0:2] = [len(rebuilt), len(texture)]
        RESOURCE.pack_into(rebuilt, 32+i*16, *record)
        rebuilt.extend(texture)
        payload += len(texture)-32
        maximum = max(maximum, len(texture))
    header[1], header[4], header[5] = len(rebuilt), payload, maximum
    HEADER.pack_into(rebuilt, 0, *header)
    with tempfile.TemporaryDirectory(prefix='daytona-mips-') as directory:
        path = Path(directory)/'world.dnt'
        path.write_bytes(rebuilt)
        subprocess.run([str(reader), str(path)], check=True)
        subprocess.run([sys.executable, str(Path(__file__).with_name('test_material_residency.py')),
                        str(path)], check=True)
    print(f'PASS: mip base offset, legacy crop rejection, {len(replacement)} actual encoded resources through C pack reader')
    if output is not None:
        with output.open('xb') as file:
            file.write(rebuilt)


if __name__ == '__main__':
    if len(sys.argv) not in (4, 5):
        raise SystemExit(__doc__)
    check(*(Path(arg).resolve() for arg in sys.argv[1:]))
