#!/usr/bin/env python3
"""Encode m2segment's local texture sources with enDjinn's installed pvrtex.

Output is ROM-derived: keep both directories under ignored build/staging paths.
This prepares observed base-level materials, not a complete stage or gameplay.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess


def dt_info(path, width, height, pixel_format, compressed):
    data = path.read_bytes()
    if len(data) < 32 or data[:4] != b"DcTx" or data[8] != 0:
        raise ValueError(f"invalid DcTx header: {path}")
    size = struct.unpack_from("<I", data, 4)[0]
    w, h, mode = struct.unpack_from("<HHI", data, 12)
    header_bytes = (data[9] + 1) * 32
    if (size != len(data) or size % 32 or header_bytes >= size or
            (w, h) != (width, height) or ((mode >> 27) & 7) != pixel_format or
            bool(mode & (1 << 30)) != compressed or mode & (1 << 31)):
        raise ValueError(f"unexpected DcTx layout/format: {path}")
    return {"file_bytes": size, "vram_payload_bytes": size - header_bytes,
            "pvr_mode": mode, "sha256": hashlib.sha256(data).hexdigest()}


def exact_palette(tga, width, height):
    # m2segment writes only uncompressed, top-origin BGRA32 TGA sources.
    if (len(tga) != 18 + width * height * 4 or tga[:3] != b"\0\0\2" or
            struct.unpack_from("<HHBB", tga, 12) != (width, height, 32, 0x28)):
        raise ValueError("expected m2segment's uncompressed BGRA32 source")
    colors = sorted({tga[i:i + 4] for i in range(18, len(tga), 4)})
    if len(colors) > 16:
        raise ValueError("exact PAL4 source contains more than 16 colors")
    return b"DPAL" + struct.pack("<I", len(colors)) + b"".join(colors)


def material_blob(entries, sources, resource_count):
    """DTM1 records: exact header/luma, resource, source indices, 16 colours."""
    records = sorted(entries, key=lambda e: struct.pack("<5H", *e["header"], e["luma"]))
    data = bytearray(32 + 96 * len(records))
    indices, previous = {}, None
    for i, entry in enumerate(records):
        key = struct.pack("<5H", *entry["header"], entry["luma"])
        if key == previous:
            raise ValueError("duplicate material key")
        previous = key
        w, h = entry["width"], entry["height"]
        name = entry["source_file"]
        if Path(name).name != name:
            raise ValueError("invalid material source filename")
        tga = (sources / name).read_bytes()
        state = (sources / Path(name).with_suffix(".m2t")).read_bytes()
        if (hashlib.sha256(tga).hexdigest() != entry["source_sha256"] or
                len(state) != 64 + w * h // 2 or len(tga) != 18 + w * h * 4):
            raise ValueError("material state/source mismatch")
        # Verify the proof describes the exact image handed to the VQ encoder.
        colors = [state[j:j + 4] for j in range(0, 64, 4)]
        pos = 64
        for y in range(0, h, 2):
            for x in range(0, w, 2):
                cell = int.from_bytes(state[pos:pos + 2], "little")
                pos += 2
                for n in range(4):
                    pixel = 18 + ((y + n // 2) * w + x + n % 2) * 4
                    if tga[pixel:pixel + 4] != colors[(cell >> (n * 4)) & 15]:
                        raise ValueError("material proof differs from source pixels")
        source = state[64:]
        if source not in indices:
            indices[source] = len(data)
            data += source
        at = 32 + i * 96
        struct.pack_into("<5H H 5I", data, at, *entry["header"], entry["luma"], 0,
                         entry["pack_index"], indices[source], len(source), 0, 0)
        data[at + 32:at + 96] = state[:64]
    struct.pack_into("<4s7I", data, 0, b"DTM1", len(data), len(records), resource_count, 96, 0, 0, 0)
    return data


def write_pack(directory, entries, sources=None):
    """DTP1: 32-byte LE header/table records, aligned DcTx/DPAL blobs.

    DTP2 additionally embeds exact material proofs. DTP1 remains inspection-only.
    """
    unique = {entry["file"]: entry for entry in entries}
    if not unique or len(unique) > 2048:
        raise ValueError("texture pack needs 1..2048 unique textures")
    data = bytearray(32 + 32 * len(unique))
    palettes, vram_bytes = {}, 0
    for index, (name, entry) in enumerate(unique.items()):
        if Path(name).name != name:
            raise ValueError("invalid texture filename")
        dt = (directory / name).read_bytes()
        if hashlib.sha256(dt).hexdigest() != entry["sha256"]:
            raise ValueError(f"texture changed after encoding: {name}")
        offset = len(data)
        data += dt
        pal_offset = pal_size = 0
        bank = 0xffffffff
        if "palette_file" in entry:
            pal_name = entry["palette_file"]
            bank = entry["palette_bank"]
            if Path(pal_name).name != pal_name or not 0 <= bank < 64:
                raise ValueError("palette filename or bank out of range")
            pal = (directory / pal_name).read_bytes()
            if (len(pal) < 12 or pal[:4] != b"DPAL" or
                    not 1 <= struct.unpack_from("<I", pal, 4)[0] <= 16 or
                    len(pal) != 8 + 4 * struct.unpack_from("<I", pal, 4)[0]):
                raise ValueError("invalid PAL4 palette")
            if bank in palettes:
                pal_offset, pal_size = palettes[bank]
                if data[pal_offset:pal_offset + pal_size] != pal:
                    raise ValueError("different palettes assigned to the same bank")
            else:
                pal_offset, pal_size = len(data), len(pal)
                data += pal
                data += bytes((-len(data)) % 32)
                palettes[bank] = (pal_offset, pal_size)
        vram_bytes += entry["vram_payload_bytes"]
        entry["pack_index"] = index
        struct.pack_into("<8I", data, 32 + index * 32, offset, len(dt), pal_offset,
                         pal_size, bank, entry["width"], entry["height"], 0)
    # Propagate resource indices to material mappings sharing one texture.
    for entry in entries:
        entry["pack_index"] = unique[entry["file"]]["pack_index"]
    material_offset = material_size = 0
    if sources is not None:
        materials = material_blob(entries, sources, len(unique))
        material_offset, material_size = len(data), len(materials)
        data += materials
    if len(data) > 3 * 1024 * 1024 or vram_bytes > 1498368:
        raise ValueError("texture pack exceeds the current Dreamcast loading/PVR budget")
    struct.pack_into("<4s7I", data, 0, b"DTP2" if sources is not None else b"DTP1",
                     len(data), len(unique), vram_bytes, 32, material_offset, material_size, 0)
    with (directory / "textures.dtp").open("xb") as out:
        out.write(data)
    return len(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", type=Path)
    parser.add_argument("output", type=Path, help="new output directory (never overwritten)")
    parser.add_argument("--format", choices=("vq", "pal4", "pal4-vq", "mixed"), default="vq")
    parser.add_argument("--palette-banks", type=int, default=48,
                        help="mixed-format bank limit, default 48 leaves 16 for UI")
    args = parser.parse_args()
    encoder = shutil.which("pvrtex")
    if not encoder:
        parser.error("pvrtex is missing; source enDjinn/environ.sh first")
    if not 0 <= args.palette_banks <= 64:
        parser.error("palette bank limit must be 0..64")
    materials = []
    for line in (args.sources / "materials.txt").read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        name, fmt, *numbers = line.split()
        width, height, *key = map(int, numbers)
        if (Path(name).name != name or not name.endswith(".tga") or
                fmt not in ("RGB565", "ARGB1555") or len(key) != 5 or
                any(n < 8 or n > 1024 or n & (n - 1) for n in (width, height)) or
                any(n < 0 or n > 65535 for n in key[:4]) or not 0 <= key[4] <= 255):
            raise ValueError("invalid material manifest row")
        if not (args.sources / name).is_file():
            raise ValueError(f"missing source image: {name}")
        if not (args.sources / Path(name).with_suffix(".m2t")).is_file():
            raise ValueError(f"missing exact material state for {name}; re-export sources with the current m2segment")
        materials.append((name, fmt, width, height, key))
    if not materials or len({m[0] for m in materials}) != len(materials):
        raise ValueError("empty or duplicate material manifest")
    if args.format == "mixed":
        # ponytail: largest images get scarce palette banks first; use actual
        # encoded savings per palette group if this heuristic misses the budget.
        materials.sort(key=lambda m: -(m[2] * m[3]))
    args.output.mkdir()  # fail on existing directory; never replace user assets
    entries, encoded, palettes = [], {}, {}
    for name, fmt, width, height, key in materials:
        source_data = (args.sources / name).read_bytes()
        source_hash = hashlib.sha256(source_data).hexdigest()
        identity = (source_hash, fmt)
        if identity in encoded:
            entries.append({**encoded[identity], "header": key[:4], "luma": key[4], "source_file": name})
            continue
        target = args.output / (Path(name).stem + ".dt")
        palette = args.format.startswith("pal4")
        pal_data = None
        if palette or args.format == "mixed":
            pal_data = exact_palette(source_data, width, height)
            pal_hash = hashlib.sha256(pal_data).hexdigest()
            palette = palette or pal_hash in palettes or len(palettes) < args.palette_banks
        compressed = args.format != "pal4"
        command = [encoder, "-i", str(args.sources / name), "-o", str(target),
                   "-f", "PAL4BPP" if palette else fmt, "-d", "0",
                   "-p", str(target.with_suffix(".png"))]
        if palette:
            pal = Path(str(target) + ".pal")
            pal.write_bytes(pal_data)
            palettes.setdefault(pal_hash, len(palettes))
            command += ["--max-color", "16", "--palette", str(pal)]
        if compressed:
            command += ["-c", "small"]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if result.returncode:
            raise RuntimeError(f"pvrtex failed on {name}: {result.stdout}\n{result.stderr}")
        entry = {"file": target.name, "source_file": name, "width": width, "height": height,
                 "header": key[:4], "luma": key[4], "format": "PAL4BPP" if palette else fmt,
                 "compressed": compressed, "source_sha256": source_hash,
                 **dt_info(target, width, height, 5 if palette else int(fmt == "RGB565"), compressed)}
        if palette:
            pal = Path(str(target) + ".pal")
            entry["palette_file"] = pal.name
            entry["palette_file_bytes"] = pal.stat().st_size
            entry["palette_bank"] = palettes[pal_hash]
            entry["palette_format"] = "ARGB8888"
        entries.append(entry)
        encoded[identity] = entry
    report = {"version": 1, "coverage": "supplied header/luma variants only; see source export for coverage",
              "levels": "base only; original mipmaps and microtextures not prepared",
              "lighting": "baked per exact header/luma; not reusable under arbitrary material state",
              "textures": entries,
              "unique_textures": len(encoded),
              "format_policy": args.format,
              "palette_banks_16_entries": len(palettes),
              "palette_banks_fit_hardware": len(palettes) <= 64,
              "vram_payload_bytes": sum(e["vram_payload_bytes"] for e in encoded.values())}
    if len(palettes) <= 64 and report["vram_payload_bytes"] <= 1498368:
        report["pack_file"] = "textures.dtp"
        report["pack_bytes"] = write_pack(args.output, entries, args.sources)
    (args.output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Prepared {len(entries)} materials / {len(encoded)} unique textures; "
          f"{report['vram_payload_bytes']} bytes PVR payload. "
          f"{len(palettes)}/64 palette banks (before reserving UI banks). "
          "Allocator overhead is separate. Local assets: do not distribute.")


if __name__ == "__main__":
    main()
