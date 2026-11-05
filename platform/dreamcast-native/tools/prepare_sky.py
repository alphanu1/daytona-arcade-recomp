"""Prepare four VQ16 sky pages from the independently checked panorama of a course (392 rows on course 0, 344
on course 2: the height of export_sky's panorama.tga).

python3 platform/dreamcast-native/tools/prepare_sky.py SOURCES/panorama.tga NEW_OUTPUT
Local ROM-derived candidates only: no live camera binding or runtime pack.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import struct

from prepare_scene import read_tga, tga, dt_info
from check_materials import decode_vq16


def prepare(source, output):
    encoder = shutil.which("pvrtex")
    if not encoder:
        raise ValueError("pvrtex missing; source enDjinn/environ.sh first")
    rows = struct.unpack_from("<H", source.read_bytes(), 14)[0]
    if rows % 8 or not 8 <= rows <= 512:
        raise ValueError("sky panorama height must be whole tile rows up to 512")
    pixels = read_tga(source, 2048, rows)
    if any(p[3] != 255 for p in pixels):
        raise ValueError("sky source must be opaque")
    # export_sky proves these authored first/last tiles supply the uniform
    # vertical fill. Keep exact24-bit colours, separate from lossy VQ pages.
    fill = [pixels[0], pixels[(rows-8)*2048]]
    for color, first_y in zip(fill, (0, rows-8)):
        if any(pixels[y*2048+x] != color for y in range(first_y, first_y+8) for x in range(8)):
            raise ValueError("sky fill tile is not uniform")
    output.mkdir()  # Never overwrite a previous asset set.
    entries = []
    for page in range(4):
        # Clamp padding, not wrap; only rows 0..rows-1 are source content.
        part = [pixels[min(y, rows-1)*2048+page*512+x]
                for y in range(512) for x in range(512)]
        path = output / f"sky_{page}.tga"
        path.write_bytes(tga(512, 512, part))
        target = path.with_suffix(".dt")
        result = subprocess.run([encoder, "-i", str(path), "-o", str(target),
                                 "-f", "RGB565", "-c", "small", "-d", "0",
                                 "-p", str(path.with_suffix(".png"))],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"pvrtex failed: {result.stdout}\n{result.stderr}")
        info = dt_info(target, 512, 512, 1, True)
        width, height, decoded = decode_vq16(target.read_bytes())
        if (width, height) != (512, 512) or any(a != 255 for a in decoded[3::4]):
            raise ValueError("encoded sky dimensions/alpha differ")
        error_sum = error_max = 0
        for i, pixel in enumerate(part[:rows*512]):
            for c in range(3):
                error = abs(pixel[c]-decoded[4*i+c])
                error_sum += error
                error_max = max(error_max, error)
        entries.append(dict(file=target.name, source_x=page*512, source_y=0,
                            width=512, height=512, content_height=rows,
                            format="RGB565", compressed=True,
                            rgb_mean_absolute_error=error_sum/(rows*512*3),
                            rgb_max_absolute_error=error_max, **info))
    report = dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  coverage="authored panorama band of the course; camera/fill/runtime not integrated",
                  fill_blue_argb=int.from_bytes(fill[0], "little"),
                  fill_ground_argb=int.from_bytes(fill[1], "little"),
                  textures=entries, palette_banks=0,
                  vram_payload_bytes=sum(e["vram_payload_bytes"] for e in entries),
                  kos_allocation_bytes=sum(e["vram_payload_bytes"]+32 for e in entries))
    pages=[(output / e["file"]).read_bytes() for e in entries]
    if any(len(p)!=67616 or p[:32]!=bytes.fromhex("44635478200801000000ff000002000236040048000000000000000000000000") for p in pages):
        raise ValueError("DNS1 requires full-codebook512 RGB565 VQ pages")
    # Header halfword 16: the content rows, 0 for course 0's 392 (include/render/scene/sky.h).
    pack=struct.pack("<4s3IH14x",b"DNS1",32+4*67616,report["fill_blue_argb"],report["fill_ground_argb"],
                     0 if rows == 392 else rows)+b"".join(pages)
    (output / "sky.dns").write_bytes(pack)
    report.update(pack_bytes=len(pack),pack_sha256=hashlib.sha256(pack).hexdigest())
    (output / "manifest.json").write_text(json.dumps(report, indent=2)+"\n")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare(args.source, args.output), indent=2))
