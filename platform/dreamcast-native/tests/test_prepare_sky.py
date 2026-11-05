"""Check real prepared sky pages, lossless slicing and actual VQ previews.

python3 platform/dreamcast-native/tests/test_prepare_sky.py SOURCES/panorama.tga PREPARED
Uses the already-installed Pillow, like check_materials.py --previews.
"""
import hashlib
import json
from pathlib import Path
import sys
import struct

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from prepare_scene import read_tga, dt_info
from check_materials import decode_vq16


def check(source, prepared):
    report = json.loads((prepared / "manifest.json").read_text())
    assert report["source_sha256"] == hashlib.sha256(source.read_bytes()).hexdigest()
    pixels = read_tga(source, 2048, 392)
    assert report["fill_blue_argb"] == int.from_bytes(pixels[0], "little")
    assert report["fill_ground_argb"] == int.from_bytes(pixels[384*2048], "little")
    assert len(report["textures"]) == 4 and report["palette_banks"] == 0
    payload = 0
    for n, entry in enumerate(report["textures"]):
        assert entry["file"] == f"sky_{n}.dt"
        assert (entry["source_x"], entry["source_y"], entry["content_height"]) == (n*512, 0, 392)
        assert (entry["width"], entry["height"], entry["format"], entry["compressed"]) == (512, 512, "RGB565", True)
        page = read_tga(prepared / f"sky_{n}.tga", 512, 512)
        for y in range(512):
            start = min(y, 391)*2048+n*512
            assert page[y*512:(y+1)*512] == pixels[start:start+512]
        target = prepared / entry["file"]
        for key, value in dt_info(target, 512, 512, 1, True).items():
            assert entry[key] == value
        w, h, decoded = decode_vq16(target.read_bytes())
        with Image.open(target.with_suffix(".png")) as image:
            assert image.size == (w, h)
            assert image.convert("RGBA").tobytes("raw", "BGRA") == decoded
        payload += entry["vram_payload_bytes"]
    assert payload == report["vram_payload_bytes"]
    assert payload+4*32 == report["kos_allocation_bytes"]
    pack=(prepared / "sky.dns").read_bytes()
    assert len(pack)==report["pack_bytes"]==270496
    assert hashlib.sha256(pack).hexdigest()==report["pack_sha256"]
    assert pack[:32]==struct.pack("<4s3I16x",b"DNS1",len(pack),report["fill_blue_argb"],report["fill_ground_argb"])
    for i in range(4):
        assert pack[32+i*67616:32+(i+1)*67616]==(prepared/f"sky_{i}.dt").read_bytes()
    print("PASS: four exact source slices/clamped padding, VQ16 bytes/previews and VRAM accounting")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    check(Path(sys.argv[1]), Path(sys.argv[2]))
