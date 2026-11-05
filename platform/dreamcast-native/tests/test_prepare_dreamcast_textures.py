"""ROM-free format validation: python3 platform/dreamcast-native/tests/test_prepare_dreamcast_textures.py."""
import importlib.util
from pathlib import Path
import struct
import hashlib
import tempfile

spec = importlib.util.spec_from_file_location("prep", Path(__file__).resolve().parents[1] / "tools/prepare_dreamcast_textures.py")
prep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prep)

tga = bytearray(18)
tga[2] = 2
struct.pack_into("<HHBB", tga, 12, 8, 8, 32, 0x28)
tga += b"\x01\x02\x03\xff" * 64
assert prep.exact_palette(bytes(tga), 8, 8) == b"DPAL\x01\0\0\0\x01\x02\x03\xff"
too_many = tga.copy()
for i in range(17):
    too_many[18 + i * 4] = i
try:
    prep.exact_palette(bytes(too_many), 8, 8)
except ValueError:
    pass
else:
    raise AssertionError("accepted 17 colors as exact PAL4")

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / "texture.dt"
    data = bytearray(64)
    data[:4] = b"DcTx"
    struct.pack_into("<I", data, 4, len(data))
    struct.pack_into("<HHI", data, 12, 32, 64, (1 << 30) | (1 << 27))
    path.write_bytes(data)
    info = prep.dt_info(path, 32, 64, 1, True)
    assert info["vram_payload_bytes"] == 32 and info["file_bytes"] == 64
    for offset, value in ((0, 0), (4, 63), (8, 1), (9, 1), (12, 64), (19, 0x08)):
        bad = data.copy()
        bad[offset] = value
        path.write_bytes(bad)
        try:
            prep.dt_info(path, 32, 64, 1, True)
        except ValueError:
            pass
        else:
            raise AssertionError(f"accepted invalid DcTx field at {offset}")
    path.write_bytes(data)
    entry = {"file": path.name, "width": 32, "height": 64, **info}
    mappings = [entry.copy(), entry.copy()]
    size = prep.write_pack(Path(directory), mappings)
    pack = (Path(directory) / "textures.dtp").read_bytes()
    assert size == len(pack) == 128 and pack[:4] == b"DTP1"
    assert struct.unpack_from("<I", pack, 8)[0] == 1
    assert mappings[0]["pack_index"] == mappings[1]["pack_index"] == 0
    try:
        prep.write_pack(Path(directory), mappings)
    except FileExistsError:
        pass
    else:
        raise AssertionError("overwrote an existing resource pack")
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    source = bytearray(tga[:18])
    struct.pack_into("<HH", source, 12, 32, 32)
    source += b"\x01\x02\x03\xff" * 1024
    (root / "material.tga").write_bytes(source)
    state = b"\x01\x02\x03\xff" * 16 + bytes(512)
    (root / "material.m2t").write_bytes(state)
    entry = {"header": [0x4000, 0, 0, 0], "luma": 255, "width": 32, "height": 32,
             "source_file": "material.tga", "source_sha256": hashlib.sha256(source).hexdigest(), "pack_index": 0}
    second = {**entry, "luma": 254}
    proof = prep.material_blob([entry, second], root, 1)
    assert len(proof) == 32 + 2 * 96 + 512  # the two exact states share source indices
    assert struct.unpack_from("<I", proof, 32 + 16)[0] == struct.unpack_from("<I", proof, 128 + 16)[0]
    (root / "material.m2t").write_bytes(bytes(64) + state[64:])
    try:
        prep.material_blob([entry], root, 1)
    except ValueError:
        pass
    else:
        raise AssertionError("accepted a proof with different source colours")
print("PASS: DcTx validation, pack deduplication and exact material/source proofs")
