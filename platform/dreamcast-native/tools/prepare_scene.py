"""Prepare one exported 2D layout; source art, never flattened screenshots.

python3 platform/dreamcast-native/tools/prepare_scene.py SOURCES NEW_OUTPUT
Outputs contain ROM-derived data and must stay in ignored local directories.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_dreamcast_textures import dt_info, exact_palette


def tga(w, h, pixels):
    if len(pixels) != w * h:
        raise ValueError("pixel count differs from dimensions")
    return (b"\0\0\2" + bytes(9) + struct.pack("<HHBB", w, h, 32, 0x28)
            + b"".join(pixels))


def read_tga(path, w, h):
    data = path.read_bytes()
    if (len(data) != 18 + w * h * 4 or data[:12] != b"\0\0\2" + bytes(9)
            or data[12:18] != struct.pack("<HHBB", w, h, 32, 0x28)):
        raise ValueError(f"unexpected TGA source: {path}")
    return [data[i:i + 4] for i in range(18, len(data), 4)]


def chunks(pixels, opaque, x=0, y=0, size=512):
    part = [pixels[row * 512 + col] for row in range(y, y + size)
            for col in range(x, x + size)]
    if not opaque:
        part = [p if p[3] else bytes(4) for p in part]
        if not any(p[3] for p in part):
            return
    if size > (256 if opaque else 128) or (not opaque and len(set(part)) > 16):
        if size <= 8:
            raise ValueError("even an 8x8 tile exceeds exact PAL4")
        for dy in (0, size // 2):
            for dx in (0, size // 2):
                yield from chunks(pixels, opaque, x + dx, y + dy, size // 2)
    else:
        yield x, y, size, part


def intersection(rect, chunk):
    layer, opaque, x, y, sx, sy, w, h = rect
    cx, cy, size, _ = chunk
    left, top = max(sx, cx), max(sy, cy)
    right, bottom = min(sx + w, cx + size), min(sy + h, cy + size)
    if left >= right or top >= bottom:
        return None
    return x + left - sx, y + top - sy, right - left, bottom - top, left - cx, top - cy


def trim_alpha(clip, size, pixels):
    x, y, w, h, u, v = clip
    left, top, right, bottom = w, h, 0, 0
    for dy in range(h):
        for dx in range(w):
            if pixels[(v + dy) * size + u + dx][3]:
                left, top = min(left, dx), min(top, dy)
                right, bottom = max(right, dx + 1), max(bottom, dy + 1)
    if left >= right:
        return None
    return x + left, y + top, right - left, bottom - top, u + left, v + top


def check_pal4(data, palette, pixels, size, height=None):
    """Decode actual twiddled PAL4 bytes, not the encoder's source preview."""
    start = (data[9] + 1) * 32
    height = size if height is None else height
    if any(n < 8 or n & (n-1) for n in (size,height)) or len(pixels) != size*height:
        raise ValueError('invalid PAL4 dimensions/pixel count')
    side = min(size,height)
    colors = [palette[i:i + 4] for i in range(8, len(palette), 4)]
    def spread(v):
        return sum(((v >> b) & 1) << (2 * b) for b in range(side.bit_length() - 1))
    for y in range(height):
        for x in range(size):
            index = (x//side+y//side)*side*side + (spread(y) | (spread(x) << 1))
            pen = (data[start + index // 2] >> ((index & 1) * 4)) & 15
            if pen >= len(colors) or colors[pen] != pixels[y * size + x]:
                raise ValueError("encoded PAL4 texel differs from exact source")


def title_motion(program):
    if len(program) != 2 * 1024 * 1024:
        raise ValueError("expected daytona93 2 MiB program image")
    # Guest 0x0023ad8c aliases program offset 0x3ad8c. Signed increments,
    # terminated by 100; store only the bounded, integrated animation curve.
    motion = [-512, -512] + list(range(-448, 0, 64))
    for step in struct.unpack_from("<128h", program, 0x3ad8c):
        if step == 100:
            if len(motion) != 44 or motion[-1] != 0:
                raise ValueError("unexpected title curve length or endpoint")
            return motion
        if not -64 <= step <= 64:
            raise ValueError("invalid title displacement")
        motion.append(motion[-1] + step)
    raise ValueError("unterminated title curve")


def prepare(sources, output, title_program=None):
    encoder = shutil.which("pvrtex")
    if not encoder:
        raise ValueError("source enDjinn/environ.sh first (pvrtex missing)")
    rects = []
    for line in (sources / "rectangles.txt").read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        r = tuple(map(int, line.split()))
        if (len(r) != 8 or r[0] not in range(4) or r[1] not in (0, 1) or
                min(r[2:]) < 0 or not r[6] or not r[7] or
                r[2] + r[6] > 496 or r[3] + r[7] > 384 or
                r[4] + r[6] > 512 or r[5] + r[7] > 512):
            raise ValueError("invalid source rectangle")
        rects.append(r)
    if not 1 <= len(rects) <= 1024:
        raise ValueError("expected 1..1024 source rectangles")
    motion, roles = None, [0] * len(rects)
    if title_program:
        program = title_program.read_bytes()
        motion = title_motion(program)
        # Semantic source regions include the offscreen edge needed by motion.
        rects = [(2, 1, 0, 0, 0, 0, 496, 384),
                 (0, 0, 0, 0, 0, 0, 512, 304),
                 (0, 0, 96, 304, 96, 304, 304, 24),
                 (0, 0, 0, 328, 0, 328, 512, 56)]
        roles = [0, 1, 2, 3]
    output.mkdir()  # Never overwrite an existing asset directory.
    assets, quads, unique, palettes, planes = [], [], {}, {}, {}
    reconstructed = [bytes((0, 0, 0, 255))] * (496 * 384)
    for order, rect in enumerate(rects):
        layer, opaque = rect[:2]
        if (layer, opaque) not in planes:
            name = f"layer_{layer}_{'opaque' if opaque else 'alpha'}.tga"
            planes[layer, opaque] = list(chunks(read_tga(sources / name, 512, 512), opaque))
        for chunk in planes[layer, opaque]:
            clip = intersection(rect, chunk)
            if clip is None:
                continue
            cx, cy, size, pixels = chunk
            if not opaque:
                clip = trim_alpha(clip, size, pixels)
                if clip is None:
                    continue
            source = tga(size, size, pixels)
            key = (opaque, source)
            if key not in unique:
                i = len(assets)
                texture = output / f"texture_{i}.dt"
                src = texture.with_suffix(".tga")
                src.write_bytes(source)
                cmd = [encoder, "-i", str(src), "-o", str(texture), "-d", "0",
                       "-f", "RGB565" if opaque else "PAL4BPP"]
                bank, palette_name = 0, None
                if opaque:
                    cmd += ["-c", "small"]
                else:
                    pal = exact_palette(source, size, size)
                    bank = palettes.setdefault(pal, len(palettes))
                    if bank >= 64:
                        raise ValueError("scene exceeds 64 PAL4 banks")
                    palette_name = f"palette_{bank}.pal"
                    (output / palette_name).write_bytes(pal)
                    cmd += ["--max-color", "16", "--palette", str(output / palette_name)]
                subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                info = dt_info(texture, size, size, 1 if opaque else 5, bool(opaque))
                if not opaque:
                    check_pal4(texture.read_bytes(), pal, pixels, size)
                assets.append(dict(file=texture.name, palette=palette_name, bank=bank,
                                   opaque=opaque, size=size, **info))
                unique[key] = i
            i = unique[key]
            x, y, w, h, u, v = clip
            quads.append((i, x, y, w, h, u, v, order + 1, roles[order]))
            for dy in range(h):
                for dx in range(w):
                    pixel = pixels[(v + dy) * size + u + dx]
                    if pixel[3] and x + dx < 496:
                        reconstructed[(y + dy) * 496 + x + dx] = pixel
    if reconstructed != read_tga(sources / "reference.tga", 496, 384):
        raise ValueError("split source geometry does not reconstruct the reference")
    payload = sum(a["vram_payload_bytes"] for a in assets)
    if payload > 1024 * 1024 or not 1 <= len(assets) <= 128 or not 1 <= len(quads) <= 1024:
        raise ValueError("static 2D scene exceeds its initial bounded budget")
    header = ["/* Local ROM-derived build output. Do not distribute. */"]
    if motion:
        header += ["#define NATIVE_TITLE 1", "static const int16_t title_motion[] = {",
                   ",".join(map(str, motion)), "};"]
        (output / "title_motion.bin").write_bytes(struct.pack(f"<{len(motion)}h", *motion))
    for i, a in enumerate(assets):
        header += [f"alignas(32) static const uint8_t scene_texture_{i}[] = {{",
                   f'#embed {json.dumps(str((output / a["file"]).resolve()))}', "};"]
    for pal, bank in palettes.items():
        header += [f"alignas(4) static const uint8_t scene_palette_{bank}[] = {{",
                   f'#embed {json.dumps(str((output / f"palette_{bank}.pal").resolve()))}', "};"]
    header += ["static const scene_asset_t scene_assets[] = {"]
    for i, a in enumerate(assets):
        palette = f'scene_palette_{a["bank"]}' if a["palette"] else "NULL"
        header.append(f'{{scene_texture_{i}, {palette}, {a["bank"]}, {a["opaque"]}}},')
    header += ["};", "static const scene_quad_t scene_quads[] = {"]
    header += ["{" + ",".join(map(str, q)) + "}," for q in quads]
    header += ["};"]
    (output / "native_scene.inc").write_text("\n".join(header) + "\n")
    report = dict(textures=len(assets), quads=len(quads), palette_banks=len(palettes),
                  vram_payload_bytes=payload, ta_upper_bound_bytes=len(quads) * 160,
                  source_reconstruction="exact", pal4_encoded_texels="exact",
                  coverage="one static 2D layout, not title timing or gameplay", assets=assets)
    if motion:
        report.update(coverage="title intro and no-credit blink; no scene exit or gameplay",
                      motion_samples=len(motion), program_sha256=hashlib.sha256(program).hexdigest(),
                      motion_program_offset=0x3ad8c, reference_tick_us=17384)
    (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {len(assets)} textures, {len(quads)} quads, {len(palettes)} PAL4 banks; "
          f"{payload} bytes VRAM payload; exact source reconstruction and PAL4 texels")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--title-program", type=Path,
                        help="prepare daytona93 title roles and authored ROM motion")
    args = parser.parse_args()
    prepare(args.sources, args.output, args.title_program)
