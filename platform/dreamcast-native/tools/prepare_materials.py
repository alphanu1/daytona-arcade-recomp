"""DNT1/DNT2 bounded native metadata + consecutive VQ/raw16 DcTx blobs.

python3 platform/dreamcast-native/tools/prepare_materials.py SOURCE_MODELS PREPARED_VQ NEW_FILE
ROM-derived output is local/ignored only. Source proofs stay on the host.
--crops NEW_CANDIDATES revalidates exact crop bytes/UV coverage and writes DNT2.
--reachable-lights removes only keys below every matching material's ambient.
--fast-backing-bytes trades bounded CPU RAM for stored zlib blocks, not new VQ loss.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib
from check_materials import inspect

HEADER = struct.Struct("<4s7I")
RESOURCE = struct.Struct("<IIHHI")
BINDING = struct.Struct("<6HI")


def validate_textures(sources, prepared):
    quality = inspect(sources / "materials", prepared)
    manifest = json.loads((prepared / "manifest.json").read_text())
    entries = manifest["textures"]
    expected = set()
    for line in (sources / "materials/materials.txt").read_text().splitlines():
        if line.strip() and not line.lstrip().startswith("#"):
            values = line.split()
            if len(values) != 9:
                raise ValueError("invalid source material record")
            expected.add(tuple(values[:2]) + tuple(map(int, values[2:])))
    actual = {(e["source_file"], e["format"], e["width"], e["height"], *e["header"], e["luma"]) for e in entries}
    if actual != expected or len(actual) != len(entries):
        raise ValueError("encoded bindings do not cover source materials exactly")
    for e in entries:
        h0 = e["header"][0]
        if not h0 & 0x4000 or not 0 <= e["luma"] <= 255 or (e["width"], e["height"]) != (32 << (h0 & 7), 32 << ((h0 >> 3) & 7)):
            raise ValueError("invalid textured binding")
    return quality, entries


def reachable_luma_bounds(sources):
    # Reuse the full geometry/material validator, including finite 0..255
    # diffuse/ambient/specular parameters. Lighting only ADDS nonnegative
    # diffuse/specular to ambient, then saturates/truncates to an integer.
    import prepare_models as models
    data, _ = models.prepare(sources)
    h = models.HEADER.unpack_from(data)
    at = models.HEADER.size+h[2]*models.MESH.size+h[3]*models.VERTEX.size+h[4]*models.FACE.size
    bounds = {}
    for i in range(h[5]):
        m = models.MATERIAL.unpack_from(data, at+i*models.MATERIAL.size)
        key = m[:4]
        bounds[key] = min(bounds.get(key, 255), int(m[5]))
    return bounds


def compress_textures(blobs, fast_backing_bytes=None):
    streams = [zlib.compress(blob, 9) for blob in blobs]
    if fast_backing_bytes is None:
        return streams, 0, 0
    if not 0 < fast_backing_bytes <= 9*1024*1024:
        raise ValueError("fast backing budget must be within 9 MiB")
    remaining = fast_backing_bytes - sum((len(s)+31)&~31 for s in streams)
    if remaining < 0:
        raise ValueError("compressed textures exceed fast backing budget")
    stored = [zlib.compress(blob, 0) for blob in blobs]
    extra = [((len(raw)+31)&~31)-((len(old)+31)&~31) for raw,old in zip(stored,streams)]
    # ponytail: greedy decoded bytes per extra RAM byte, not a global knapsack
    # optimum or a route-specific trace. Revisit only with measured decode costs.
    count = decoded_bytes = 0
    for i in sorted(range(len(blobs)), key=lambda i: (extra[i]/len(blobs[i]), i)):
        if extra[i] <= remaining:
            streams[i] = stored[i]
            remaining -= extra[i]
            count += 1
            decoded_bytes += len(blobs[i])
    return streams, count, decoded_bytes


def prepare(sources, prepared, crops=None, cpu_backed=False, reachable_lights=False, deflate=False,
            fast_backing_bytes=None):
    if deflate and (not cpu_backed or crops is None):
        raise ValueError("DNT3 requires CPU backing and validated crop coordinates")
    if fast_backing_bytes is not None and not deflate:
        raise ValueError("fast backing requires DNT3 deflate")
    quality, entries = validate_textures(sources, prepared)
    if crops is not None:
        from prepare_material_crops import validate_crops
        entries, payload = validate_crops(sources, prepared, entries, crops)
        quality = dict(quality, vram_payload_bytes=payload)
        prepared = crops
    bounds = reachable_luma_bounds(sources) if reachable_lights else None
    def keep(header, luma):
        if bounds is None:
            return True
        if tuple(header) not in bounds:
            raise ValueError("material binding has no validated model owner")
        return luma >= bounds[tuple(header)]
    original_bindings = len(entries)
    entries = [e for e in entries if keep(e["header"], e["luma"])]
    unique = {e["file"]: e for e in entries}
    # Recompute after filtering/dedup; the proof/alpha report still describes
    # the complete original asset set, not just the surviving subset.
    quality = dict(quality, vram_payload_bytes=sum((prepared / name).stat().st_size-32 for name in unique))
    if not 1 <= len(unique) <= (4096 if cpu_backed else 2048) or (not deflate and quality["vram_payload_bytes"] > (8 if cpu_backed else 3)*1024*1024):
        raise ValueError("native material residency exceeds texture count/payload guard")
    indices = {name: i for i, name in enumerate(unique)}
    bindings = [BINDING.pack(*e["header"], e["luma"], indices[e["file"]],
        e["crop_origin"][0] | (e["crop_origin"][1]<<16) if crops is not None else 0xffffffff) for e in entries]
    for line in (sources / "solid_materials.txt").read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        values = list(map(int, line.split()))
        if len(values) != 6 or values[0] & 0x4000 or not 0 <= values[4] <= 255 or values[5] >> 24 != 255:
            raise ValueError("invalid solid material")
        original_bindings += 1
        if keep(values[:4], values[4]):
            bindings.append(BINDING.pack(*values[:5], 65535, values[5]))
    bindings.sort(key=lambda b: b[:10])
    if not 1 <= len(bindings) <= 8192 or any(a[:10] == b[:10] for a,b in zip(bindings, bindings[1:])):
        raise ValueError("invalid/duplicate material bindings")
    data = bytearray(32 + len(unique)*16)
    data += b"".join(bindings)
    data += bytes(-len(data) % 32)
    backing_start = len(data)
    staging = 0
    blobs = []
    for name, entry in unique.items():
        dt = (prepared / name).read_bytes()
        if not 64 <= len(dt) <= 264224:
            raise ValueError("texture exceeds native staging limit")
        if dt[9] or hashlib.sha256(dt).hexdigest() != entry["sha256"]:
            raise ValueError("changed texture or unsupported extended DcTx metadata")
        blobs.append(dt)
    streams, stored_count, stored_bytes = compress_textures(blobs, fast_backing_bytes) if deflate else (blobs, 0, 0)
    for i, (dt, stored, entry) in enumerate(zip(blobs, streams, unique.values())):
        RESOURCE.pack_into(data, 32+i*16, len(data), len(dt), entry["width"], entry["height"], len(stored) if deflate else 0)
        data += stored
        data += bytes(-len(data) % 32)
        staging = max(staging, len(dt))
    if len(data) > (10 if cpu_backed else 4)*1024*1024:
        raise ValueError("native material file exceeds selected residency guard")
    if deflate and len(data)-backing_start > (fast_backing_bytes or 8*1024*1024):
        raise ValueError("compressed backing exceeds selected main-RAM guard")
    magic = b"DNT3" if deflate else b"DNT2" if crops is not None else b"DNT1"
    HEADER.pack_into(data, 0, magic, len(data), len(unique), len(bindings), quality["vram_payload_bytes"], staging, 0, 0)
    return data, dict(bytes=len(data), textures=len(unique), bindings=len(bindings),
        format=magic.decode(),
        stored_block_resources=stored_count, stored_block_decoded_bytes=stored_bytes,
        unreachable_bindings_removed=original_bindings-len(bindings),
        residency="CPU-backed variants; runtime VRAM cache required" if cpu_backed else "all textures resident in VRAM",
        metadata_bytes=(len(unique)+len(bindings))*16, staging_bytes=staging,
        backing_bytes=len(data)-backing_start,
        vram_payload_bytes=quality["vram_payload_bytes"], alpha_changed_pixels=quality["alpha_changed_pixels"],
        alpha_quality_scope="original uncropped texture assets against source",
        sha256=hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", type=Path)
    parser.add_argument("prepared", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--crops", type=Path, help="validate exact crop candidates against PREPARED_VQ and emit DNT2")
    parser.add_argument("--cpu-backed", action="store_true", help="bounded main-RAM variants (8 MiB default); NOT a larger VRAM budget")
    parser.add_argument("--reachable-lights", action="store_true", help="validate model materials and omit impossible sub-ambient lumas")
    parser.add_argument("--deflate", action="store_true", help="DNT3: independently zlib-compress exact DcTx blobs in CPU backing; GPU VQ16 unchanged")
    parser.add_argument("--fast-backing-bytes", type=int, help="DNT3: spend up to this CPU backing budget (max 9 MiB) on faster stored blocks")
    args = parser.parse_args()
    data, report = prepare(args.sources, args.prepared, args.crops, args.cpu_backed, args.reachable_lights, args.deflate,
                           args.fast_backing_bytes)
    with args.output.open("xb") as out:
        out.write(data)
    print(json.dumps(report, indent=2))
