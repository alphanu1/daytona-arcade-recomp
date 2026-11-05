"""Offline crop candidates, not a target material pack.

Retain existing VQ codebooks/indices or raw16 words: decoded texels stay
bit-identical to the original assets. Original VQ loss remains. Native sampling
still require target integration/verification before these can replace DNT1.
Called by inspect_course --cell-assets ... --crop-output NEW_DIRECTORY, or:
python3 prepare_material_crops.py SOURCE_MODELS PREPARED_VQ NEW_DIRECTORY
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from check_materials import decode_texture16
from prepare_models import rows


def crop_axis(low, high, size):
    # Source UVs use 8 units/texel. One texel of halo on each side; only
    # interior regions qualify. Wrapped/mirrored period crossings retain the
    # full axis until their addressing is explicitly implemented and checked.
    if not 0 <= low <= high <= 65535 or size < 8 or size & (size-1):
        raise ValueError("invalid UV interval or texture size")
    first, last = low//8-1, high//8+1
    if first < 0 or last >= size:
        return 0, size
    first &= ~1  # VQ codewords cover aligned 2x2 texels.
    extent = max(8, 1 << (last-first).bit_length())
    if extent >= size:
        return 0, size
    return min(first, size-extent), extent


def block_index(x, y, width, height):
    side = min(width, height)
    index = (x//side + y//side)*side*side
    for bit in range(side.bit_length()-1):
        index |= ((y >> bit) & 1) << (2*bit)
        index |= ((x >> bit) & 1) << (2*bit+1)
    return index


def crop_texture16(data, box):
    width, height, source = decode_texture16(data)
    x, y, w, h = box
    if (data[9] or min(x,y) < 0 or x % 2 or y % 2 or
            any(n < 8 or n & (n-1) for n in (w,h)) or
            x+w > width or y+h > height):
        raise ValueError("invalid aligned base-level texture crop")
    if box == (0,0,width,height):
        return data
    mode = struct.unpack_from("<I", data, 16)[0]
    if mode & (1 << 30):
        index_start = 32 + (data[10]+1)*8
        out = bytearray(data[:index_start])
        indices = bytearray(w*h//4)
        for by in range(h//2):
            for bx in range(w//2):
                indices[block_index(bx,by,w//2,h//2)] = data[index_start +
                    block_index(bx+x//2,by+y//2,width//2,height//2)]
        out += indices
    else:
        out = bytearray(data[:32]) + bytearray(w*h*2)
        for row in range(h):
            for col in range(w):
                src = 32 + 2*block_index(x+col,y+row,width,height)
                dst = 32 + 2*block_index(col,row,w,h)
                out[dst:dst+2] = data[src:src+2]
    out += bytes(-len(out) % 32)
    mode = (mode & ~63) | ((w.bit_length()-4) << 3) | (h.bit_length()-4)
    struct.pack_into("<I", out, 4, len(out))
    struct.pack_into("<HHI", out, 12, w,h,mode)
    rw, rh, decoded = decode_texture16(out)
    expected = b"".join(source[((y+row)*width+x)*4:((y+row)*width+x+w)*4] for row in range(h))
    if (rw,rh) != (w,h) or decoded != expected:
        raise ValueError("cropped texels differ from original texture")
    return bytes(out)


def crop_vq(data, box):
    """Preserve the strict VQ-only contract for existing callers."""
    if len(data) < 32 or not struct.unpack_from("<I", data, 16)[0] & (1 << 30):
        raise ValueError("expected VQ16 crop source")
    return crop_texture16(data, box)


def prepare_crops(source, prepared, entries, output):
    # entries and original assets must already pass validate_textures.
    bounds = {}
    for fields in rows(source / "material_uv.txt"):
        values = tuple(map(int, fields))
        if len(values) != 9 or values[:5] in bounds:
            raise ValueError("invalid or duplicate material UV bounds")
        bounds[values[:5]] = values[5:]
    keys = {tuple(e["header"]) + (e["luma"],) for e in entries}
    if set(bounds) != keys:
        raise ValueError("UV bounds do not cover source bindings exactly")
    planned = {}
    for entry in entries:
        uv = bounds[tuple(entry["header"]) + (entry["luma"],)]
        x,w = crop_axis(uv[0],uv[2],entry["width"])
        y,h = crop_axis(uv[1],uv[3],entry["height"])
        planned.setdefault(entry["file"], []).append((entry,uv,(x,y,w,h)))
    output.mkdir()  # Never overwrite original or previous candidate assets.
    unique, result, cropped, retained_aliases = {}, [], 0, 0
    for name, group in planned.items():
        original = (prepared / name).read_bytes()
        digest = hashlib.sha256(original).hexdigest()
        if original[9] or any(digest != e["sha256"] for e,_,_ in group):
            raise ValueError("source VQ changed or has unsupported extended metadata")
        original_box = (0,0,group[0][0]["width"],group[0][0]["height"])
        candidates = {box: original if box == original_box else crop_texture16(original,box)
                      for box in {box for _,_,box in group}}
        # A light alias may already share the full texture. Do not replace it
        # with several crops that together cost more. File size = payload +
        # the measured 32-byte/resource allocator overhead used by the budget.
        # ponytail: choose per original resource, not a global packing search;
        # revisit only if measured cross-resource sharing warrants that work.
        retain = sum(map(len,set(candidates.values()))) >= len(original)
        retained_aliases += retain and len(candidates) > 1
        for entry, uv, box in group:
            if retain:
                box = (0,0,entry["width"],entry["height"])
            data = original if retain else candidates[box]
            cropped += box != (0,0,entry["width"],entry["height"])
            if data not in unique:
                filename = f"texture_{len(unique)}.dt"
                unique[data] = filename
                (output / filename).write_bytes(data)
            result.append(dict(entry, file=unique[data], width=box[2], height=box[3],
                sha256=hashlib.sha256(data).hexdigest(), vram_payload_bytes=len(data)-32,
                original_file=entry["file"], original_size=[entry["width"],entry["height"]],
                crop_origin=list(box[:2]), source_uv_bounds=list(uv)))
    report = dict(coverage="offline base-level interior crop candidates; native UV rebasing/sampling not integrated or verified",
        texel_check="exact decoded VQ/raw16 texels including alpha; existing VQ loss remains",
        bindings=len(result), cropped_bindings=cropped, textures=len(unique),
        retained_alias_groups=retained_aliases,
        vram_payload_bytes=sum(len(data)-32 for data in unique), textures_metadata=result)
    (output / "crops.json").write_text(json.dumps(report, indent=2)+"\n")
    return result, {k:v for k,v in report.items() if k != "textures_metadata"}


def validate_crops(source, prepared, entries, cropped):
    """Re-prove candidate bytes and UV coverage, not just their stored hashes."""
    originals = {tuple(e["header"])+(e["luma"],): e for e in entries}
    bounds = {}
    for fields in rows(source / "material_uv.txt"):
        values = tuple(map(int, fields))
        if len(values) != 9 or values[:5] in bounds:
            raise ValueError("invalid or duplicate source UV bounds")
        bounds[values[:5]] = values[5:]
    if bounds.keys() != originals.keys():
        raise ValueError("incomplete source UV coverage")
    result, seen, payloads, checked = [], set(), {}, {}
    for e in json.loads((cropped / "crops.json").read_text())["textures_metadata"]:
        key = tuple(e["header"])+(e["luma"],)
        if key not in originals or key in seen or Path(e["file"]).name != e["file"]:
            raise ValueError("invalid crop binding or filename")
        seen.add(key)
        original = originals[key]
        if e["original_file"] != original["file"] or e["original_size"] != [original["width"],original["height"]]:
            raise ValueError("crop source identity changed")
        box = (*e["crop_origin"],e["width"],e["height"])
        if len(box) != 4 or not all(type(v) is int for v in box):
            raise ValueError("invalid crop rectangle")
        for a,size in enumerate((original["width"],original["height"])):
            low,high = bounds[key][a],bounds[key][a+2]
            crop_axis(low,high,size)  # Validate the source interval too.
            if box[a+2] != size and not (box[a] <= low//8-1 and high//8+1 < box[a]+box[a+2]):
                raise ValueError("crop lacks source UV halo coverage")
        identity = (original["file"],box)
        if identity not in checked:
            data = (prepared / original["file"]).read_bytes()
            if hashlib.sha256(data).hexdigest() != original["sha256"]:
                raise ValueError("original VQ changed")
            checked[identity] = crop_texture16(data,box)
        data = (cropped / e["file"]).read_bytes()
        if data != checked[identity] or hashlib.sha256(data).hexdigest() != e["sha256"]:
            raise ValueError("candidate is not an exact crop of original VQ")
        payloads[e["file"]] = len(data)-32
        result.append(dict(original, file=e["file"], width=box[2], height=box[3],
                           crop_origin=list(box[:2]), sha256=e["sha256"]))
    if seen != originals.keys():
        raise ValueError("crop bindings do not cover original materials")
    return result, sum(payloads.values())


if __name__ == "__main__":
    from prepare_materials import validate_textures
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("prepared", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    _, entries = validate_textures(args.source, args.prepared)
    _, report = prepare_crops(args.source, args.prepared, entries, args.output)
    print(json.dumps(report, indent=2))
