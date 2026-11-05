"""Inspect actual base-level VQ/raw16 bytes and compare pixels with sources.

python3 platform/dreamcast-native/tools/check_materials.py SOURCES PREPARED [--previews]
--previews additionally cross-checks pvrtex PNGs using installed Pillow.
Reads only; emitted metrics are not permission to ignore visible errors.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_dreamcast_textures import material_blob


def decode_texture16(data):
    if len(data) < 32 or data[:4] != b"DcTx" or data[8] != 0:
        raise ValueError("invalid DcTx header")
    size = struct.unpack_from("<I", data, 4)[0]
    w, h, mode = struct.unpack_from("<HHI", data, 12)
    start, entries = (data[9] + 1) * 32, data[10] + 1
    compressed = bool(mode & (1 << 30))
    mipmapped = bool(mode & (1 << 31))
    if mipmapped and (not compressed or entries != 256 or w != h):
        raise ValueError("mipmaps require square full-codebook VQ16")
    index_count = (w * h - 1) // 3 + 1 if mipmapped else w * h // 4
    payload = entries * 8 + index_count if compressed else w * h * 2
    if (size != len(data) or size % 32
            or mode & ~0xc80007ff or (w, h) != (8 << ((mode >> 3) & 7), 8 << (mode & 7))
            or (not compressed and data[10]) or (start + payload + 31) // 32 * 32 != size):
        raise ValueError("expected bounded twiddled RGB565/ARGB1555 VQ/raw")
    words = struct.unpack_from(f"<{entries * 4 if compressed else w * h}H", data, start)
    pixels = []
    for word in words:
        b = word & 31
        g = (word >> 5) & (63 if mode & (1 << 27) else 31)
        r = (word >> (11 if mode & (1 << 27) else 10)) & 31
        pixels.append(bytes(((b << 3) | (b >> 2),
            ((g << 2) | (g >> 4)) if mode & (1 << 27) else ((g << 3) | (g >> 2)),
            (r << 3) | (r >> 2), 255 if mode & (1 << 27) or word & 0x8000 else 0)))
    base_start = start + entries * 8 + index_count - w * h // 4
    indices = data[base_start:base_start + w * h // 4] if compressed else b""
    base = 256 - entries  # Small codebooks occupy the tail of the hardware book.
    if compressed and any(index < base for index in indices):
        raise ValueError("VQ index refers to missing codebook prefix")
    side = min(w, h)
    spread = [sum(((v >> bit) & 1) << (bit * 2) for bit in range(side.bit_length() - 1))
              for v in range(side)]
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            twiddle = (x // side + y // side) * side * side + spread[y % side] + 2 * spread[x % side]
            pixel = pixels[(indices[twiddle // 4] - base) * 4 + twiddle % 4 if compressed else twiddle]
            at = (y * w + x) * 4
            out[at:at + 4] = pixel
    return w, h, bytes(out)


def decode_vq16(data):
    """VQ-only contract for codebook-preserving cropping and legacy callers."""
    if (len(data) < 32 or not struct.unpack_from("<I", data, 16)[0] & (1 << 30)
            or struct.unpack_from("<I", data, 16)[0] & (1 << 31)):
        raise ValueError("expected base-level VQ16 texture")
    return decode_texture16(data)


def inspect(sources, prepared, previews=False):
    report = json.loads((prepared / "manifest.json").read_text())
    unique = {entry["file"]: entry for entry in report["textures"]}
    for entry in report["textures"]:
        if any(entry[key] != unique[entry["file"]][key]
               for key in ("sha256", "width", "height", "format", "compressed")):
            raise ValueError("aliased texture metadata disagrees")
    indices = {name: i for i, name in enumerate(unique)}
    # Reuse the existing exact source proof, including aliased material entries.
    proof = material_blob([{**entry, "pack_index": indices[entry["file"]]} for entry in report["textures"]],
                          sources, len(unique))
    payload = changed_alpha = alpha_pixels = rgb_error = rgb_samples = max_error = 0
    alpha_files = []
    for name, entry in unique.items():
        if Path(name).name != name or Path(entry["source_file"]).name != entry["source_file"]:
            raise ValueError("invalid material filename")
        data = (prepared / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise ValueError("encoded file changed")
        try:
            w, h, decoded = decode_texture16(data)
        except ValueError as error:
            raise ValueError(f"{name}: {error}") from error
        fmt = "RGB565" if struct.unpack_from("<I", data, 16)[0] & (1 << 27) else "ARGB1555"
        compressed = bool(struct.unpack_from("<I", data, 16)[0] & (1 << 30))
        if ((w, h) != (entry["width"], entry["height"]) or entry["format"] != fmt
                or entry["compressed"] != compressed):
            raise ValueError("encoded format disagrees with manifest")
        source = (sources / entry["source_file"]).read_bytes()
        if (hashlib.sha256(source).hexdigest() != entry["source_sha256"] or
                len(source) != 18 + w*h*4 or source[:12] != b"\0\0\2" + bytes(9) or
                source[12:18] != struct.pack("<HHBB", w, h, 32, 0x28)):
            raise ValueError("source metadata/hash mismatch")
        if previews:
            from PIL import Image
            with Image.open(prepared / Path(name).with_suffix(".png")) as image:
                if image.size != (w, h) or image.convert("RGBA").tobytes("raw", "BGRA") != decoded:
                    raise ValueError(f"actual texture bytes disagree with pvrtex preview: {name}")
        errors = sum(bool(a) != bool(b) for a, b in zip(source[21::4], decoded[3::4]))
        if entry["format"] == "ARGB1555":
            alpha_pixels += w*h
            if errors: alpha_files.append(dict(file=name, changed_pixels=errors, pixels=w*h))
        elif errors:
            raise ValueError("opaque texture decoded with transparency")
        changed_alpha += errors
        for at in range(0, len(decoded), 4):
            if not source[18 + at + 3]:
                continue  # Invisible source RGB must not inflate the quality metric.
            for c in range(3):
                error = abs(source[18 + at + c] - decoded[at + c])
                rgb_error += error; rgb_samples += 1; max_error = max(max_error, error)
        payload += len(data) - (data[9] + 1) * 32
    if payload != report["vram_payload_bytes"]:
        raise ValueError("manifest payload budget disagrees with files")
    return dict(materials=len(report["textures"]), textures=len(unique), vram_payload_bytes=payload,
                exact_source_proof_bytes=len(proof),
                alpha_pixels=alpha_pixels, alpha_changed_pixels=changed_alpha,
                alpha_changed_files=alpha_files, rgb_mean_absolute_error=rgb_error/max(1, rgb_samples),
                rgb_max_absolute_error=max_error, pvrtex_previews_checked=previews)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", type=Path)
    parser.add_argument("prepared", type=Path)
    parser.add_argument("--previews", action="store_true")
    args = parser.parse_args()
    print(json.dumps(inspect(args.sources, args.prepared, args.previews), indent=2))
